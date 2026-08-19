# test/fixtures/ — KAT fixtures

Two families live here, with different golden sources:

- **Phase-1 field/encoding fixtures** (`seed0.fixture` .. `seed9.fixture`,
  `generator_g.txt`) — golden source `ref/reference.py`, described below.
- **Phase-2 protocol fixtures** (`zt5_dkg61.fixture`) — no Python golden
  exists (there is no reference implementation of libOTe's interactive
  `RegularDpf` keyGen), so these are *recorded* from the first green run of
  the test that consumes them, which is what the task briefs call
  `[POST-GATE]`. Each such file carries its own header explaining exactly how
  to regenerate it and on which build variant it was recorded; see
  `zt5_dkg61.fixture`. The format (`# comments` plus `<key> <value...>` rows,
  read by `test/utils/fixture_support.hpp`) is shared with the Phase-1 files.

## Phase-1 fixtures

Golden source: `ref/reference.py` (pure Python, bigints). Every Phase-1 file
here is generated, never hand-edited. Regenerate with:

```
python3 ref/reference.py emit --seed <N> --out test/fixtures/seed<N>.fixture
```

`seed0.fixture` .. `seed9.fixture` were generated with exactly that command
for `N` in `0..9` (task-3-brief.md: "Tests use seeds 0..9 where seeded").

## Why not JSON

The plan text (task-3-brief.md, W1.7/requirement 1) names the emitter's
output "out.json" and asks for a compact form (a PRNG seed + count + digest,
not raw samples) precisely so the file stays small. Requirement 3 explicitly
allows a line-based format instead of JSON, "so the plan's 'out.json' name
still applies" only as the CLI flag's historical label — to avoid pulling in
a JSON library dependency for the C++ side (test/utils/fixture_support.hpp
is a small line-based reader, no JSON parser). `.fixture` is used as the
file extension for clarity; content is plain text as described below.

## Format (v1)

- Lines starting with `#` are comments (always the first two lines: a
  version banner and the exact generator invocation for that file).
- Every other non-blank line is whitespace-separated tokens: `<key> <value...>`.
- A key may repeat across several rows (`fld4` appears 5 times, one per
  boundary input; `tbl1` appears once per file).

| key | values | meaning |
|---|---|---|
| `format` | `1` | fixture format version |
| `seed` | `N` | the `--seed` this file was generated with |
| `p` | `2305843009213693951` | the field modulus, `2^61 - 1` |
| `generator_g` | `37` | the generator found by `ref/reference.py`'s independent `find_generator()` (Python bigint, same 12-factor order test as `src/utils/encoding.cpp`'s `Encoder`) |
| `generator_g_crosscheck_pass` | `1` | always `1`; `emit_fixtures` asserts `g == 37` before writing anything, so a `0` here can never actually be committed |
| `fld4_count` | `5` | number of `fld4` rows that follow |
| `fld4` | `<input> <expected>` | one boundary-reduction golden row (FLD-4): `expected = fp_from_u64(input)`, mirroring `Fp::from_u64`'s Mersenne double-fold. Seed-independent — identical across every committed file. |
| `mulprop_prng_seed` | `N` | initial splitmix64 state for the mul-associativity/canonicity property fixture (this file uses `N = seed`) |
| `mulprop_count` | `1000000` | number of `(a, b, c)` triples drawn from that PRNG stream |
| `mulprop_digest` | a `u64` | FNV-1a-style digest folding `(a*b mod p, (a*b)*c mod p)` for every sample — see `ref/reference.py`'s `digest_update()` and `test/utils/fixture_support.hpp`'s mirror. Regenerating the same `count` samples from `mulprop_prng_seed` via `Fp::mul` and folding the same digest must reproduce this value exactly; a mismatch means `Fp::mul`'s reduction diverges from plain bigint `mod p` arithmetic somewhere in the 10^6-sample space. |
| `tbl1_k` | `7` | power-sum depth (`Params::K`) used for the `tbl1` row |
| `tbl1_count` | `1` | number of `tbl1` rows |
| `tbl1` | `<id_a> <id_b> <s1> .. <s7>` | `Ref.power_sums([id_a, id_b], g, 7)` — emitted in Phase 1, consumed starting Phase 3b (`test/core/kat_table.cpp`'s TBL-1): the C++ side re-derives the two per-id power-sum vectors from `enc.sigma(id)` itself, checks they sum to this row, then checks `PowerSumTable::init({id_a,id_b},...)`'s two per-bucket rows against those per-id vectors individually. |
| `upd1_n_init` | `4` | size of `upd1_init_ids` |
| `upd1_init_ids` | `<id...>` | UPD-1's initial `my_ids` set (task-11-brief.md, Phase 3b, additive) |
| `upd1_I_count` / `upd1_I` | `<id...>` | raw insert candidates (fresh ids, none colliding with `upd1_init_ids`) |
| `upd1_D_count` / `upd1_D` | `<id...>` | raw delete candidates — the first 2 of `upd1_init_ids` (valid deletes by construction) |
| `upd1_Iprime_count` / `upd1_Iprime` | `<id...>` | `Ref.update_filter(upd1_init_ids, upd1_I, upd1_D)`'s `I'` (equals `upd1_I` here — this fixture exercises basic apply, not the filter's skip paths) |
| `upd1_Dprime_count` / `upd1_Dprime` | `<id...>` | same call's `D'` (equals `upd1_D` here) |
| `upd1_expected_final_count` / `upd1_expected_final_ids` | `<id...>` | sorted `(set(upd1_init_ids) - set(upd1_Dprime)) \| set(upd1_Iprime)` — the expected `my_ids` after `Update::apply` |
| `upd1_expected_my_size` | `<n>` | `len(upd1_init_ids) + len(upd1_Iprime) - len(upd1_Dprime)` |

## Phase-3b schedule fixtures (`schedule0.fixture` .. `schedule2.fixture`)

Golden source: `ref/reference.py`'s `Ref.make_schedule()` (schedule generator) +
`Ref.simulate()` (expected-count replay), task-11-brief.md's R-SIM ruling.
Regenerate with:

```
python3 ref/reference.py emit-schedule --seed <N> --out test/fixtures/schedule<N>.fixture
```

`schedule0.fixture` .. `schedule2.fixture` were generated with exactly that
command for `N` in `0..2` (R-SIM: "2-3 small schedule fixtures").

R-SIM's *logical* schema is `schedule.json`: `{"n_init": int, "epochs":
[{"I": [ids], "D": [ids], "query": bool}]}` — the name is kept only as a
schema label, exactly like cut 1's `out.json` CLI-flag precedent (see "Why
not JSON" above): the on-disk fixture uses the same line-based format as
every other file here, and additionally stores the CONCRETE initial ids
(`init_ids`), not just their count, so the C++ side never needs to replicate
`make_schedule()`'s PRNG stream — every id in the file is a realized value,
same convention as `tbl1`'s `id_a`/`id_b`.

| key | values | meaning |
|---|---|---|
| `n_init` | `3` | size of `init_ids` |
| `init_ids` | `<id...>` | the concrete starting `my_ids` set |
| `epoch_count` | `4` | number of `epoch` rows |
| `epoch` | `<idx> <query 0/1> <I_count> <I ids...> <D_count> <D ids...>` | one Update epoch, in order; `query=1` means an expected-count row follows for this epoch |
| `query_count` | `2` | number of `query` rows |
| `query` | `<query_idx> <epoch_idx> <expected_count>` | `expected_count = my_size` after replaying epochs `0..epoch_idx` inclusive through `Ref.update_filter`'s three-stage filter (`Ref.simulate()`'s golden replay) |

**Forward-looking scope (R-SIM, mirrors how `tbl1` was emitted unconsumed in
Phase 1):** the Phase-3 C++ suite (`test/protocols/kat_update.cpp`) consumes
ONLY the `epoch`/`query` rows, as an update-path multi-epoch state check —
it replays every `epoch` through the real `Update::apply` on a `PartyState`
seeded from `init_ids`, and asserts `st.my_size` at each `query` epoch
matches the fixture's `expected_count`. `Ref.count()`/`Ref.t_of()`'s
two-party PSI-cardinality semantics are NOT exercised by this fixture or by
any Phase-3 C++ test; that is Phase 4/6 work, once `gates/minors.hpp` and a
bucket-aware `count()` exist.

## MIN-2 self-check (task-11-brief.md R-MIN)

`ref/reference.py` runs a module-level self-test (`_selftest_minors()`,
called unconditionally at import time, not gated behind any CLI subcommand)
that checks `Ref.minors()`/`Ref.t_of()` against the MIN-1/MIN-2 worked rows
in `.handoff/sympsica-test-vectors.md`'s "minors / rank" section (toy field
F_101, signed difference `{2(+), 3(-)}`): `D_1=100, D_2=95, D_3=0, D_4=0`,
`t_of(...) == 2`. This is a pure-Python self-check — no fixture file is
involved; `test/gates/kat_minors.cpp`'s `MIN2_ConcreteF101Row` test (Phase 4,
task-13-brief.md) mirrors the same F_101 worked row through a plaintext
recomputation helper (`MinorCircuit` is hardwired to the production `Fp`,
p=2^61-1, so it cannot run over F_101 directly).

## MIN-3/MIN-4 fixtures (`min0.fixture` .. `min2.fixture`, task-13-brief.md)

Golden source: `ref/reference.py`'s `Ref.minors()`/`Ref.t_of()` (the m-index
schedule — see `_selftest_minors()`), cross-checked against
`Ref.minors_det()` (an INDEPENDENT literal-Hankel-determinant path via
fraction-free Bareiss elimination, falling back to exact Leibniz expansion
only when Bareiss hits a zero interior pivot — controller ruling, binding:
"NOT the m-index scheme") before anything is written. Regenerate with:

```
python3 ref/reference.py emit-min --seed <N> --out test/fixtures/min<N>.fixture
```

`min0.fixture` .. `min2.fixture` were generated with exactly that command for
`N` in `0..2` — three files, mirroring the schedule-fixture precedent above
("2-3 small schedule fixtures") rather than the full `seed0..9` range used by
the Phase-1 field/encoding family: `test/gates/kat_minors.cpp` consumes only
`min0.fixture` for its MIN-3/MIN-4 assertions (500 MIN-4 cases already run
the full two-party `MinorCircuit`/`BeaverEngine` protocol end-to-end over a
real TCP `Channel`; running that 3x over would not add coverage, only
runtime), so `min1.fixture`/`min2.fixture` exist for golden-source
reproducibility/consistency with the project's seed convention but are not
separately wired into the C++ suite.

| key | values | meaning |
|---|---|---|
| `seed` | `N` | the `--seed` this file was generated with |
| `min3_count` | `3` | number of `min3` rows (one per `t` in `{2,3,4}`) |
| `min3` | `<n> <d1..d7> <D1..D4> <expected_t>` | one `Ref.find_min3_example(seed, t)` row: `n` signed elements were drawn (informational only — the C++ side consumes only the resulting syndrome vector `d`), `D_1` is always `0` (the deliberate interior zero — MIN-3/TV-F7's max-rule pin), `expected_t` is `Ref.t_of(d)` (must equal `t`) |
| `min4_count` | `500` | number of `min4` rows (`t` in `{0..4}`, 100 each) |
| `min4` | `<n> <d1..d7> <D1..D4> <expected_t>` | one random signed set per row (`Ref.make_signed_set`), same row shape as `min3`; `expected_t` is the golden `Ref.t_of(d)` the C++ side's `MinorCircuit::eval` + `t_of()` output must match exactly |

x_i/s_i (the underlying signed elements) are NOT stored on the wire — only
the resulting syndrome vector `d`, since that is all `MinorCircuit::eval`
(and `Ref.minors`/`Ref.minors_det`) ever consume.

## SD-1/SD-2 fixtures (`sd0.fixture` .. `sd2.fixture`, task-14-brief.md)

Golden source: the SAME generator as the MIN fixtures above
(`Ref.make_signed_set`/`Ref.signed_syndromes`/`Ref.t_of`, cross-checked
against `Ref.minors_det()` before anything is written) — R-SYNDROMES
("power_sums difference" semantics) is satisfied by `signed_syndromes`
itself, whose docstring derives it as algebraically equivalent to
`power_sums(A) - power_sums(B)`. Regenerate with:

```
python3 ref/reference.py emit-sd --seed <N> --out test/fixtures/sd<N>.fixture
```

`sd0.fixture` .. `sd2.fixture` were generated with exactly that command for
`N` in `0..2` (mirroring the MIN/schedule fixture precedent: 3 files, not
the full `seed0..9` range). `test/gates/kat_symdiff.cpp` consumes only
`sd0.fixture`.

| key | values | meaning |
|---|---|---|
| `sd_count` | `5` | number of `sd` rows (one per `t` in `{0,1,2,3,4}`, in order) |
| `sd` | `<n> <d1..d7> <D1..D4> <expected_t>` | same row shape as `min3`/`min4`; row index `t` (0-based, matching `t`'s own value) is what `SymdiffSD1_*`/`SymdiffSD2_*` (kat_symdiff.cpp) index into |

The `t=0` row (index 0) is doubly meaningful: `Ref.make_signed_set(state, 0)`
draws no elements, so `Ref.signed_syndromes([], [])` is already the
all-zero vector — simultaneously "a genuine empty-difference bucket" AND
"the padding-row convention" SD-1 names (a beta paired with an all-zero-
SHARE syndrome row). `test/gates/kat_symdiff.cpp` exercises both readings
of that one row by varying HOW it is additively shared (a random split
summing to zero vs. a literal `Share{Fp(0)}` on both parties), not by
asking `reference.py` for two different rows. SD-2 ("t = T = 4 exactly,
pivot at bound") reads row index 4 directly.

## `sched100.fixture` (task-19-brief.md SC1, W5.8)

Golden source: `ref/reference.py`'s `Ref.make_paired_day_schedule()` (random
day-schedule-pair generator, W5.7 per-party day format) + `Ref.simulate_days()`
(expected per-query-day count replay). Regenerate with:

```
python3 ref/reference.py emit-sched100 --seed-lo 0 --seed-hi 99 --out test/fixtures/sched100.fixture
```

**All 100 seeds (0..99), per R-SCALE19-AMEND:** the plan's literal "100
randomized schedules" is NOT reduced -- an earlier draft of this task
shipped a 35-seed reduction to fit a "~180s Release" figure, but that
figure was a controller-side convenience estimate, not a requirement; the
100-seed count itself is spec-side (plan W5.8 text + the phase SC line)
and stays as-is behind the `heavy` ctest LABEL/TIMEOUT, which is exactly
what that label exists for. Measured runtime: 436.2s for all 100 seeds. See
`test/protocols_heavy/kat_schedule100.cpp`'s own top comment for the
(negative) provisioning-overhead investigation and (positive) n-scale
retuning that went into that final cost.

Ids are stored CONCRETE (same convention as every other fixture in this
directory), so the C++ side never needs to replicate this file's PRNG
stream.

| key | values | meaning |
|---|---|---|
| `seed_lo` / `seed_hi` | `0` / `99` | the seed range this file covers |
| `sched_count` | `100` | number of `sched` rows |
| `sched` | `<seed> <n_days> <n_queries>` | one schedule's shape |
| `day` | `<seed> <day_label> <query 0/1> <ins_r_count> <ins_r...> <del_r_count> <del_r...> <ins_s_count> <ins_s...> <del_s_count> <del_s...>` | one day of one schedule's PAIR (both parties' insert/delete lists for that day) |
| `expected` | `<seed> <count...>` | `Ref.simulate_days()`'s golden per-query-day count sequence for that seed, in day order |

## Pinned PRNG: splitmix64

Both `ref/reference.py` (`splitmix64_next`) and
`test/utils/fixture_support.hpp` (`splitmix64_next`) implement the same
public-domain splitmix64 generator (Sebastiano Vigna) with identical mixing
constants, so a `(prng_seed, count)` pair regenerates byte-identical
sequences in either language without shipping the raw samples.

## `golden_seed0.fixture` .. `golden_seed9.fixture` (task-21-brief.md W6.2, R6-GOLDCONTENT)

Golden source: `ref/reference.py`'s `Ref.make_golden_pair()` (two `nR = nS =
2^10` concrete id sets, R/S per R-SYND's own Receiver/Sender convention) +
`Ref.emit_golden_fixture()` (per-symmetric-difference-id syndrome/rank via
`Ref.syndromes()`/`Ref.t_of()`, self-checked against a direct plaintext
intersection before writing). Regenerate with:

```
python3 ref/reference.py emit-golden --seed <N> --out test/fixtures/golden_seed<N>.fixture
```

`golden_seed0.fixture` .. `golden_seed9.fixture` were generated with exactly
that command for `N` in `0..9` (task-21-brief.md W6.2: "seeds 0..9,
n = 2^10"). **NOT the same family as `seed0.fixture` .. `seed9.fixture`
above** (R6-GOLDNAME, binding) -- those are Phase-1 field/encoding KAT
inputs with a completely different meaning; the two families happen to
share the `0..9` seed range only by convention.

R6-GOLDCONTENT rationale: a golden pinning only the final count would be too
weak to be Phase 6's positive evidence (a wrong per-bucket computation that
happens to sum correctly would still pass) -- so this fixture pins, per
seed: the two full id sets, EVERY symmetric-difference id's per-bucket
signed syndrome vector and recovered rank, the aggregate rank, AND the
derived count, cross-checked against a direct plaintext intersection inside
the generator itself.

There is no real bucket LABEL stored in the file: `ref/reference.py` never
ports `BucketOracle`/BLAKE3 (R-ORACLE-AGNOSTIC, task-18-brief.md), so each
symmetric-difference id is treated as occupying its own "virtual bucket"
keyed by the id itself. `test/protocols/kat_golden_cheap.cpp` (the C++
consumer) is what ties this back to the REAL `Params` oracle: it explicitly
checks, per seed, that the real `BucketOracle::of()` is injective over every
id in `ids_r U ids_s` (a genuine, falsifiable precondition -- see that
file's own top comment for why this is sound rather than an unverified
assumption at `M = 2^31` buckets / ~2000 ids).

| key | values | meaning |
|---|---|---|
| `seed` | `N` | the `--seed` this file was generated with |
| `generator_g` | `37` | the canonical generator (same cross-check as every other family) |
| `golden_overlap` | `<k>` | `\|ids_r ∩ ids_s\|` by construction (varies per seed, `37 + 41*(seed % 10)`) |
| `golden_nR` / `golden_ids_r` | `1024` / `<1024 ids>` | the Receiver-role id set, CONCRETE |
| `golden_nS` / `golden_ids_s` | `1024` / `<1024 ids>` | the Sender-role id set, CONCRETE |
| `golden_bucket_count` | `<K>` | number of `golden_bucket` rows (= `\|ids_r Δ ids_s\|`) |
| `golden_bucket` | `<id> <sign 0/1> <d1..d7> <t_beta>` | one symmetric-difference id: `sign` 0 = R-only (+, this id's real bucket gets `+table.row`), 1 = S-only (-); `d1..d7` = `Ref.syndromes` (R-minus-S convention, matching `query.cpp`'s `build_syndrome`); `t_beta` = `Ref.t_of(d)`, always `1` for a lone id (MIN-1's rank-1 identity) |
| `golden_t` | `<t>` | aggregate rank, `sum(t_beta)` over every `golden_bucket` row |
| `golden_count` | `<n>` | `(golden_nR + golden_nS - golden_t) / 2`, cross-checked in the generator against `len(set(ids_r) & set(ids_s))` before writing |

**Two-layer C++ consumption (R6-GOLDDEPTH):** all 10 seeds run through a
CHEAP layer (`test/protocols/kat_golden_cheap.cpp`: real `core/table` +
`protocols/update` table construction, real `gates/minors.hpp`
`MinorCircuit` rank recovery over DEALER-style test triples -- no OT/
Setup-produced pools, no full `Query::run` dispatch; measured runtime for
all 10 seeds: see task-21-report.md). ONE seed (9, the largest configured
overlap, i.e. the smallest union-bucket count among the 10) additionally
runs through a FULL layer (`test/protocols_heavy/kat_golden_full.cpp`,
`heavy` label): the real two-party `Query::run` over a real TCP `Channel`,
with `Setup`-produced (OT/silent-VOLE) pools, asserting the opened count
equals `golden_count`.

## `overflow_planted.fixture` (task-21-brief.md W6.3, R6-OVFDEMO)

Golden source: `ref/reference.py`'s `Ref.build_overflow_demo_data()` -- the
SAME planted-5-ids-in-one-bucket construction `_selftest_overflow()`
(task-18-brief.md SC5/FC3) already proves at import time, factored out so
both share one definition instead of two hand-maintained copies. Regenerate
with:

```
python3 ref/reference.py emit-ovf --out test/fixtures/overflow_planted.fixture
```

**SCOPE (R6-OVFSCOPE, binding):** this fixture and everything that consumes
it are PLAINTEXT/REFERENCE-ONLY. No MPC overflow detector exists anywhere
in this codebase; `src/protocols/salt.cpp`'s `OverflowChecker::check` is a
`[[noreturn]]` stub that always aborts. `Ref.detect_overflow` is the ONLY
working implementation of the deep-scan overflow check anywhere in this
codebase (task-18-brief.md W5.6 scope decision).

| key | values | meaning |
|---|---|---|
| `ovf_bucket` | `777` | the synthetic bucket 5 ids are planted into |
| `ovf_planted_ids_count` / `ovf_planted_ids` | `5` / `<5 ids>` | the planted ids (true multiplicity 5) |
| `ovf_pre_d` | `<d1..d13>` | the depth-`2*7-1=13` syndrome vector at the planted bucket, pre-refresh |
| `ovf_pre_rank` | `5` | `Ref.detect_overflow(..., t_prime=7)`'s recovered rank at the planted bucket |
| `ovf_pre_flagged` | `1` | whether the planted bucket is in `overflowing_buckets` at `t_prime=7` (SC4) |
| `ovf_t4_rank` | `<=4` | recovered rank at `t_prime=4` (FC3: a depth-7 Hankel test cannot report a rank above 4) |
| `ovf_t4_flagged` | `0` | whether flagged at `t_prime=4` -- must be `0` (FC3: no deep headroom, no detection) |
| `ovf_post_flagged_any` | `0` | whether ANY bucket is flagged after a salt-refresh dispersal (FC4) |
| `ovf_post_bucket_present` | `0` | whether the planted bucket (777) still appears in the post-refresh detail at all (it does not: the refreshed oracle disperses the 5 ids to 5 distinct buckets) |

**Consumer (SC4/SC5):** `test/overflow/gen_overflow_demo.py`, registered
directly via `add_test` (ctest name
`overflow.PlantedFiveIdsFlagAtT7AndClearAfterRefresh`) since
`detect_overflow` has no C++ production call site to exercise from a gtest
binary. It loads this fixture, recomputes fresh via
`Ref.build_overflow_demo_data()` (catching numeric drift), asserts SC4/FC3/
FC4 directly, and on success (re)generates `docs/overflow_demo.md` -- see
that script's own top comment and `docs/overflow_demo.md`'s own opening
disclaimer paragraph for the full R6-OVFSCOPE statement.

## `generator_g.txt` (ENC-5, `[POST-GATE]`)

Separate from the per-seed fixtures: a single line, `37`, pinning the
generator `Encoder` computes. Generated once from a real (compiled)
`Encoder()` run and independently cross-checked by
`ref/reference.py`'s `find_generator()` before being committed — see
`task-3-report.md` for that cross-check's output. `test/utils/kat_encoding.cpp`'s
`ENC5_GeneratorPinnedGolden` test only compares against this file; it does
not regenerate it.
