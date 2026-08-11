# Phase 0 spike — is a Monzo Under-16s account visible via the developer API?

**Status: OPEN — findings not yet recorded.**

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

_Fill in after running the steps:_

- Date tested:
- Accounts returned (types/descriptions only — **do not paste real account ids
  into this public repo**):
- Child account visible? (yes/no):
- If visible — `/balance` works? `/transactions` works?
- Pots listing works on parent account? (yes/no):
- **Decision: `MODE=account` / `MODE=pot`**

## Plan B, for reference

If the child account is invisible: create a pot for the child on the parent
account. `GET /pots` gives its balance. Pots have no per-pot transaction feed in
the public API, so the transactions screen either shows pot transfers filtered
from the parent's feed (by `metadata.pot_id`) or is dropped.
