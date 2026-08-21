/**
 * monzo-kid-balance Worker.
 *
 * Serves a pre-formatted, read-only "what the gadget should show" payload.
 * The device never talks to Monzo; this Worker is the only thing that does.
 *
 * Routes:
 *   GET /display                 device payload (Bearer DEVICE_TOKEN)
 *   GET /oauth/start?admin=      begin Monzo OAuth (302 to auth.monzo.com)
 *   GET /oauth/callback          OAuth redirect target; stores tokens
 *   GET /admin/accounts?admin=   live /accounts + /pots dump (setup helper)
 *   GET /status?admin=           token/health summary, no secrets
 */

import {
  AUTH_BASE,
  exchangeCode,
  loadTokens,
  monzoGet,
  getStatus,
} from "./monzo.js";

// Payload contract with the firmware. `state` is one of:
//   ok           — show balance/tx as given
//   needs_reauth — show the "ask a grown-up to re-approve" screen
//   error        — show last-good data with a "stale" badge
// Strings are pre-formatted server-side; the device does zero money math.
// Mock until the live-data PR replaces it with the KV-cached payload.
const MOCK_DISPLAY = {
  state: "ok",
  balance: "£12.34",
  today: "£1.20 spent today",
  updated: "12:00",
  tx: [
    ["Toy Shop", "-£4.99"],
    ["Pocket money", "+£2.00"],
    ["Bus", "-£1.75"],
  ],
};

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method !== "GET") return json({ error: "not found" }, 404);

    switch (url.pathname) {
      case "/display":
        if (!(await isDevice(request, env))) {
          return json({ error: "unauthorized" }, 401);
        }
        return json(MOCK_DISPLAY);

      case "/oauth/start":
        if (!isAdmin(url, env)) return json({ error: "unauthorized" }, 401);
        return oauthStart(url, env);

      case "/oauth/callback":
        return oauthCallback(url, env);

      case "/admin/accounts":
        if (!isAdmin(url, env)) return json({ error: "unauthorized" }, 401);
        return adminAccounts(env);

      case "/status":
        if (!isAdmin(url, env)) return json({ error: "unauthorized" }, 401);
        return statusReport(env);

      default:
        return json({ error: "not found" }, 404);
    }
  },
};

// ---------------------------------------------------------------- OAuth

async function oauthStart(url, env) {
  const state = randomToken();
  await env.KV.put("oauth_state", state, { expirationTtl: 600 });
  const auth = new URL(AUTH_BASE);
  auth.searchParams.set("client_id", env.MONZO_CLIENT_ID);
  auth.searchParams.set("redirect_uri", redirectUri(url));
  auth.searchParams.set("response_type", "code");
  auth.searchParams.set("state", state);
  return Response.redirect(auth.toString(), 302);
}

async function oauthCallback(url, env) {
  const state = url.searchParams.get("state") ?? "";
  const code = url.searchParams.get("code") ?? "";
  const expected = await env.KV.get("oauth_state");
  if (!expected || !timingSafeEqual(state, expected)) {
    return html("<h1>State mismatch</h1><p>Restart from /oauth/start.</p>", 403);
  }
  await env.KV.delete("oauth_state");

  try {
    await exchangeCode(env, code, redirectUri(url));
  } catch (e) {
    return html(`<h1>Token exchange failed</h1><pre>${escapeHtml(String(e))}</pre>`, 502);
  }

  return html(`
    <h1>Connected ✅</h1>
    <p>Tokens are stored. Two more steps:</p>
    <ol>
      <li><strong>Open the Monzo app</strong> and approve the access prompt
          (Strong Customer Authentication). Without this every API call
          returns <code>insufficient_permissions</code>.</li>
      <li>Then visit <code>/admin/accounts?admin=…</code> to see which
          accounts and pots this grant can read.</li>
    </ol>`);
}

/** The redirect URI must match the Monzo OAuth client's config exactly. */
function redirectUri(url) {
  return `${url.origin}/oauth/callback`;
}

// ---------------------------------------------------------------- Admin

async function adminAccounts(env) {
  const tokens = await loadTokens(env);
  if (!tokens) {
    return json({ error: "no tokens stored — run /oauth/start first" }, 409);
  }
  if (Date.now() >= tokens.expires_at) {
    // Refreshing here would race the cron writer; freshness is cron's job.
    return json({ error: "access token expired — wait for the next cron refresh" }, 503);
  }
  try {
    const accounts = await monzoGet(env, tokens.access_token, "/accounts");
    const result = { accounts: accounts.accounts };
    for (const acc of accounts.accounts ?? []) {
      const pots = await monzoGet(
        env,
        tokens.access_token,
        `/pots?current_account_id=${encodeURIComponent(acc.id)}`
      );
      result[`pots_for_${acc.id}`] = pots.pots?.filter((p) => !p.deleted);
    }
    return json(result);
  } catch (e) {
    return json({ error: String(e), needsReauth: e.needsReauth ?? false }, 502);
  }
}

async function statusReport(env) {
  const tokens = await loadTokens(env);
  const status = await getStatus(env);
  return json({
    hasTokens: tokens !== null,
    tokenAgeMinutes: tokens ? Math.round((Date.now() - tokens.updated_at) / 60000) : null,
    tokenExpiresInMinutes: tokens ? Math.round((tokens.expires_at - Date.now()) / 60000) : null,
    needsReauth: status.needsReauth ?? false,
    lastError: status.lastError ?? null,
  });
}

// ---------------------------------------------------------------- Auth & utils

/** Bearer-token check for the gadget, constant-time to avoid oracle timing. */
async function isDevice(request, env) {
  if (!env.DEVICE_TOKEN) return false; // secret not configured yet
  const auth = request.headers.get("Authorization") ?? "";
  const token = auth.startsWith("Bearer ") ? auth.slice(7) : "";
  return timingSafeEqual(token, env.DEVICE_TOKEN);
}

/**
 * Admin auth via ?admin= query param so routes work from a browser.
 * Trade-off: query strings can end up in logs; acceptable for a
 * single-admin hobby service, and the token is rotatable via
 * `wrangler secret put ADMIN_TOKEN`.
 */
function isAdmin(url, env) {
  if (!env.ADMIN_TOKEN) return false;
  return timingSafeEqual(url.searchParams.get("admin") ?? "", env.ADMIN_TOKEN);
}

function timingSafeEqual(a, b) {
  const enc = new TextEncoder();
  const ab = enc.encode(a);
  const bb = enc.encode(b);
  if (ab.byteLength !== bb.byteLength) return false;
  return crypto.subtle.timingSafeEqual(ab, bb);
}

function randomToken() {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
    },
  });
}

function html(body, status = 200) {
  return new Response(`<!doctype html><meta charset="utf-8">${body}`, {
    status,
    headers: { "content-type": "text/html; charset=utf-8" },
  });
}

function escapeHtml(s) {
  return s.replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
}
