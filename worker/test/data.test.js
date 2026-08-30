import { test } from "node:test";
import assert from "node:assert/strict";
import { markIfStale, refreshDisplayData } from "../src/data.js";

function fakeEnv() {
  const store = new Map();
  return {
    ACCOUNT_ID: "acc_test",
    KV: {
      async get(k) { return store.get(k) ?? null; },
      async put(k, v) { store.set(k, v); },
      async delete(k) { store.delete(k); },
    },
    _store: store,
  };
}

function stubFetch(handler) {
  const original = globalThis.fetch;
  globalThis.fetch = handler;
  return () => { globalThis.fetch = original; };
}

const okJson = (obj) => ({ ok: true, json: async () => obj });
const failure = { ok: false, status: 500, text: async () => "boom" };

const MIN = 60 * 1000;

test("fresh ok payload passes through untouched", () => {
  const p = { state: "ok", balance: "£12.34", built_at: 1000 };
  assert.deepEqual(markIfStale(p, 1000 + 15 * MIN), p);
});

test("ok payload older than 45 min is served as error", () => {
  const p = { state: "ok", balance: "£12.34", built_at: 1000 };
  const marked = markIfStale(p, 1000 + 46 * MIN);
  assert.equal(marked.state, "error");
  assert.equal(marked.balance, "£12.34"); // last-good strings preserved
});

test("non-ok payloads are never rewritten by the staleness check", () => {
  const p = { state: "needs_reauth", balance: "£12.34", built_at: 1000 };
  assert.deepEqual(markIfStale(p, 1000 + 90 * MIN), p);
});

test("payload without built_at is treated as stale, not trusted", () => {
  const p = { state: "ok", balance: "£12.34" };
  assert.equal(markIfStale(p, Date.now()).state, "error");
});

test("successful refresh writes display and clears needsReauth", async () => {
  const env = fakeEnv();
  await env.KV.put("status", JSON.stringify({ needsReauth: true }));
  const restore = stubFetch(async (url) =>
    String(url).includes("/balance")
      ? okJson({ balance: 1234, spend_today: -120 })
      : okJson({ transactions: [{ amount: 200, counterparty: { name: "Dad" } }] })
  );
  try {
    await refreshDisplayData(env, "tok");
  } finally {
    restore();
  }
  const display = JSON.parse(env._store.get("display"));
  assert.equal(display.state, "ok");
  assert.equal(display.balance, "£12.34");
  assert.deepEqual(display.tx, [["Dad", "+£2.00", ""]]);
  assert.ok(display.built_at > 0);
  const status = JSON.parse(env._store.get("status"));
  assert.equal(status.needsReauth, false); // re-approval without rotation heals
  assert.equal(status.lastPollOk, true);
});

test("loud (cron) failure degrades display and records status", async () => {
  const env = fakeEnv();
  await env.KV.put("display", JSON.stringify({ state: "ok", balance: "£9.99", built_at: Date.now() }));
  const restore = stubFetch(async () => failure);
  try {
    await refreshDisplayData(env, "tok");
  } finally {
    restore();
  }
  assert.equal(JSON.parse(env._store.get("display")).state, "error");
  assert.equal(JSON.parse(env._store.get("display")).balance, "£9.99"); // last-good kept
  assert.equal(JSON.parse(env._store.get("status")).lastPollOk, false);
});

test("quiet (webhook) failure touches neither display nor status", async () => {
  const env = fakeEnv();
  const healthy = JSON.stringify({ state: "ok", balance: "£9.99", built_at: Date.now() });
  await env.KV.put("display", healthy);
  const restore = stubFetch(async () => failure);
  try {
    await refreshDisplayData(env, "tok", { quiet: true });
  } finally {
    restore();
  }
  assert.equal(env._store.get("display"), healthy); // byte-identical, untouched
  assert.equal(env._store.get("status"), undefined);
});
