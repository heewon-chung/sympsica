# baselines/acns2147/PROVENANCE.md — eprint 2025/2147 (ACNS'26, Alborch et al.)

- URL: https://eprint.iacr.org/2025/2147.pdf
- Accessed: 2026-08-21T01:49:40Z
- Size: 701,656 bytes (measured with `wc -c`); PDF version 1.5; 30 pages
- SHA-256: `d479dccb9a7cee809321bce6c076e1d1d8fadaf015838222338ba1580686d172`

**NOT committed** (R8-2147-PDF: third-party copyright). The PDF and its `pdftotext -layout`
extraction (`acns2147.pdf` / `acns2147-extracted.txt`) live beside
`.superpowers/sdd/sympsica-plan/ACNS2147-EVIDENCE.md`, a directory this repo's `.gitignore`
already excludes wholesale (`.superpowers`). `derived.py`'s Tables 3/4 transcription (§ W8.6)
is therefore reproducible from this file plus `ACNS2147-EVIDENCE.md`, without re-fetching the
PDF.

## Facts this file re-verifies from the PDF (W8.6 re-verification, 2026-08-21)

- **§7.1 substrate sentence**: Intel Core i7-7800X (6C/12T, 3.5 GHz, AVX-512), 128 GB RAM,
  Ubuntu 20.04; the two parties run as separate **THREADS** communicating through localhost,
  network shaping via Linux `tc` — not our per-party-process + netns-pair harness. Recorded
  verbatim in every derived row's `notes` (`substrate=i7-7800X-128GB-Ubuntu20.04-two-THREADS-`
  `over-localhost-tc`).
- **The α/Paillier characterization**: α is a PUBLIC THRESHOLD update-set size, not a
  per-party secret budget — update sets are PADDED to reach the common threshold α, so the
  size of an update is no longer leaked (paper lines 558/661, 534/710). Paillier
  (kzen-paillier 0.4.3) is the AHE inside Π_CnP (combine-and-permute): P1 encrypts its shares,
  P2 reconstructs under encryption and shuffles, returns for P1 decryption ([13, Appendix C]).
- **Protocol identity**: Protocol 2 = Π_uPSI-card, the CA variant (line 659); Protocol 1 is
  full uPSI (line 557) and is transcribed with `"variant":"uPSI(not CA)"` — never claimed
  CA-comparable. Their `[4]` (line 1355, Badrinarayanan–Miao–Shi–Tromanhauser–Zeng,
  "Updatable ...") is the same BMS+24 baseline this phase wraps in `baselines/bms24/`.
- **The network-label hazard and A1**: 2147 labels its WAN columns by BANDWIDTH ONLY
  (`200 Mbps` / `50 Mbps` / `5 Mbps`, with the space) and never restates an RTT for them in
  the Table 3/4 captions — our harness's WAN200/WAN50/WAN5 profiles assume 80 ms RTT, a fact
  2147 does not state for its own numbers. Controller amendment A1 (phase-8-plan.md) requires
  every derived row to carry the source's VERBATIM label in `config.network` (never relabeled
  onto `WAN200`/`WAN50`/`WAN5`) plus an explicit `rtt_caveat=source-table-states-bandwidth-only`
  notes token — see `derived.py`'s `NETS` tuple and `NOTES_COMMON`.

## Live repo pin

`gitlab.eurecom.fr/project-spring-2025/PSI` main @ `39558fde926b999d569e7a20661e55e5753e0772`
(recorded for reference only — W8.6 transcribes the published PDF tables, not this repo; no
code from it is vendored or built).

## Transcribed output

`baselines/acns2147/derived.py --out <path>` writes 48 DERIVED rows (Table 3: 6 rows ×
4 networks = 24; Table 4: 6 rows × 4 networks = 24), byte-deterministic across runs:

- SHA-256 of the output file: `ef4b2465c4952a6da628dab32e4e3fbc3a59b22a35a46e80f4b7bc9ea5159539`
  (verified this task: two independent runs produced byte-identical files at this hash).
- Every line passes `bench/jsonl_check.py`'s `validate_record` (amendments A1 + A3: `env`
  `"DERIVED"`, `scenario` `"derived"`, `r_out`/`s_out` = 0, `total == external_total`, the
  table name riding in `notes` as `table=T3|T4`, never in `scenario`).
- Table 2 (line 857, the only published 2147-vs-BMS+24 head-to-head) is recorded in
  `ACNS2147-EVIDENCE.md` for Phase-10's reference but is OUT of W8.6/Phase-8 scope — not
  transcribed here.

See `baselines/acns2147/test_derived.py` (`bench.Acns2147Derived` ctest gate) for the pinned
cell-level assertions and `task-32-report.md` for the R6-NOTAUTO negative demonstrations
(line-count, cell-value, and SHA drift).
