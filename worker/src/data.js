/**
 * Builds the gadget's display payload from live Monzo data and caches it in KV.
 * Called only from the scheduled (cron) handler and, later, the webhook.
 */

import { monzoGet, updateStatus } from "./monzo.js";
import { formatPence, formatToday, formatTime, txRow } from "./format.js";

const TX_WINDOW_DAYS = 89; // SCA caps transaction history at 90 days
const TX_SHOWN = 3;

export async function refreshDisplayData(env, accessToken) {
  const accountId = env.ACCOUNT_ID; // secret: keeps the id out of the repo
  if (!accountId) {
    await updateStatus(env, { lastError: "ACCOUNT_ID secret not set" });
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
    await updateStatus(env, { lastPollOk: true, lastError: null });
  } catch (e) {
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

export async function loadDisplay(env) {
  const stored = await env.KV.get("display");
  if (stored) return JSON.parse(stored);
  // Nothing cached yet (fresh deploy): a friendly not-connected payload.
  return {
    state: "error",
    balance: "—",
    today: "Not connected yet",
    updated: "--:--",
    tx: [],
  };
}

async function putDisplay(env, payload) {
  await env.KV.put("display", JSON.stringify(payload));
}
