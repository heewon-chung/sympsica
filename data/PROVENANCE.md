# data/PROVENANCE.md — OFAC SDN 2023 changes file

- URL: https://www.treasury.gov/ofac/downloads/sdnnew23.txt
- Accessed: 2026-08-20 (HTTP 200, text/plain,
  last-modified: Thu, 28 Dec 2023 15:06:24 GMT — a frozen 2023 archive)
- Size: 2,710,116 bytes, 46,384 lines
- SHA-256: 92f04e19c0082a1fdae778f88960180ca1e4ff037af40773b49ce56fd5f35920
  (verify: `cd data && shasum -a 256 -c SHA256SUMS`)

## The pinned parse convention (.handoff/sympsica-plan.md W7.1, verbatim)

> an entry ends at a line whose last non-whitespace char is '.'; count every entry under
> add/remove/changed action-block headers (headers accumulate wrapped lines until the
> terminating ':'); every `changed` entry counts as delete + add (2 ops); NO abbreviation
> filtering.

Amendments pinned by the Phase-7 controller pre-flight (progress.md, 2026-08-20), binding:

- Percentiles over the 114 per-day op counts: median = mean of the two middle order
  statistics; p90 = linear interpolation at index 0.9*(n-1) over the sorted counts; then
  Python round() (banker's rounding) on both. Raw 41.5 -> 42, raw 180.7 -> 181.
- Entity ids: low 60 bits of stdlib hashlib.blake2b (digest_size=8, little-endian) over the
  entry text (lines .strip()-ed, joined with single spaces). BLAKE3 is not available; no
  cross-language equality depends on this choice. Real ids: bit 60 clear. Synthetic base
  (filler) ids: bit 60 set, low 60 bits uniform, the single value 2^61-1 (== p) excluded.
- A `changed` entry splits at the FIRST " -to- ": del = hash(old half), add = hash(new
  half); if no " -to- " is present (923 real occurrences), del and add both hash the
  full entry text.
- Deletes of ids never inserted in 2023 are mapped at emission time to pre-seeded base
  members: the c-th unknown delete maps to base_id(seed=1, index=c) (data/gen_base.py).
  1800 mappings total; table in data/deltas/mapping.json.

Pinned observables under this convention (test data.OFC1 asserts them exactly):
days=114, total=11499, median=42, p90=181, max=961.

The 114 per-day delta files under data/deltas/ and mapping.json are COMMITTED, deterministic
artifacts of `python3 data/parse_ofac.py --raw data/sdnnew23.txt --out-dir data/deltas
--base-seed 1`; data.OFC3 asserts committed == regenerated.
