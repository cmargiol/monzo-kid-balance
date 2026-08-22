# Plan — PR sequence and status

Source of truth for the build order across sessions. Update as PRs merge.

**Project:** an M5StickC PLUS2 desk gadget showing a kid's Monzo balance and
last 3 transactions, via a Cloudflare Worker proxy. Architecture and design
decisions: see `README.md`; Monzo API findings: `docs/spike-notes.md`.

## Sequence

| # | PR | Status | Depends on |
|---|----|--------|------------|
| 1 | Scaffold: README, spike notes, gitignore, license | ✅ merged (#1) | — |
| 2 | Worker skeleton: mock `/display` + bearer auth | ✅ merged (#2→#3) | 1 |
| 3 | Worker OAuth: login flow, KV token rotation, admin routes | ✅ merged (#4) | 2 |
| 4 | Worker live data: balance/tx fetch, formatting, cron | ✅ merged (#5) | 3 |
| 5 | Worker resilience: webhook, re-auth nags | ✅ merged (#6); staleness + review fixes re-landing in #9 | 4 |
| 6 | Firmware import: vendored factory demo + upstream fixes | ✅ merged (#7) | — (independent of 2–5) |
| 7 | Balance app rendering mock data in the screen cycle | ⏳ next | 6 |
| 8 | Device↔Worker: HTTPS + CA bundle + NTP, NVS cache, error screens | ⏳ | 7, and Worker deployed |
| 9 | Transactions screen + dual power modes | ⏳ | 8 |

Worker PRs (2–5) and firmware PRs (6–9) are two independent tracks; they only
meet at PR 8, which needs the Worker deployed and reachable.

## Manual setup (outside PRs, owner: repo owner)

- ✅ Phase 0 spike: Under-16s account is API-visible, balance + transactions
  readable via parent token (`docs/spike-notes.md`)
- ⏳ Cloudflare: `wrangler login`, KV namespace, secrets, first deploy
  (steps: `worker/README.md`)
- ⏳ Monzo OAuth client (Confidential) + connect + `ACCOUNT_ID` secret
- ⏳ Optional: webhook registration, ntfy topic for re-auth nags
- ⏳ Device: flash firmware over USB once PR 8 lands (`secrets.h` from template)

## Working agreements

- One branch per PR, based on `main`; stacked work is prepared locally but its
  PR opens only after the base merges. Delete branches on merge.
- Commit messages and repo-visible names are proposed for review before
  committing; no secrets or account identifiers anywhere in the repo.
- Every PR gets a `/code-review` pass with findings addressed before review,
  and lists its verification steps in the description.
