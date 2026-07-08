import http from "node:http";

const upstream = "https://client.meshtastic.org";
const port = Number(process.env.PORT || 8099);

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url || "/", upstream);
    const upstreamRes = await fetch(url, {
      headers: {
        "user-agent": "codex-meshtastic-local-proxy",
        accept: req.headers.accept || "*/*",
      },
      redirect: "follow",
    });

    let body = Buffer.from(await upstreamRes.arrayBuffer());
    const contentType = upstreamRes.headers.get("content-type") || "";

    res.statusCode = upstreamRes.status;
    for (const [key, value] of upstreamRes.headers) {
      if (
        ![
          "content-encoding",
          "content-length",
          "content-security-policy",
          "strict-transport-security",
        ].includes(key.toLowerCase())
      ) {
        res.setHeader(key, value);
      }
    }

    if (contentType.includes("text/html")) {
      body = Buffer.from(
        body
          .toString("utf8")
          .replace(
            /<script async src="https:\/\/cdn-cookieyes\.com\/[^"]+"><\/script>/,
            "",
          )
          .replace(
            /<script id="vite-plugin-pwa:register-sw" src="\/registerSW\.js"><\/script>/,
            "",
          ),
      );
      res.setHeader("content-type", "text/html; charset=utf-8");
    } else if (
      contentType.includes("javascript") ||
      (req.url || "").endsWith(".js")
    ) {
      body = Buffer.from(
        body
          .toString("utf8")
          .replace("async function v1(e,t=2500)", "async function v1(e,t=10000)"),
      );
    }

    res.setHeader("cache-control", "no-store");
    res.end(body);
  } catch (error) {
    res.statusCode = 502;
    res.setHeader("content-type", "text/plain; charset=utf-8");
    res.end(`Proxy error: ${error instanceof Error ? error.message : error}`);
  }
});

server.listen(port, "127.0.0.1", () => {
  console.log(`Meshtastic Web local proxy: http://127.0.0.1:${port}/`);
});
