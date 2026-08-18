import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const panel = await readFile(new URL("../src/index.html", import.meta.url), "utf8");
const worker = await readFile(new URL("../src/worker.js", import.meta.url), "utf8");

test("serves the status panel in Simplified Chinese", () => {
  assert.match(panel, /<html lang="zh-CN">/);
  assert.match(panel, /LoRa 邮箱/);
  assert.match(panel, /电池数据来源/);
  assert.match(panel, /云端校准/);
  assert.match(panel, /已连接/);
  assert.match(panel, /标记为已取件/);
});

test("serves the passkey login flow in Simplified Chinese", () => {
  assert.match(worker, /<html lang="zh-CN">/);
  assert.match(worker, /邮箱监控登录/);
  assert.match(worker, /使用通行密钥登录/);
  assert.match(worker, /通行密钥注册成功/);
});
