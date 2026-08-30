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
    ["Toy Shop", "-£4.99", ""]
  );
  // Real shape of a pocket-money transfer on an Under-16s account:
  // merchant is null, counterparty carries the human name, notes carries
  // the sender's (editable) message.
  assert.deepEqual(
    txRow({
      amount: 200,
      merchant: null,
      counterparty: { name: "Dad" },
      description: "Dad",
      notes: "💰 Pocket money",
    }),
    ["Dad", "+£2.00", "💰 Pocket money"]
  );
  assert.deepEqual(
    txRow({ amount: -175, description: "Bus travel\nref 42" }),
    ["Bus travel", "-£1.75", ""]
  );
});

test("txRow notes are first-line only and width-unit truncated", () => {
  const [, , note] = txRow({
    amount: 100,
    description: "Dad",
    notes: "line one\nline two",
  });
  assert.equal(note, "line one");

  // Greek glyphs are double-width in the device's efont: 6 chars = 12 units,
  // fits whole; 20 chars = 40 units, cut at 25 units = 12 chars + ellipsis.
  const [, , greek] = txRow({ amount: 100, description: "Dad", notes: "Μπράβο" });
  assert.equal(greek, "Μπράβο");
  const [, , longGreek] = txRow({
    amount: 100,
    description: "Dad",
    notes: "α".repeat(20),
  });
  assert.equal(longGreek, "α".repeat(12) + "…");

  // Emoji also count 2 units and never split surrogate pairs.
  const [, , long] = txRow({
    amount: 100,
    description: "Dad",
    notes: "🎉".repeat(40),
  });
  assert.equal([...long].length, 13); // 12 emoji (24 units) + ellipsis
  assert.ok(long.endsWith("…"));

  // 28 ASCII chars = 28 units: exactly fits, untouched.
  const [, , ascii] = txRow({ amount: 100, description: "Dad", notes: "x".repeat(28) });
  assert.equal(ascii, "x".repeat(28));
});

test("txRow truncates long names to 16 chars with ellipsis", () => {
  const [name] = txRow({
    amount: -100,
    merchant: { name: "An Extremely Long Merchant Name Ltd" },
  });
  assert.equal(name.length, 16);
  assert.ok(name.endsWith("…"));
});
