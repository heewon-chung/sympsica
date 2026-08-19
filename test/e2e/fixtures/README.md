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
- `2023-01-05` deletes an id on BOTH sides (R: `3`; S: `100`) on a
  NON-maintenance query day, immediately preceded by NO
  `PowerSumTable::rebuild()` — this is deliberate (see "R-OCCUPIED" below),
  not an oversight: it is exactly the shape that used to abort both real
  party processes before that fix landed.

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
| 01-05 | `{4..44}` (insert 41-44, delete 3) | `{12..33,101,102,103,104}` (insert 103,104, delete 100) | **yes** | `{12..33}` = **22** |
| 01-06 | `{4..44}` (no-op) | `{12..33,101,102,103,104}` (no-op) | no | — |

Expected count sequence: **`[21, 22, 22]`**. `test/e2e/run_smoke.py`
computes this same sequence via `Ref.simulate_days(schedule_r, schedule_s)`
(oracle-independent set arithmetic, R-ORACLE-AGNOSTIC) and asserts BOTH
real party processes' JSONL `"count"` fields equal it exactly, in order,
at every query day.

## `e2e_seed{0,1,2,3}_{r,s}.json` (task-19-brief.md SC2/E2E-1..4, W5.8)

Golden source: `ref/reference.py`'s `Ref.make_e2e_schedule_pair(seed)` +
`Ref.emit_e2e_schedule_pair()`. Regenerate with:

```
python3 ref/reference.py emit-e2e --seed <N> --out-r test/e2e/fixtures/e2e_seed<N>_r.json --out-s test/e2e/fixtures/e2e_seed<N>_s.json
```

for `N` in `0..3` (R-SCALE19: "seeds 0..9 is a superset -- run 4"). Unlike
`schedule_r.json`/`schedule_s.json` above (hand-authored), these ARE
generator-emitted -- but still real JSON (party_main.cpp's native
`--schedule` format), not this directory's own line-based fixture
convention, since these files are consumed directly by the `party` binary,
not by a C++ test reading via `fixture_support.hpp`.

Each pair: n ~ 2^10 ids/party (`seed*10000` offsets keep the four pairs'
id universes disjoint), exactly 4 days --
`d0` (bulk insert, no query) / `d1` (first query, plain `FullPublic`) /
`d2` (`"maintenance":true` on both files -- salt-refresh query day,
R-SCALE19's ">= 1 salt-refresh day") / `d3` (delete-bearing query day,
R-SCALE19's ">= 1 delete-bearing normal day") -- 3 query days total,
deliberately kept SHORT (R-SCALE19: "Expect minutes per schedule; keep
each schedule SHORT"). `test/e2e/run_e2e_gate.py` computes the expected
per-query-day counts LIVE via `Ref.simulate_days()` (not against a
committed golden transcript -- committed E2E goldens are Phase-6's
deliverable, per task-19-brief.md requirement 2).

These same 4 fixture pairs are also reused (unmodified) by the FC1/FC2/FC5
wrong-construction legs (`--expect-mismatch` mode, run against a
`SYMPSICA_WRONG_SIGN`/`SYMPSICA_WRONG_CONVERT`/`SYMPSICA_STALE_SIZES` build
of `party` -- see CMakeLists.txt's `SYMPSICA_NO_FILTER` precedent and
task-19-report.md for the exact commands run and observed output); no
separate FC-specific fixtures exist. FC1 additionally verifies (via
`--require-asymmetric`) that the reused pair really is an asymmetric set
(`|A\B| != |B\A|`), since TV-F8 specifically requires that shape.

## Why 01-05 deletes on a non-maintenance day (R-OCCUPIED)

**Found and FIXED defect (controller ruling R-OCCUPIED, task-18-brief.md):**
`src/protocols/query.cpp`'s `run_full()` used to compute `occupied` as
every KEY present in `PartyState::table`'s underlying map.
`PowerSumTable::edit(id, -1, ...)` (a delete) decrements a bucket's row
values but never ERASES the map entry, even once the row is exactly
all-zero — so a delete that empties a bucket entirely, followed by a
full-path query with no intervening `PowerSumTable::rebuild()`, used to
inflate `occupied` past `min(my_size, Params::M)` and trip
`run_full()`'s own `SYMPSICA_REQUIRE`, aborting BOTH real party processes.
This fixture originally dodged the shape (kept every non-maintenance day
insert-only) instead of exercising it; that dodge was rejected (masking a
symptom rather than fixing the root cause) and this fixture was restored
to include it once the real fix landed.

**The fix**: `run_full()` now filters `occupied` to buckets whose row is
NON-ZERO, matching `table.hpp`'s own `row()` contract (an absent row and
an all-zero row are already treated identically everywhere else). This
filter is EXACT, not a heuristic, for any bucket occupancy up to
`Params::K = 7` (a row only ever stores `K` power sums): by Newton's
identities, an all-zero power-sum vector forces every elementary symmetric
polynomial of the bucket's `sigma`-values to vanish too, which forces the
bucket's characteristic polynomial to `X^t` — i.e. every `sigma(x_i) = 0`,
impossible since `sigma(x) = g^x` is never `0` in `F_p^*`. So "row is
all-zero" `<=>` "bucket holds nothing", unconditionally. See
`src/protocols/query.cpp`'s own comment at the fix site (`run_full`) for
the full argument, and `test/protocols/kat_query.cpp`'s
`Query.ROCCUPIED_DeleteThenFullPathQueryDoesNotAbort` for a direct
regression test that reproduces the exact old abort shape (verified, by
temporarily reverting the fix, to actually crash without it) and now
passes.

`2023-01-03` (the maintenance day) still deletes too (R: `[1,2]`, S:
`[10,11]`) — safe either way, since `SaltManager::refresh()`'s own
`table.rebuild()` clears any staleness before its own forced-full query
regardless of this fix. `2023-01-05` is the day that specifically requires
the fix: a plain, non-maintenance `Query::run` call, reached via
`SwitchRule::decide`'s ordinary size condition, with a delete immediately
before it and no rebuild in between.
