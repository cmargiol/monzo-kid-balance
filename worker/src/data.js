/**
 * Builds the gadget's display payload from live Monzo data and caches it in KV.
 * Called only from the scheduled (cron) handler and, later, the webhook.
 */

import { monzoGet, updateStatus } from "./monzo.js";
import { formatPence, formatToday, formatTime, txRow } from "./format.js";

const TX_WINDOW_DAYS = 89; // SCA caps transaction history at 90 days
const TX_SHOWN = 3;

/**
 * quiet=true is the webhook path: best-effort freshness only. It updates the
 * display cache on success but NEVER writes `status` and NEVER degrades on
 * failure — status stays single-writer (the cron), so concurrent webhook
 * bursts can't clobber needsReauth or flip a healthy display to "error"
 * over a transient Monzo hiccup. The cron backstop repairs anything within
 * 15 minutes. (Residual accepted race: monzoGet itself sets needsReauth on
 * an SCA-403 even when called from the webhook — that write only ever turns
 * the flag ON, and a later successful cron poll below turns it off again.)
 */
export async function refreshDisplayData(env, accessToken, { quiet = false } = {}) {
  const accountId = env.ACCOUNT_ID; // secret: keeps the id out of the repo
  if (!accountId) {
    if (!quiet) await updateStatus(env, { lastError: "ACCOUNT_ID secret not set" });
    return;
  }

  try {
    const balance = await monzoGet(
      env,
      accessToken,
      `/balance?account_id=${encodeURIComponent(accountId)}`
    );
    const tx = await recentTransactions(env, accessToken, accountId);

    await putDisplay(env, {
      state: "ok",
      balance: formatPence(balance.balance),
      today: formatToday(balance.spend_today),
      updated: formatTime(),
      tx: tx.map(txRow),
    });
    if (!quiet) {
      // A successful data read proves the SCA grant is healthy again, so a
      // re-approval that didn't rotate tokens still clears the nag state.
      await updateStatus(env, { lastPollOk: true, lastError: null, needsReauth: false });
    }
  } catch (e) {
    if (quiet) return;
    await degradeDisplay(env, e.needsReauth ? "needs_reauth" : "error");
    await updateStatus(env, { lastPollOk: false, lastError: String(e) });
  }
}

/**
 * Newest transactions, excluding declines. Monzo returns oldest-first
 * (confirmed against the real account), so we page forward through the
 * 89-day window and keep the tail.
 */
async function recentTransactions(env, accessToken, accountId) {
  const sinceDate = new Date(
    Date.now() - TX_WINDOW_DAYS * 24 * 60 * 60 * 1000
  ).toISOString();

  let since = sinceDate;
  let all = [];
  for (let page = 0; page < 5; page++) {
    const res = await monzoGet(
      env,
      accessToken,
      `/transactions?account_id=${encodeURIComponent(accountId)}` +
        `&limit=100&since=${encodeURIComponent(since)}&expand[]=merchant`
    );
    const batch = res.transactions ?? [];
    all = all.concat(batch);
    if (batch.length < 100) break;
    since = batch[batch.length - 1].id; // object-id cursor for the next page
  }

  return all
    .filter((t) => !t.decline_reason && !t.amount_is_pending)
    .slice(-TX_SHOWN);
}

/**
 * Something went wrong: keep the last-good strings so the gadget can still
 * show a (stale) balance, but flip `state` so it can badge the situation.
 */
async function degradeDisplay(env, state) {
  const current = await loadDisplay(env);
  await putDisplay(env, { ...current, state });
}

// Three missed 15-min cron ticks with no successful rebuild = the payload
// can no longer be trusted as fresh. Detected at serve time so a silently
// dead cron can't keep the gadget looking healthy.
const STALE_AFTER_MS = 45 * 60 * 1000;

export async function loadDisplay(env, now = Date.now()) {
  const stored = await env.KV.get("display");
  if (stored) return markIfStale(JSON.parse(stored), now);
  // Nothing cached yet (fresh deploy): a friendly not-connected payload.
  return {
    state: "error",
    balance: "—",
    today: "Not connected yet",
    updated: "--:--",
    tx: [],
  };
}

export function markIfStale(payload, now) {
  // No stamp = unknown age = untrusted: putDisplay stamps every write, so a
  // missing built_at can only mean a payload that predates the check.
  const age = now - (payload.built_at ?? 0);
  if (payload.state === "ok" && age > STALE_AFTER_MS) {
    return { ...payload, state: "error" };
  }
  return payload;
}

async function putDisplay(env, payload) {
  await env.KV.put("display", JSON.stringify({ ...payload, built_at: Date.now() }));
}
