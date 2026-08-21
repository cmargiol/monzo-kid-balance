# Worker

Cloudflare Worker that authenticates with Monzo, caches the balance, and serves
it to the gadget. A cron tick every 15 minutes refreshes OAuth tokens (early,
45 min before expiry) and rebuilds the cached `/display` payload from live
`/balance` + `/transactions` data; `/display` itself never calls Monzo, so it
stays fast and keeps serving (stale) data even if Monzo is down.

## Routes

| Route | Auth | Purpose |
|---|---|---|
| `GET /display` | `Authorization: Bearer <DEVICE_TOKEN>` | Payload the gadget renders |
| `GET /oauth/start?admin=<ADMIN_TOKEN>` | admin | Begin Monzo OAuth (open in a browser) |
| `GET /oauth/callback` | CSRF state | Redirect target; stores the token pair |
| `GET /admin/accounts?admin=<ADMIN_TOKEN>` | admin | Live dump of accounts + pots — use this to pick the account/pot id |
| `GET /status?admin=<ADMIN_TOKEN>` | admin | Token age/expiry, needs-reauth flag, last poll result |
| `POST /webhook/monzo?key=<WEBHOOK_KEY>` | shared key | Monzo `transaction.created` doorbell → background cache refresh |
| `GET /admin/register-webhook?admin=<ADMIN_TOKEN>` | admin | One-time, idempotent webhook registration with Monzo |

## Local dev

```bash
cd worker
cp .dev.vars.example .dev.vars        # then edit tokens if you like
npx wrangler dev
curl -H "Authorization: Bearer dev-token-change-me" http://localhost:8787/display
curl "http://localhost:8787/status?admin=dev-admin-change-me"
```

## First-time setup (production)

```bash
cd worker
npx wrangler login                     # one-time, opens browser
npx wrangler kv namespace create KV    # paste printed id into wrangler.toml
openssl rand -base64 32                # device token…
npx wrangler secret put DEVICE_TOKEN   # …paste it (save in password manager too)
openssl rand -base64 32                # admin token…
npx wrangler secret put ADMIN_TOKEN    # …paste it (password manager again)
npx wrangler deploy                    # prints your workers.dev URL
```

Then create the Monzo OAuth client at <https://developers.monzo.com>:

- **Redirect URI**: `https://<your-worker-url>/oauth/callback`
- **Confidentiality**: **Confidential** (required — only confidential clients
  get refresh tokens)

and store its credentials:

```bash
npx wrangler secret put MONZO_CLIENT_ID
npx wrangler secret put MONZO_CLIENT_SECRET
```

## Connecting to Monzo

1. Open `https://<your-worker-url>/oauth/start?admin=<ADMIN_TOKEN>` in a
   browser; log in with the Monzo account's email (magic link).
2. **Approve the prompt in the Monzo app** — until then every API call fails
   with `insufficient_permissions` (this is Monzo's SCA).
3. Visit `/admin/accounts?admin=<ADMIN_TOKEN>` to see every account and pot
   the grant can read; `/status?admin=<ADMIN_TOKEN>` for health.
4. Copy the child account's `acc_…` id from that response and store it:
   `npx wrangler secret put ACCOUNT_ID` (a secret, not a var, to keep
   identifiers out of this public repo). The next cron tick (≤15 min) populates
   `/display` with real data.
5. Near-instant updates (optional but nice — balance changes seconds after the
   card is tapped): `openssl rand -hex 24` → `npx wrangler secret put
   WEBHOOK_KEY`, redeploy, then visit
   `/admin/register-webhook?admin=<ADMIN_TOKEN>` once.
6. Re-auth nags (recommended): pick an unguessable topic name
   (`openssl rand -hex 8`) → `npx wrangler secret put NTFY_TOPIC`, and
   subscribe to that topic in the free [ntfy](https://ntfy.sh) phone app.
   When Monzo's ~90-day SCA lapse hits, you get one push per day telling you
   the re-approval steps instead of a silently stale gadget.

## Tests

```bash
cd worker && npm test    # pure-function tests for the formatting layer
```

## Monzo quirks the code is built around

- **Refresh tokens are single-use** — the rotated pair is persisted to KV
  immediately after every exchange, and only ever by one writer (the cron; the
  OAuth callback before the cron exists), so concurrent refreshes can't race
  and burn the grant.
- **~90-day SCA lapse** — undocumented: all calls (even balance) start
  returning 403 `forbidden.insufficient_permissions` until the account holder
  re-approves in the Monzo app (Settings → Privacy & Security → Manage apps).
  The Worker flags this as `needsReauth` in `/status` and (in a later PR) tells
  the gadget to show its "ask a grown-up" screen.
