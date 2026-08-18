import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const panel = await readFile(new URL("../src/index.html", import.meta.url), "utf8");

test("subscribes to dedicated calibrated topics and prefers their battery fields", () => {
  assert.match(panel, /status-calibrated/);
  assert.match(panel, /heartbeat-calibrated/);
  assert.match(panel, /if \(calibratedHeartbeat && calibratedHeartbeat\.calibrated === true\) return calibratedHeartbeat/);
  assert.match(panel, /云端校准/);
});
