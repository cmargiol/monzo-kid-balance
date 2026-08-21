/**
 * Formatting: Monzo's raw numbers → the exact strings the gadget renders.
 * All money math happens here, server-side; the device just draws strings.
 */

const NAME_MAX = 16; // fits the tx row at the font size the firmware uses

/** 1234 → "£12.34", -499 → "-£4.99" */
export function formatPence(pence) {
  const sign = pence < 0 ? "-" : "";
  const abs = Math.abs(pence);
  const pounds = Math.floor(abs / 100);
  const rem = String(abs % 100).padStart(2, "0");
  return `${sign}£${pounds}.${rem}`;
}

/** Monzo reports spend_today as a negative number of pence (0 if nothing). */
export function formatToday(spendToday) {
  if (!spendToday) return "Nothing spent today";
  return `${formatPence(Math.abs(spendToday))} spent today`;
}

/** "HH:MM" in Europe/London regardless of where the Worker runs. */
export function formatTime(date = new Date()) {
  return new Intl.DateTimeFormat("en-GB", {
    timeZone: "Europe/London",
    hour: "2-digit",
    minute: "2-digit",
  }).format(date);
}

/** Transaction → ["Toy Shop", "-£4.99"] with a signed, kid-readable amount. */
export function txRow(tx) {
  const name =
    tx.merchant?.name || tx.counterparty?.name || tidyDescription(tx.description);
  const amount = `${tx.amount < 0 ? "-" : "+"}${formatPence(Math.abs(tx.amount))}`;
  return [truncate(name, NAME_MAX), amount];
}

/** Bank-transfer descriptions are noisy ("FASTER PAYMENT REF 123…"); soften. */
function tidyDescription(desc) {
  if (!desc) return "Something";
  return desc.split("\n")[0].trim();
}

function truncate(s, max) {
  return s.length <= max ? s : s.slice(0, max - 1) + "…";
}
