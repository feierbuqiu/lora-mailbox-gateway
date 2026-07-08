// Passkey-authenticated Cloudflare Pages worker for the mailbox panel.
//
// Required bindings:
//   AUTH_KV            KV namespace for credentials, challenges, sessions, and rate limits
//   ASSETS             Cloudflare Pages static asset binding
//
// Required environment variables:
//   RP_ID
//   PANEL_MQTT_URL
//   PANEL_MQTT_USERNAME
//   PANEL_MQTT_PASSWORD
//
// Optional environment variables:
//   SETUP_TOKEN        enable first-device passkey registration while present
//   PANEL_TOPIC_PREFIX default: mailbox
//   PANEL_STALE_MINUTES default: 12
//   HC_API_KEY, HC_CHECK_UUID for maintenance mode

import {
  generateRegistrationOptions,
  verifyRegistrationResponse,
  generateAuthenticationOptions,
  verifyAuthenticationResponse,
} from "@simplewebauthn/server";

const SESSION_TTL = 60 * 60 * 24 * 30;
const COOKIE = "panel_session";

function b64urlFromBytes(bytes) {
  let s = "";
  for (const b of new Uint8Array(bytes)) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function bytesFromB64url(s) {
  s = s.replace(/-/g, "+").replace(/_/g, "/");
  while (s.length % 4) s += "=";
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

function getCookie(request, name) {
  const header = request.headers.get("Cookie") || "";
  for (const part of header.split(";")) {
    const [key, ...value] = part.trim().split("=");
    if (key === name) return value.join("=");
  }
  return null;
}

function json(data, status = 200, extraHeaders = {}) {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "Content-Type": "application/json",
      "Cache-Control": "no-store",
      "X-Robots-Tag": "noindex, nofollow",
      ...extraHeaders,
    },
  });
}

async function loadCredentials(env) {
  const raw = await env.AUTH_KV.get("credentials");
  return raw ? JSON.parse(raw) : [];
}

async function hasSession(request, env) {
  const token = getCookie(request, COOKIE);
  if (!token) return false;
  return (await env.AUTH_KV.get("session:" + token)) !== null;
}

async function createSession(env) {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  const token = b64urlFromBytes(bytes);
  await env.AUTH_KV.put("session:" + token, "1", { expirationTtl: SESSION_TTL });
  return token;
}

function sessionCookie(token, maxAge) {
  return `${COOKIE}=${token}; Path=/; Max-Age=${maxAge}; HttpOnly; Secure; SameSite=Lax`;
}

async function rateLimitOk(env, request, limit) {
  const ip = request.headers.get("CF-Connecting-IP") || "unknown";
  const bucket = Math.floor(Date.now() / 600000);
  const key = `rl:${ip}:${bucket}`;
  try {
    const current = parseInt((await env.AUTH_KV.get(key)) || "0", 10);
    if (current >= limit) return false;
    await env.AUTH_KV.put(key, String(current + 1), { expirationTtl: 1200 });
  } catch {
    return true;
  }
  return true;
}

function extractRegistrationCredential(info) {
  if (info.credential) {
    return {
      id: info.credential.id,
      publicKey: b64urlFromBytes(info.credential.publicKey),
      counter: info.credential.counter || 0,
      transports: info.credential.transports || [],
    };
  }
  return {
    id: typeof info.credentialID === "string" ? info.credentialID : b64urlFromBytes(info.credentialID),
    publicKey: b64urlFromBytes(info.credentialPublicKey),
    counter: info.counter || 0,
    transports: [],
  };
}

function loginHtml(showRegistration) {
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex">
<title>Mailbox Panel Login</title>
<style>
:root { --bg:#f5f6f8; --surface:#fff; --text:#1a1d21; --muted:#667085; --border:#d9dee7; --primary:#2563eb; }
@media (prefers-color-scheme: dark) { :root { --bg:#111418; --surface:#1c2128; --text:#e6e8ea; --muted:#9aa3ad; --border:#303846; } }
* { box-sizing:border-box; }
body { margin:0; min-height:100vh; display:flex; align-items:center; justify-content:center; background:var(--bg); color:var(--text); font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; }
main { width:min(420px, 92vw); background:var(--surface); border:1px solid var(--border); border-radius:8px; padding:28px; }
h1 { font-size:20px; margin:0 0 6px; }
p { margin:0 0 22px; color:var(--muted); font-size:14px; }
button { width:100%; border:0; border-radius:6px; padding:13px 14px; font-size:15px; font-weight:650; color:#fff; background:var(--primary); cursor:pointer; }
details { margin-top:22px; }
summary { color:var(--muted); cursor:pointer; font-size:13px; }
input { width:100%; margin:12px 0; padding:10px; border-radius:6px; border:1px solid var(--border); background:var(--bg); color:var(--text); }
#msg { min-height:18px; margin-top:14px; font-size:13px; color:var(--muted); }
</style>
</head>
<body>
<main>
  <h1>Mailbox Panel</h1>
  <p>Use a registered passkey to access the live mailbox controls.</p>
  <button id="loginBtn">Sign in with passkey</button>
  <div id="msg"></div>
  ${showRegistration ? `<details>
    <summary>Register this device</summary>
    <input id="setupToken" type="password" autocomplete="one-time-code" placeholder="Setup token">
    <button id="regBtn" type="button">Register passkey</button>
  </details>` : ""}
</main>
<script src="https://unpkg.com/@simplewebauthn/browser@11/dist/bundle/index.umd.min.js"></script>
<script>
"use strict";
const msg = (text) => { document.getElementById("msg").textContent = text; };
async function post(url, body) {
  const response = await fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.error || "HTTP " + response.status);
  return data;
}
document.getElementById("loginBtn").addEventListener("click", async () => {
  try {
    msg("Preparing passkey challenge...");
    const options = await post("/auth/login-options");
    const resp = await SimpleWebAuthnBrowser.startAuthentication({ optionsJSON: options });
    await post("/auth/login-verify", resp);
    location.href = "/";
  } catch (error) {
    msg("Sign-in failed: " + error.message);
  }
});
const regBtn = document.getElementById("regBtn");
if (regBtn) regBtn.addEventListener("click", async () => {
  try {
    const token = document.getElementById("setupToken").value.trim();
    if (!token) { msg("Enter the setup token first."); return; }
    const options = await post("/auth/reg-options", { token });
    const resp = await SimpleWebAuthnBrowser.startRegistration({ optionsJSON: options });
    await post("/auth/reg-verify", { token, resp });
    msg("Passkey registered. You can sign in now.");
  } catch (error) {
    msg("Registration failed: " + error.message);
  }
});
</script>
</body>
</html>`;
}

function panelConfig(env) {
  const missing = ["PANEL_MQTT_URL", "PANEL_MQTT_USERNAME", "PANEL_MQTT_PASSWORD"].filter((name) => !env[name]);
  if (missing.length) {
    return { error: `missing environment variables: ${missing.join(", ")}` };
  }
  const topicPrefix = (env.PANEL_TOPIC_PREFIX || "mailbox").replace(/\/+$/, "");
  return {
    mqttUrl: env.PANEL_MQTT_URL,
    mqttUsername: env.PANEL_MQTT_USERNAME,
    mqttPassword: env.PANEL_MQTT_PASSWORD,
    topicPrefix,
    staleMinutes: Number(env.PANEL_STALE_MINUTES || 12),
  };
}

async function handleMaintenance(request, env) {
  if (!env.HC_API_KEY || !env.HC_CHECK_UUID) {
    return json({ error: "healthchecks is not configured" }, 500);
  }
  const headers = { "X-Api-Key": env.HC_API_KEY };
  const base = "https://healthchecks.io/api/v3/checks/" + env.HC_CHECK_UUID;
  if (request.method === "GET") {
    const response = await fetch(base, { headers });
    if (!response.ok) return json({ error: "healthchecks API " + response.status }, 502);
    const check = await response.json();
    return json({ status: check.status, last_ping: check.last_ping });
  }
  if (request.method === "POST") {
    const response = await fetch(base + "/pause", { method: "POST", headers });
    if (!response.ok) return json({ error: "healthchecks API " + response.status }, 502);
    return json({ ok: true, status: "paused" });
  }
  return json({ error: "method not allowed" }, 405);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const rpID = env.RP_ID || url.hostname;
    const origin = "https://" + rpID;

    if (env.RP_ID && url.hostname !== rpID) {
      return Response.redirect(origin + url.pathname + url.search, 302);
    }

    if (url.pathname === "/robots.txt") {
      return new Response("User-agent: *\nDisallow: /\n", {
        headers: { "Content-Type": "text/plain", "X-Robots-Tag": "noindex, nofollow" },
      });
    }

    if (url.pathname.startsWith("/auth/") && request.method === "POST") {
      if (!(await rateLimitOk(env, request, 15))) {
        return json({ error: "too many requests" }, 429, { "Retry-After": "600" });
      }
    }

    if (url.pathname === "/auth/reg-options" && request.method === "POST") {
      if (!env.SETUP_TOKEN) return json({ error: "not found" }, 404);
      const body = await request.json().catch(() => ({}));
      if (body.token !== env.SETUP_TOKEN) return json({ error: "invalid setup token" }, 403);
      const existing = await loadCredentials(env);
      const options = await generateRegistrationOptions({
        rpName: "Mailbox Panel",
        rpID,
        userName: "mailbox-owner",
        attestationType: "none",
        excludeCredentials: existing.map((cred) => ({ id: cred.id, transports: cred.transports })),
        authenticatorSelection: { residentKey: "required", userVerification: "preferred" },
      });
      await env.AUTH_KV.put("challenge:reg", options.challenge, { expirationTtl: 300 });
      return json(options);
    }

    if (url.pathname === "/auth/reg-verify" && request.method === "POST") {
      if (!env.SETUP_TOKEN) return json({ error: "not found" }, 404);
      const body = await request.json().catch(() => ({}));
      if (body.token !== env.SETUP_TOKEN) return json({ error: "invalid setup token" }, 403);
      const expectedChallenge = await env.AUTH_KV.get("challenge:reg");
      if (!expectedChallenge) return json({ error: "challenge expired" }, 400);
      let verification;
      try {
        verification = await verifyRegistrationResponse({
          response: body.resp,
          expectedChallenge,
          expectedOrigin: origin,
          expectedRPID: rpID,
          requireUserVerification: false,
        });
      } catch (error) {
        return json({ error: "verification failed: " + error.message }, 400);
      }
      if (!verification.verified) return json({ error: "registration was not verified" }, 400);
      const cred = extractRegistrationCredential(verification.registrationInfo);
      const creds = await loadCredentials(env);
      creds.push(cred);
      await env.AUTH_KV.put("credentials", JSON.stringify(creds));
      await env.AUTH_KV.delete("challenge:reg");
      return json({ ok: true });
    }

    if (url.pathname === "/auth/login-options" && request.method === "POST") {
      const creds = await loadCredentials(env);
      if (creds.length === 0) return json({ error: "no passkey is registered" }, 400);
      const options = await generateAuthenticationOptions({
        rpID,
        userVerification: "preferred",
        allowCredentials: creds.map((cred) => ({ id: cred.id, transports: cred.transports })),
      });
      await env.AUTH_KV.put("challenge:auth", options.challenge, { expirationTtl: 300 });
      return json(options);
    }

    if (url.pathname === "/auth/login-verify" && request.method === "POST") {
      const resp = await request.json().catch(() => ({}));
      const expectedChallenge = await env.AUTH_KV.get("challenge:auth");
      if (!expectedChallenge) return json({ error: "challenge expired" }, 400);
      const creds = await loadCredentials(env);
      const cred = creds.find((item) => item.id === resp.id);
      if (!cred) return json({ error: "unknown credential" }, 400);
      const credential = {
        id: cred.id,
        publicKey: bytesFromB64url(cred.publicKey),
        counter: cred.counter || 0,
        transports: cred.transports || [],
      };
      let verification;
      try {
        verification = await verifyAuthenticationResponse({
          response: resp,
          expectedChallenge,
          expectedOrigin: origin,
          expectedRPID: rpID,
          credential,
          authenticator: {
            credentialID: credential.id,
            credentialPublicKey: credential.publicKey,
            counter: credential.counter,
          },
          requireUserVerification: false,
        });
      } catch (error) {
        return json({ error: "verification failed: " + error.message }, 400);
      }
      if (!verification.verified) return json({ error: "login was not verified" }, 401);
      cred.counter = verification.authenticationInfo?.newCounter ?? cred.counter;
      await env.AUTH_KV.put("credentials", JSON.stringify(creds));
      await env.AUTH_KV.delete("challenge:auth");
      const token = await createSession(env);
      return json({ ok: true }, 200, { "Set-Cookie": sessionCookie(token, SESSION_TTL) });
    }

    if (url.pathname === "/auth/logout") {
      const token = getCookie(request, COOKIE);
      if (token) await env.AUTH_KV.delete("session:" + token);
      return new Response(null, {
        status: 302,
        headers: { Location: "/", "Set-Cookie": sessionCookie("", 0) },
      });
    }

    if (url.pathname.startsWith("/api/")) {
      if (!(await hasSession(request, env))) return json({ error: "unauthorized" }, 401);
      if (url.pathname === "/api/config" && request.method === "GET") {
        const config = panelConfig(env);
        return config.error ? json({ error: config.error }, 500) : json(config);
      }
      if (url.pathname === "/api/maintenance") {
        return handleMaintenance(request, env);
      }
      return json({ error: "not found" }, 404);
    }

    if (await hasSession(request, env)) {
      const response = await env.ASSETS.fetch(request);
      const out = new Response(response.body, response);
      out.headers.set("X-Robots-Tag", "noindex, nofollow");
      return out;
    }

    if (url.pathname === "/" || url.pathname === "/index.html") {
      return new Response(loginHtml(Boolean(env.SETUP_TOKEN)), {
        status: 200,
        headers: {
          "Content-Type": "text/html; charset=utf-8",
          "Cache-Control": "no-store",
          "X-Robots-Tag": "noindex, nofollow",
        },
      });
    }

    return json({ error: "unauthorized" }, 401);
  },
};
