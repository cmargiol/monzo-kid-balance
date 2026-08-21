# Phase 0 spike — is a Monzo Under-16s account visible via the developer API?

**Status: RESOLVED (2026-08-21) — YES on all counts. `MODE=account`; the pot
fallback is not needed and was dropped from the implementation.**

Monzo's Under-16s account (launched July 2024) is parent-managed, has no sort
code/account number, and legally the money belongs to the parent. Whether it
appears in the parent's `GET /accounts` API response is undocumented and — as
far as we could find — unreported anywhere. Everything downstream of this spike
is parameterised (`MODE=account` vs `MODE=pot`), so the answer changes a config
value, not the design.

## Steps (~30 min, needs the parent's phone with the Monzo app)

1. Go to <https://developers.monzo.com> → **Sign in** with the parent's Monzo
   login (magic-link email, then approve the push in the Monzo app).
2. Open the **API Playground**.
3. Run `GET /accounts` (no filter). Note every account object: `id`,
   `description`, `type`, `created`. Look for anything that could be the child's
   account.
4. For each candidate id, try:
   - `GET /balance?account_id=<id>`
   - `GET /transactions?account_id=<id>&limit=3`
   Note whether they return data, `403 forbidden.insufficient_permissions`, or
   another error. (Reminder: full transaction history is only available for
   5 minutes after auth; after that, the last 90 days — plenty for this test.)
5. Run `GET /pots?current_account_id=<parent account id>` and confirm pots list
   with balances (this validates plan B).

## Findings

- **Date tested:** 2026-08-21, via the API playground with the parent's login.
- **Child account visible?** **Yes** — the Under-16s ("youth") account appears
  in the parent's `GET /accounts` response.
- **`/balance` on the child account:** **works**, returns pence as usual.
  (First attempt looked like `forbidden.insufficient_permissions`, caused by a
  stray space in the `account_id` parameter — Monzo returns a permissions error,
  not a validation error, for malformed ids. Worth remembering when debugging.)
- **`/transactions` on the child account:** **works.** Notable shapes:
  - Transactions come back **oldest-first** — "latest 3" means the tail.
  - Pocket-money transfers have `merchant: null`; the human-readable name is in
    `counterparty.name` (falling back merchant → counterparty → description is
    the right display order).
  - `metadata` marks these clearly (`p2p_is_pocket_money`,
    `p2p_young_transfer_direction: parent_account_to_young_account`).
  - No `account_balance` field on transactions — fine, `/balance` works.
- **Decision: `MODE=account`.** The pot fallback (below) is retained only as
  historical context and is not implemented.

## Plan B, for reference

If the child account is invisible: create a pot for the child on the parent
account. `GET /pots` gives its balance. Pots have no per-pot transaction feed in
the public API, so the transactions screen either shows pot transfers filtered
from the parent's feed (by `metadata.pot_id`) or is dropped.
