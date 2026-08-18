import test from "node:test";
import assert from "node:assert/strict";

import { buildLegacyMigration, TOPICS } from "../src/index.js";

test("keeps raw gateway topics separate from calibrated panel topics", () => {
  assert.equal(TOPICS.rawHeartbeat, "mailbox/heartbeat");
  assert.equal(TOPICS.rawStatus, "mailbox/status");
  assert.equal(TOPICS.calibratedHeartbeat, "mailbox/heartbeat-calibrated");
  assert.equal(TOPICS.calibratedStatus, "mailbox/status-calibrated");
  assert.notEqual(TOPICS.rawHeartbeat, TOPICS.calibratedHeartbeat);
  assert.notEqual(TOPICS.rawStatus, TOPICS.calibratedStatus);
});

test("migrates a retained calibrated payload from the legacy shared topics", () => {
  const heartbeat = {
    ts: 123,
    mv: 3760,
    pct: 51,
    raw_mv: 3594,
    raw_pct: 19,
    low: false,
    battery_alert: "ok",
    calibrated: true,
    calibration: "cloud-offset-median-v1",
    calibration_offset_mv: 164,
    filtered_samples: 5,
  };
  const migration = buildLegacyMigration({
    heartbeat,
    status: { state: "ARMED", battery_mv: 3760, battery_pct: 51, battery_calibrated: true },
    calibratedHeartbeat: null,
    calibratedStatus: null,
  });

  assert.deepEqual(migration[TOPICS.calibratedHeartbeat], heartbeat);
  assert.equal(migration[TOPICS.calibratedStatus].battery_mv, 3760);
  assert.equal(migration[TOPICS.calibratedStatus].battery_calibrated, true);
  assert.equal(migration[TOPICS.rawHeartbeat], undefined);
  assert.equal(migration[TOPICS.rawStatus], undefined);
});

test("does not repeat a completed legacy topic migration", () => {
  const heartbeat = {
    mv: 3760,
    pct: 51,
    calibrated: true,
    calibration: "cloud-offset-median-v1",
  };
  const calibratedStatus = { battery_calibrated: true };
  assert.equal(buildLegacyMigration({
    heartbeat,
    status: calibratedStatus,
    calibratedHeartbeat: heartbeat,
    calibratedStatus,
  }), null);
});
