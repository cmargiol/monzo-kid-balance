/**
 * Formatting: Monzo's raw numbers → the exact strings the gadget renders.
 * All money math happens here, server-side; the device just draws strings.
 */

const NAME_MAX = 16; // fits the tx row at the font size the firmware uses
// The device draws the message line in efontCN_16 (the only bundled font
// with Greek): ASCII glyphs are 8px wide, Greek 16px, across 228px = 28
// "units" per line. Truncation is unit-aware; the ellipsis renders as "..."
// (3 units) after the device's sanitiser, so we cut to 25 units + "…".
// Change any link in that chain and re-derive the rest.
const NOTE_UNITS_MAX = 28;
const NOTE_UNITS_CUT = 25;

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

/**
 * Transaction → ["Dad", "+£2.00", "For the school trip"].
 * The third element is the sender's message (Monzo `notes`, editable after
 * sending between Monzo accounts — we forward whatever is current at fetch
 * time), or "" when there is none.
 */
export function txRow(tx) {
  const name =
    tx.merchant?.name || tx.counterparty?.name || tidyDescription(tx.description);
  const amount = `${tx.amount < 0 ? "-" : "+"}${formatPence(Math.abs(tx.amount))}`;
  const note = firstLine(tx.notes ?? "");
  return [truncate(name, NAME_MAX), amount, truncateUnits(note)];
}

/** Bank-transfer descriptions are noisy ("FASTER PAYMENT REF 123…"); soften. */
function tidyDescription(desc) {
  if (!desc) return "Something";
  return firstLine(desc);
}

function firstLine(s) {
  return s.split(/\r?\n/)[0].trim();
}

/** Display-width-aware truncation: ASCII counts 1 unit, everything else 2. */
function truncateUnits(s) {
  let units = 0;
  const chars = [...s];
  for (let i = 0; i < chars.length; i++) {
    units += chars[i].codePointAt(0) < 0x80 ? 1 : 2;
    if (units > NOTE_UNITS_MAX) {
      let cut = [], u = 0;
      for (const c of chars) {
        u += c.codePointAt(0) < 0x80 ? 1 : 2;
        if (u > NOTE_UNITS_CUT) break;
        cut.push(c);
      }
      return cut.join("") + "…";
    }
  }
  return s;
}

function truncate(s, max) {
  // Codepoint-aware: notes often carry emoji, and slicing a surrogate pair
  // in half would send malformed text to the device.
  const chars = [...s];
  return chars.length <= max ? s : chars.slice(0, max - 1).join("") + "…";
}
