# Worker

Cloudflare Worker that (eventually) authenticates with Monzo and caches the
balance. Right now: serves a mock `/display` payload behind bearer-token auth.

## Local dev

```bash
cd worker
cp .dev.vars.example .dev.vars        # then edit the token if you like
npx wrangler dev
curl -H "Authorization: Bearer dev-token-change-me" http://localhost:8787/display
```

## First deploy

```bash
cd worker
npx wrangler login                     # one-time, opens browser
openssl rand -base64 32                # generate the real device token…
npx wrangler secret put DEVICE_TOKEN   # …and paste it here (also save it in
                                       #    your password manager — the firmware
                                       #    will need it)
npx wrangler deploy                    # prints your workers.dev URL
```

Verify the live endpoint:

```bash
curl https://<your-worker-url>/display                                  # → 401
curl -H "Authorization: Bearer <token>" https://<your-worker-url>/display  # → mock JSON
```

The deploy URL matters beyond testing: it becomes the OAuth redirect host.
After first deploy, create the Monzo OAuth client (next PR) with redirect URI
`https://<your-worker-url>/oauth/callback` and **confidentiality: Confidential**
at <https://developers.monzo.com>.
