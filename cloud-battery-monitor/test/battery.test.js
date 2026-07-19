import test from "node:test";
import assert from "node:assert/strict";

import {
  DEFAULT_BATTERY_CONFIG,
  calibrateVoltage,
  linearPercent,
  markAlertSent,
  median,
  processTelemetry,
} from "../src/battery.js";

const DEPLOYMENT_CONFIG = Object.freeze({
  ...DEFAULT_BATTERY_CONFIG,
  offsetMv: 164,
});

function heartbeat(mv, ts, pct = 85) {
  return {
    ts,
    mv,
    pct,
    low: false,
    battery_alert: "ok",
    state: "ARMED",
  };
}

test("calibrates the observed full-battery reading", () => {
  assert.equal(calibrateVoltage(4051, DEPLOYMENT_CONFIG), 4215);
  assert.equal(calibrateVoltage(4060, DEPLOYMENT_CONFIG), 4220);
});

test("does not assume a deployment-specific offset by default", () => {
  assert.equal(calibrateVoltage(4051), 4051);
});

test("uses the requested 3.3V to 4.2V linear percentage", () => {
  assert.equal(linearPercent(3300), 0);
  assert.equal(linearPercent(4050), 83);
  assert.equal(linearPercent(4200), 100);
  assert.equal(linearPercent(4220), 100);
});

test("median rejects a single fast voltage drop", () => {
  assert.equal(median([4215, 4205, 4110, 4214, 4208]), 4208);
});

test("rewrites heartbeat and status with calibrated stable values", () => {
  const result = processTelemetry({
    heartbeat: heartbeat(4051, 1000),
    status: { state: "ARMED", battery_mv: 4051, battery_pct: 85, battery_alert: "ok" },
    state: null,
    nowSec: 1010,
    config: DEPLOYMENT_CONFIG,
  });
  assert.equal(result.changed, true);
  assert.equal(result.displayMv, 4220);
  assert.equal(result.percent, 100);
  assert.equal(result.heartbeat.raw_mv, 4051);
  assert.equal(result.heartbeat.mv, 4220);
  assert.equal(result.heartbeat.calibrated, true);
  assert.equal(result.status.battery_raw_mv, 4051);
  assert.equal(result.status.battery_mv, 4220);
  assert.equal(result.status.battery_pct, 100);
  assert.equal(result.alertDue, false);
});

test("keeps five samples and smooths 10mV display movement", () => {
  let state = null;
  let result;
  for (const [index, mv] of [4051, 4041, 4050, 4046, 3940, 4048].entries()) {
    result = processTelemetry({
      heartbeat: heartbeat(mv, 2000 + index),
      status: {},
      state,
      nowSec: 3000 + index,
      config: DEPLOYMENT_CONFIG,
    });
    state = result.state;
  }
  assert.equal(state.samples.length, DEFAULT_BATTERY_CONFIG.sampleCount);
  assert.equal(result.displayMv, 4210);
  assert.equal(result.percent, 100);
});

test("triggers calibrated low and critical levels with cooldown", () => {
  const low = processTelemetry({
    heartbeat: heartbeat(3316, 4000, 0),
    status: {},
    state: null,
    nowSec: 4001,
    config: DEPLOYMENT_CONFIG,
  });
  assert.equal(low.displayMv, 3480);
  assert.equal(low.percent, 20);
  assert.equal(low.level, "low");
  assert.equal(low.alertDue, true);

  const alertedState = markAlertSent(low.state, low.level, 4001);
  const repeatedLow = processTelemetry({
    heartbeat: heartbeat(3310, 4002, 0),
    status: {},
    state: alertedState,
    nowSec: 4010,
    config: DEPLOYMENT_CONFIG,
  });
  assert.equal(repeatedLow.alertDue, false);

  let criticalState = repeatedLow.state;
  let critical;
  for (const [index, mv] of [3000, 3000, 3000].entries()) {
    critical = processTelemetry({
      heartbeat: heartbeat(mv, 4003 + index, 0),
      status: {},
      state: criticalState,
      nowSec: 4020 + index,
      config: DEPLOYMENT_CONFIG,
    });
    criticalState = critical.state;
  }
  assert.equal(critical.level, "critical");
  assert.equal(critical.alertDue, true);
});

test("does not loop on its own calibrated retained heartbeat", () => {
  const result = processTelemetry({
    heartbeat: { ...heartbeat(4220, 5000, 100), calibrated: true },
    status: {},
    state: null,
    nowSec: 5001,
  });
  assert.deepEqual(result, { changed: false, reason: "already-calibrated" });
});
