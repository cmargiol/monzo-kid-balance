import { test } from "node:test";
import assert from "node:assert/strict";
import { formatPence, formatToday, formatTime, txRow } from "../src/format.js";

test("formatPence", () => {
  assert.equal(formatPence(1234), "£12.34");
  assert.equal(formatPence(0), "£0.00");
  assert.equal(formatPence(5), "£0.05");
  assert.equal(formatPence(-499), "-£4.99");
  assert.equal(formatPence(100000), "£1000.00");
});

test("formatToday", () => {
  assert.equal(formatToday(0), "Nothing spent today");
  assert.equal(formatToday(undefined), "Nothing spent today");
  assert.equal(formatToday(-120), "£1.20 spent today");
});

test("formatTime is Europe/London HH:MM", () => {
  // 2026-01-15T14:02Z is winter (GMT): London == UTC.
  assert.equal(formatTime(new Date("2026-01-15T14:02:00Z")), "14:02");
  // 2026-07-15T14:02Z is summer (BST): London == UTC+1.
  assert.equal(formatTime(new Date("2026-07-15T14:02:00Z")), "15:02");
});

test("txRow prefers merchant, falls back to counterparty then description", () => {
  assert.deepEqual(
    txRow({ amount: -499, merchant: { name: "Toy Shop" } }),
    ["Toy Shop", "-£4.99"]
  );
  // Real shape of a pocket-money transfer on an Under-16s account:
  // merchant is null, counterparty carries the human name.
  assert.deepEqual(
    txRow({ amount: 200, merchant: null, counterparty: { name: "Dad" }, description: "Dad" }),
    ["Dad", "+£2.00"]
  );
  assert.deepEqual(
    txRow({ amount: -175, description: "Bus travel\nref 42" }),
    ["Bus travel", "-£1.75"]
  );
});

test("txRow truncates long names to 16 chars with ellipsis", () => {
  const [name] = txRow({
    amount: -100,
    merchant: { name: "An Extremely Long Merchant Name Ltd" },
  });
  assert.equal(name.length, 16);
  assert.ok(name.endsWith("…"));
});
