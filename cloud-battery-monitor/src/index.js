import {
  DEFAULT_BATTERY_CONFIG,
  markAlertSent,
  processTelemetry,
} from "./battery.js";
import { publishRetainedTopics, readRetainedTopics } from "./mqtt.js";

const TOPIC_HEARTBEAT = "mailbox/heartbeat";
const TOPIC_STATUS = "mailbox/status";
const TOPIC_STATE = "mailbox/battery-cloud-state";

function numberEnv(value, fallback) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function configFromEnv(env) {
  return {
    ...DEFAULT_BATTERY_CONFIG,
    offsetMv: numberEnv(env.BATTERY_OFFSET_MV, DEFAULT_BATTERY_CONFIG.offsetMv),
    emptyMv: numberEnv(env.BATTERY_EMPTY_MV, DEFAULT_BATTERY_CONFIG.emptyMv),
    fullMv: numberEnv(env.BATTERY_FULL_MV, DEFAULT_BATTERY_CONFIG.fullMv),
    maxMv: numberEnv(env.BATTERY_MAX_MV, DEFAULT_BATTERY_CONFIG.maxMv),
  };
}

function mqttOptions(env, suffix) {
  if (!env.MQTT_WSS_URL || !env.MQTT_USERNAME || !env.MQTT_PASSWORD) {
    throw new Error("MQTT_WSS_URL, MQTT_USERNAME and MQTT_PASSWORD are required");
  }
  return {
    url: env.MQTT_WSS_URL,
    username: env.MQTT_USERNAME,
    password: env.MQTT_PASSWORD,
    clientId: `battery-monitor-${suffix}-${crypto.randomUUID().slice(0, 8)}`,
  };
}

function parseJsonMessage(messages, topic) {
  const message = messages[topic];
  if (!message) return null;
  try {
    return JSON.parse(message.payload);
  } catch {
    throw new Error(`Retained MQTT topic ${topic} is not valid JSON`);
  }
}

export function buildBatteryAlert(result) {
  const critical = result.level === "critical";
  const subject = critical
    ? "Mailbox: calibrated battery critical"
    : "Mailbox: calibrated battery low";
  const body = [
    critical
      ? "\u4e91\u7aef\u6821\u51c6\u540e\u7684\u4fe1\u7bb1\u7535\u6c60\u5df2\u8fdb\u5165\u4e25\u91cd\u4f4e\u7535\u91cf\u3002"
      : "\u4e91\u7aef\u6821\u51c6\u540e\u7684\u4fe1\u7bb1\u7535\u6c60\u5df2\u8fdb\u5165\u4f4e\u7535\u91cf\u3002",
    `\u6821\u51c6\u5e76\u5e73\u6ed1\u540e\u7684\u7535\u538b\uff1a${(result.displayMv / 1000).toFixed(2)}V`,
    `\u7ebf\u6027\u4f30\u7b97\u7535\u91cf\uff1a${result.percent}%`,
    `\u8282\u70b9\u539f\u59cb\u8bfb\u6570\uff1a${(result.rawMv / 1000).toFixed(3)}V`,
    `\u4e91\u7aef\u6821\u51c6\u504f\u79fb\uff1a+${result.heartbeat.calibration_offset_mv}mV`,
    critical
      ? "\u8bf7\u5c3d\u5feb\u5b89\u6392\u5145\u7535\u3002"
      : "\u8bf7\u5728\u65b9\u4fbf\u65f6\u51c6\u5907\u5145\u7535\u3002",
  ].join("\n");
  return {
    subject,
    body,
    node: "mail",
    counter: 0,
    source: "cloud-battery-monitor",
  };
}

async function sendBatteryAlert(env, result) {
  if (!env.MAKE_WEBHOOK_URL) throw new Error("MAKE_WEBHOOK_URL is required for battery alerts");
  const response = await fetch(env.MAKE_WEBHOOK_URL, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(buildBatteryAlert(result)),
  });
  if (!response.ok) throw new Error(`Make webhook returned HTTP ${response.status}`);
}

export async function runMonitor(env, nowSec = Math.floor(Date.now() / 1000)) {
  const messages = await readRetainedTopics({
    ...mqttOptions(env, "read"),
    topics: [TOPIC_HEARTBEAT, TOPIC_STATUS, TOPIC_STATE],
    requiredTopic: TOPIC_HEARTBEAT,
  });
  const heartbeat = parseJsonMessage(messages, TOPIC_HEARTBEAT);
  const status = parseJsonMessage(messages, TOPIC_STATUS);
  const state = parseJsonMessage(messages, TOPIC_STATE);
  const result = processTelemetry({
    heartbeat,
    status,
    state,
    nowSec,
    config: configFromEnv(env),
  });

  if (!result.changed) {
    return { ok: true, changed: false, reason: result.reason };
  }

  let nextState = result.state;
  let alertSent = false;
  let alertError = null;
  if (result.alertDue) {
    try {
      await sendBatteryAlert(env, result);
      nextState = markAlertSent(nextState, result.level, nowSec);
      alertSent = true;
    } catch (error) {
      alertError = error instanceof Error ? error.message : String(error);
    }
  }

  await publishRetainedTopics(mqttOptions(env, "write"), {
    [TOPIC_HEARTBEAT]: result.heartbeat,
    [TOPIC_STATUS]: result.status,
    [TOPIC_STATE]: nextState,
  });

  return {
    ok: true,
    changed: true,
    raw_mv: result.rawMv,
    corrected_sample_mv: result.correctedSampleMv,
    displayed_mv: result.displayMv,
    pct: result.percent,
    level: result.level,
    samples: nextState.samples.length,
    alert_due: result.alertDue,
    alert_sent: alertSent,
    alert_error: alertError,
  };
}

function json(data, status = 200) {
  return new Response(JSON.stringify(data, null, 2), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" },
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/health") {
      return json({ ok: true, service: "mailbox-battery-monitor", calibration: "offset+median+linear-percent" });
    }
    if (url.pathname === "/run" && request.method === "POST") {
      const expected = env.MONITOR_ADMIN_TOKEN ? `Bearer ${env.MONITOR_ADMIN_TOKEN}` : null;
      if (!expected || request.headers.get("Authorization") !== expected) {
        return json({ error: "unauthorized" }, 401);
      }
      try {
        return json(await runMonitor(env));
      } catch (error) {
        return json({ ok: false, error: error instanceof Error ? error.message : String(error) }, 500);
      }
    }
    return json({ error: "not found" }, 404);
  },

  async scheduled(_controller, env, ctx) {
    ctx.waitUntil(
      runMonitor(env).then(
        (result) => console.log(JSON.stringify(result)),
        (error) => console.error(error instanceof Error ? error.stack : String(error)),
      ),
    );
  },
};
