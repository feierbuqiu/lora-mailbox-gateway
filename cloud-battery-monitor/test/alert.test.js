import test from "node:test";
import assert from "node:assert/strict";

import { buildBatteryAlert } from "../src/index.js";

test("builds a readable Chinese low-battery alert", () => {
  const alert = buildBatteryAlert({
    level: "low",
    displayMv: 3470,
    percent: 19,
    rawMv: 3306,
    heartbeat: { calibration_offset_mv: 164 },
  });

  assert.equal(alert.subject, "Mailbox: calibrated battery low");
  assert.match(alert.body, /\u4e91\u7aef\u6821\u51c6\u540e\u7684\u4fe1\u7bb1\u7535\u6c60\u5df2\u8fdb\u5165\u4f4e\u7535\u91cf/);
  assert.match(alert.body, /3\.47V/);
  assert.match(alert.body, /19%/);
  assert.match(alert.body, /3\.306V/);
  assert.equal(alert.source, "cloud-battery-monitor");
});

test("uses urgent wording for a critical alert", () => {
  const alert = buildBatteryAlert({
    level: "critical",
    displayMv: 3370,
    percent: 8,
    rawMv: 3206,
    heartbeat: { calibration_offset_mv: 164 },
  });

  assert.equal(alert.subject, "Mailbox: calibrated battery critical");
  assert.match(alert.body, /\u4e25\u91cd\u4f4e\u7535\u91cf/);
  assert.match(alert.body, /\u8bf7\u5c3d\u5feb\u5b89\u6392\u5145\u7535/);
});
