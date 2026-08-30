# Plan — PR sequence and status

**Status: COMPLETE.** The gadget is live: real balance and last-3 transactions
on the child's desk, updating within a minute of a card tap. This file is the
historical record of how the build was sequenced.

**Project:** an M5StickC PLUS2 desk gadget showing a kid's Monzo balance and
last 3 transactions, via a Cloudflare Worker proxy. Architecture and design
decisions: `README.md`; Monzo API findings: `docs/spike-notes.md`; accepted
security trade-offs: `worker/README.md`.

## Sequence (all merged)

| # | PR | Merged as |
|---|----|-----------|
| 1 | Scaffold: README, spike notes, gitignore, license | #1 |
| 2 | Worker skeleton: mock `/display` + bearer auth | #2→#3 |
| 3 | Worker OAuth: login flow, KV token rotation, admin routes | #4 |
| 4 | Worker live data: balance/tx fetch, formatting, cron | #5 |
| 5 | Worker resilience: webhook, re-auth nags, staleness detection | #6 + #9 |
| 6 | Firmware import: vendored factory demo + upstream fixes | #7 |
| 7 | Balance app rendering mock data in the screen cycle | #10 |
| 8 | Device↔Worker: HTTPS + CA bundle + NTP, NVS cache, error screens | #11 |
| 9 | Battery power policy + PWR messaging (planned as "polish") | #12 |
| — | Post-plan: re-auth nags by email via Brevo | #13 |
| — | Post-plan: PLAN.md close-out + accepted-trade-offs docs | #14 |
| — | Post-plan: transaction messages on the gadget, with Greek | #15 |
| — | Post-plan: `scripts/update.sh` day-2 operations | #16 |
| — | Post-plan: firmware README + docs sweep | #17 |

(#8 was the PLAN.md introduction itself. The #2→#3 duplication and the #6/#9
split were stacked-PR merge mishaps, both documented in those PRs.)

## Manual setup (all done)

- ✅ Phase 0 spike: Under-16s account is API-visible via the parent's token
- ✅ Cloudflare: KV namespace, secrets, deploy (monzo-kid-balance.workers.dev)
- ✅ Monzo OAuth client (Confidential) connected; `ACCOUNT_ID` set
- ✅ Webhook registered (card tap → screen update in under a minute, verified)
- ✅ Device flashed in live mode; phases 1–3 field-tested on hardware
- ✅ Re-auth nag by email (Brevo), live-drilled end to end: forced a real
  Monzo 403 → email + "Ask Dad" screen → restored → self-healed

## What field testing changed (kept for the next hardware project)

Every firmware PR was tested on the physical device before merge, and every
round found something the desk build couldn't: the missing £ glyph in every
bundled font, `esp_restart()` dropping the power-hold GPIO, the TLS handshake
starving on heap beside a 16-bit framebuffer, fetch completions being
swallowed by the next fetch's flag reset, a charging battery misread as
"on battery" bricking via `power_off()`, and buttons that felt a second late
because actions waited for release. Emulators would have shown none of these.

## Working agreements (unchanged)

- One branch per PR, based on `main`; stacked work opens only after its base
  merges. Delete branches on merge.
- Commit messages and repo-visible names are proposed for review before
  committing; no secrets or account identifiers anywhere in the repo.
- Every PR gets a `/code-review` pass with findings addressed before review,
  and lists its verification steps in the description.
