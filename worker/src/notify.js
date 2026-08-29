/**
 * Re-auth nagging. Monzo's ~90-day SCA lapse gives no warning — without a
 * nag the gadget would just quietly show stale data until someone
 * investigates.
 *
 * Transports (both optional; both fire if both configured):
 *  - Email via Brevo: BREVO_API_KEY + NAG_FROM (a sender verified in the
 *    Brevo account) + NAG_EMAIL (recipient).
 *  - Push via ntfy.sh: NTFY_TOPIC, subscribed to in the free ntfy phone app.
 *
 * Contract: this function never throws (a nag problem must not fail the
 * cron that detected the lapse) and every failure — including partial
 * email configuration — lands in status.lastError so /status shows it.
 */

import { getStatus, updateStatus } from "./monzo.js";

const NAG_INTERVAL_MS = 24 * 60 * 60 * 1000; // at most one nag per day

const NAG_TITLE = "Balance gadget needs re-approval";
const NAG_TEXT =
  "Monzo access has lapsed (90-day SCA). Open Monzo → Settings → " +
  "Privacy & Security → Manage apps → re-approve the balance app. " +
  "If it was fully revoked, re-run /oauth/start instead.";

export async function nagIfReauthNeeded(env) {
  try {
    const emailReady = env.BREVO_API_KEY && env.NAG_FROM && env.NAG_EMAIL;
    const emailPartial =
      !emailReady && (env.BREVO_API_KEY || env.NAG_FROM || env.NAG_EMAIL);
    const pushReady = env.NTFY_TOPIC;

    if (emailPartial) {
      // Surface immediately (not only during a lapse): this is a setup
      // mistake that would otherwise silently disable the nag.
      const msg =
        "email nag half-configured: BREVO_API_KEY, NAG_FROM and NAG_EMAIL are all required";
      const status = await getStatus(env);
      if (status.lastError !== msg) await updateStatus(env, { lastError: msg });
    }
    if (!emailReady && !pushReady) return;

    const status = await getStatus(env);
    if (!status.needsReauth) return;
    if (status.lastNagAt && Date.now() - status.lastNagAt < NAG_INTERVAL_MS) return;

    await updateStatus(env, { lastNagAt: Date.now() }); // set first: a failed
    // send shouldn't retry every 15 min and spam if a transport is flaky

    const errors = [];
    if (emailReady) {
      const err = await send("nag email", "https://api.brevo.com/v3/smtp/email", {
        method: "POST",
        headers: { "api-key": env.BREVO_API_KEY, "content-type": "application/json" },
        body: JSON.stringify({
          sender: { name: "Balance gadget", email: env.NAG_FROM },
          to: [{ email: env.NAG_EMAIL }],
          subject: NAG_TITLE,
          textContent: NAG_TEXT,
        }),
      });
      if (err) errors.push(err);
    }
    if (pushReady) {
      const err = await send("ntfy push", `https://ntfy.sh/${encodeURIComponent(env.NTFY_TOPIC)}`, {
        method: "POST",
        headers: { Title: NAG_TITLE, Priority: "default" },
        body: NAG_TEXT,
      });
      if (err) errors.push(err);
    }
    if (errors.length) await updateStatus(env, { lastError: errors.join("; ") });
  } catch (e) {
    // Last-ditch: even a KV failure inside the handlers must not reach the
    // cron. Try once to record it, then swallow.
    try {
      await updateStatus(env, { lastError: `nag: ${e}` });
    } catch {}
  }
}

/** One guarded send. Returns an error string (with a truncated response
 *  body, so a 403 is distinguishable from a bad key) or null on success. */
async function send(label, url, init) {
  try {
    const res = await fetch(url, init);
    if (!res.ok) {
      const body = (await res.text()).slice(0, 120);
      return `${label} failed: ${res.status} ${body}`;
    }
    return null;
  } catch (e) {
    return `${label} failed: ${e}`;
  }
}
