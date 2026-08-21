/**
 * Monzo API helpers: OAuth token lifecycle and authenticated GETs.
 *
 * Token rules (see README "Monzo quirks"):
 * - Refresh tokens are SINGLE-USE. Persisting the rotated pair is the first
 *   thing we do after a successful exchange — before any other await — so a
 *   crash can't leave us holding a burned token.
 * - Only ONE writer may ever refresh: the cron handler (added in a later PR)
 *   and the one-time OAuth callback. Request-time handlers must never refresh,
 *   or two concurrent refreshes would race and one would persist a dead token.
 * - A 403 `forbidden.insufficient_permissions` means the ~90-day SCA approval
 *   has lapsed: the tokens are fine, but a human must re-approve in the Monzo
 *   app (Settings → Privacy & Security → Manage apps).
 */

const API = "https://api.monzo.com";
export const AUTH_BASE = "https://auth.monzo.com/";

/** Exchange the OAuth authorization code and persist the token pair. */
export async function exchangeCode(env, code, redirectUri) {
  const res = await fetch(`${API}/oauth2/token`, {
    method: "POST",
    body: new URLSearchParams({
      grant_type: "authorization_code",
      client_id: env.MONZO_CLIENT_ID,
      client_secret: env.MONZO_CLIENT_SECRET,
      redirect_uri: redirectUri,
      code,
    }),
  });
  if (!res.ok) {
    throw new Error(`code exchange failed: ${res.status} ${await res.text()}`);
  }
  await saveTokens(env, await res.json());
}

/**
 * Return a valid access token, refreshing if it expires within 45 minutes.
 * CRON/CALLBACK USE ONLY — request-time handlers call loadTokens() instead
 * and treat a stale token as "wait for the next cron tick".
 */
export async function ensureFreshToken(env) {
  const t = await loadTokens(env);
  if (!t) return null;
  if (Date.now() < t.expires_at - 45 * 60 * 1000) return t.access_token;

  const res = await fetch(`${API}/oauth2/token`, {
    method: "POST",
    body: new URLSearchParams({
      grant_type: "refresh_token",
      client_id: env.MONZO_CLIENT_ID,
      client_secret: env.MONZO_CLIENT_SECRET,
      refresh_token: t.refresh_token,
    }),
  });
  if (!res.ok) {
    // invalid_grant => grant revoked or rotation lost; needs a human re-auth.
    await updateStatus(env, {
      needsReauth: true,
      lastError: `token refresh failed: ${res.status}`,
    });
    return null;
  }
  const fresh = await res.json();
  await saveTokens(env, fresh); // must be the first await after parsing
  return fresh.access_token;
}

export async function loadTokens(env) {
  return JSON.parse((await env.KV.get("tokens")) ?? "null");
}

async function saveTokens(env, t) {
  await env.KV.put(
    "tokens",
    JSON.stringify({
      access_token: t.access_token,
      refresh_token: t.refresh_token,
      expires_at: Date.now() + t.expires_in * 1000,
      updated_at: Date.now(),
    })
  );
  await updateStatus(env, { needsReauth: false, lastError: null });
}

/**
 * Authenticated GET. Returns parsed JSON, or throws MonzoError with
 * `needsReauth: true` on the SCA-lapse 403.
 */
export async function monzoGet(env, accessToken, path) {
  const res = await fetch(API + path, {
    headers: { Authorization: `Bearer ${accessToken}` },
  });
  if (res.ok) return res.json();

  const body = await res.text();
  const needsReauth =
    res.status === 403 && body.includes("insufficient_permissions");
  if (needsReauth) {
    await updateStatus(env, { needsReauth: true, lastError: "SCA lapsed" });
  }
  const err = new Error(`GET ${path}: ${res.status} ${body}`);
  err.needsReauth = needsReauth;
  throw err;
}

export async function getStatus(env) {
  return JSON.parse((await env.KV.get("status")) ?? "{}");
}

export async function updateStatus(env, patch) {
  const status = await getStatus(env);
  await env.KV.put(
    "status",
    JSON.stringify({ ...status, ...patch, updated_at: Date.now() })
  );
}
