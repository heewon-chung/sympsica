# test/fixtures/ — Phase-1 KAT fixtures

Golden source: `ref/reference.py` (pure Python, bigints). Every file here is
generated, never hand-edited. Regenerate with:

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
| `tbl1` | `<id_a> <id_b> <s1> .. <s7>` | `Ref.power_sums([id_a, id_b], g, 7)` — a future-phase (table layer) fixture, emitted now per plan text; nothing in Phase 1 consumes it. |

## Pinned PRNG: splitmix64

Both `ref/reference.py` (`splitmix64_next`) and
`test/utils/fixture_support.hpp` (`splitmix64_next`) implement the same
public-domain splitmix64 generator (Sebastiano Vigna) with identical mixing
constants, so a `(prng_seed, count)` pair regenerates byte-identical
sequences in either language without shipping the raw samples.

## `generator_g.txt` (ENC-5, `[POST-GATE]`)

Separate from the per-seed fixtures: a single line, `37`, pinning the
generator `Encoder` computes. Generated once from a real (compiled)
`Encoder()` run and independently cross-checked by
`ref/reference.py`'s `find_generator()` before being committed — see
`task-3-report.md` for that cross-check's output. `test/utils/kat_encoding.cpp`'s
`ENC5_GeneratorPinnedGolden` test only compares against this file; it does
not regenerate it.
