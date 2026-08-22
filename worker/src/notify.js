/**
 * Re-auth nagging via ntfy.sh (free, no signup: subscribe to the topic in the
 * ntfy phone app). Monzo's ~90-day SCA lapse gives no warning — without a nag
 * the gadget would just quietly show stale data until someone investigates.
 */

import { getStatus, updateStatus } from "./monzo.js";

const NAG_INTERVAL_MS = 24 * 60 * 60 * 1000; // at most one push per day

export async function nagIfReauthNeeded(env) {
  if (!env.NTFY_TOPIC) return; // notifications not configured; that's fine

  const status = await getStatus(env);
  if (!status.needsReauth) return;
  if (status.lastNagAt && Date.now() - status.lastNagAt < NAG_INTERVAL_MS) return;

  await updateStatus(env, { lastNagAt: Date.now() }); // set first: a failed
  // send shouldn't retry every 15 min and spam if ntfy is flaky

  // Guarded: a nag failure must never fail the cron that detected the
  // problem, and a misconfigured topic should at least surface in /status.
  try {
    const res = await fetch(`https://ntfy.sh/${encodeURIComponent(env.NTFY_TOPIC)}`, {
      method: "POST",
      headers: { Title: "Balance gadget needs re-approval", Priority: "default" },
      body:
        "Monzo access has lapsed (90-day SCA). Open Monzo → Settings → " +
        "Privacy & Security → Manage apps → re-approve the balance app. " +
        "If it was fully revoked, re-run /oauth/start instead.",
    });
    if (!res.ok) {
      await updateStatus(env, { lastError: `ntfy send failed: ${res.status}` });
    }
  } catch (e) {
    await updateStatus(env, { lastError: `ntfy send failed: ${e}` });
  }
}
