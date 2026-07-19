import test from "node:test";
import assert from "node:assert/strict";

import worker from "../src/worker.js";

function fakeKv() {
  const store = new Map();
  return {
    store,
    binding: {
      async get(key) {
        return store.has(key) ? store.get(key) : null;
      },
      async put(key, value) {
        store.set(key, value);
      },
      async delete(key) {
        store.delete(key);
      },
    },
  };
}

test("generates registration options with the supported SimpleWebAuthn API", async () => {
  const { store, binding } = fakeKv();
  const request = new Request("https://mailbox.example.com/auth/reg-options", {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "cf-connecting-ip": "127.0.0.1",
    },
    body: JSON.stringify({ token: "test-setup" }),
  });

  const response = await worker.fetch(request, {
    RP_ID: "mailbox.example.com",
    SETUP_TOKEN: "test-setup",
    AUTH_KV: binding,
  });
  const payload = await response.json();

  assert.equal(response.status, 200);
  assert.equal(payload.rp.id, "mailbox.example.com");
  assert.equal(typeof payload.challenge, "string");
  assert.equal(store.get("challenge:reg"), payload.challenge);
});
