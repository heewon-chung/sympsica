# test/e2e/fixtures/ — smoke-E2E schedule pair

`schedule_r.json` / `schedule_s.json` (task-18-brief.md R-E2E-DRIVER, SC6):
a small, hand-built, COMMITTED schedule PAIR in the W5.7 per-party day
format (`{"day": "...", "insert": [ids], "delete": [ids], "query": bool,
"maintenance": bool}`), consumed by `test/e2e/run_smoke.py`. Not generated
at test time (unlike the Phase-1/3 `.fixture` files under `test/fixtures/`,
which ARE regenerated from `ref/reference.py`'s emitters) — hand-authored
once here so the expected per-query counts below can be independently
hand-derived and cross-checked against `Ref.simulate_days()`'s own
computation, rather than trusting one generator to grade its own homework.

- 6 days, `2023-01-01` .. `2023-01-06`.
- 3 query days (`2023-01-02`, `2023-01-03`, `2023-01-05`) — SC6 needs
  `>= 2`.
- 1 maintenance day (`2023-01-03`, `"maintenance": true` on BOTH files) —
  SC6 needs exactly this: `SaltManager::due()` fires via the
  `maintenance_day` OR-branch regardless of `phi`/`force`, so this day's
  JSONL record carries `"path":"maintenance_full"`.
- 49 distinct ids total across both parties' plaintext universes (R:
  `1..44` contiguous; S: `10..33` plus `100..104`, all of `10..33` already
  covered by R's range) — close to, not exactly, the brief's "n ~= 64"
  approximate target; the exact count is not load-bearing, only that the
  schedule is small (fast) and produces genuinely non-trivial, hand-
  verifiable per-day intersections.

## Expected per-query counts (hand-derived, cross-checked by
`Ref.simulate_days()`)

Applying each day's `insert`/`delete` to that party's own running id set
(via `update_filter()`, the same three-stage filter `Update::apply` uses),
in day order:

| day | R's set after this day's edits | S's set after this day's edits | query | intersection |
|---|---|---|---|---|
| 01-01 | `{1..24}` | `{10..33}` | no | — |
| 01-02 | `{1..30}` | `{10..33,100,101}` | **yes** | `{10..30}` = **21** |
| 01-03 (maintenance) | `{3..34}` (insert 31-34, delete 1,2) | `{12..33,100,101,102}` (insert 102, delete 10,11) | **yes** | `{12..33}` = **22** |
| 01-04 | `{3..40}` | `{12..33,100,101,102}` (no-op) | no | — |
| 01-05 | `{3..44}` (insert 41-44, no delete) | `{12..33,100,101,102,103,104}` (insert 103,104, no delete) | **yes** | `{12..33}` = **22** |
| 01-06 | `{3..44}` (no-op) | `{12..33,100,101,102,103,104}` (no-op) | no | — |

Expected count sequence: **`[21, 22, 22]`**. `test/e2e/run_smoke.py`
computes this same sequence via `Ref.simulate_days(schedule_r, schedule_s)`
(oracle-independent set arithmetic, R-ORACLE-AGNOSTIC) and asserts BOTH
real party processes' JSONL `"count"` fields equal it exactly, in order,
at every query day.

## Why 01-05 (and every non-maintenance day) carries NO deletes

**Found defect (documented, not fixed here — out of task-18-brief.md's
file scope, which permits touching `protocols/query.hpp`/`.cpp` only via
the additive R-FORCEFULL parameter):** `src/protocols/query.cpp`'s
`run_full()` asserts `occupied.size() <= min(my_size, Params::M)`, where
`occupied` is every KEY currently present in `PartyState::table`'s
underlying map. `PowerSumTable::edit(id, -1, ...)` (a delete) decrements
that bucket's row values but never ERASES the map entry, even once the row
is exactly all-zero (table.hpp documents the zero row as the padding-row
convention for reads, but `edit()` itself is silent on removal). At this
fixture's scale, M = 2^31 makes bucket collisions between our ~49 ids
astronomically unlikely, so a single id is effectively the sole occupant of
its bucket — meaning ANY delete leaves a "ghost" zero row that inflates
`occupied` by one, without `my_size` (which correctly decrements) moving to
match. A full-path query that runs AFTER such a delete, without an
intervening `PowerSumTable::rebuild()` in between, then trips
`run_full()`'s invariant check and aborts.

`SaltManager::refresh()`'s own `table.rebuild()` (W5.5) clears this
staleness unconditionally, which is exactly why `2023-01-03`'s two deletes
(R: `[1,2]`, S: `[10,11]`) are safe: that day IS the maintenance day, so
its own forced-full query runs immediately AFTER a fresh rebuild. Every
OTHER day in this fixture is insert-only by construction, specifically to
avoid re-triggering the same latent issue on a day with no rebuild ahead of
it. See `task-18-report.md`'s "found defect" section for the full
reproduction (an earlier draft of this fixture DID delete on `2023-01-05`
and reproduced `SYMPSICA_REQUIRE failed: Query::run: occupied buckets
exceed min(my_size, m)` on both real party processes).
