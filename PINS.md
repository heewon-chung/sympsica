# PINS.md — vendored / fetched dependency pins

This file is the reproducibility record for `sympsica`'s external dependencies.
`vendor/` is git-ignored (see below), so this file — plus the CMake configure-time
guard in `CMakeLists.txt` that checks `vendor/libOTe`'s checked-out commit against
the `libOTe:` line below — is the source of truth for exact versions. To re-vendor:

```
git clone https://github.com/osu-crypto/libOTe vendor/libOTe
git -C vendor/libOTe checkout <sha below>
cd vendor/libOTe && python3 build.py --setup
```

`python3 build.py --setup` configures libOTe's CMake project with `-DFETCH_AUTO=ON`
but does not build/install libOTe itself (the `--setup` flag skips the build step);
the CMake configure step fetches libOTe's own default thirdparty dependencies as a
side effect (SEAL is additionally built + installed by its own fetch script during
this configure step, since that happens synchronously regardless of `--setup`).

## Vendored (git clone; checked out, git history intact on disk)

Machine-readable pins consumed by the CMake W0.4/task-4 guard (`CMakeLists.txt`) — do
not reformat these lines, they are parsed verbatim:

- libOTe: d644366cd6de1e47f8cfce49b372db90eeff0764
- cryptoTools: 1a344ee4b7f4afaf39193eb413300ba17962b19e

| Repo | SHA |
|---|---|
| libOTe (osu-crypto/libOTe) | `d644366cd6de1e47f8cfce49b372db90eeff0764` |

## Vendored via libOTe's own git submodule (pinned transitively by the libOTe SHA above)

Task 4 (obligation (e)) extended the W0.4 configure-time guard to also check this
checkout's HEAD against the `cryptoTools:` line above (`git -C
vendor/libOTe/cryptoTools rev-parse HEAD`), verified exact at `1a344ee4b7f4afaf39193eb413300ba17962b19e`
(same value already recorded here since Phase 0/1 — no re-vendor was needed, only the
CMake check was missing). Mismatch => `FATAL_ERROR`, same as the libOTe check.

| Repo | SHA |
|---|---|
| cryptoTools (ladnir/cryptoTools, libOTe's submodule) | `1a344ee4b7f4afaf39193eb413300ba17962b19e` |

## Fetched by `python3 build.py --setup` (libOTe's default thirdparty; recorded from
## the checkouts left under `vendor/libOTe/out/`)

| Repo | SHA |
|---|---|
| coproto (ladnir/coproto) | `ded64cbc51c041ee534e5c34a5a81135af455ce7` |
| macoro (ladnir/macoro) | `91fc6b42ff719c681713c6d9f7476b94fd822983` |
| function2 (Naios/function2) | `02ca99831de59c7c3a4b834789260253cace0ced` |
| libdivide (ridiculousfish/libdivide) | `66190e9daa603cabe95d99f09fb79b5c186d0417` |
| SEAL (microsoft/SEAL) | `206648d0e4634e5c61dcf9370676630268290b59` (built + installed under `vendor/libOTe/out/install/osx` by the fetch step) |

Note: `thirdparty/KyberOT` is vendored in-tree inside the libOTe repo itself (not a
separate fetch), so it is already covered by the libOTe SHA above.

## Fetched by top-level CMake via `FetchContent` (see `CMakeLists.txt`)

| Dependency | Tag |
|---|---|
| BLAKE3 (official C implementation, BLAKE3-team/BLAKE3) | `1.8.6` (latest release tag as of 2026-08-16) |
| googletest (google/googletest) | `v1.18.0` (latest release tag as of 2026-08-16) |

## Boost (task-4 brief, obligation (b), controller ruling order 1)

Fetched transitively by libOTe's own thirdparty machinery when the top-level
`CMakeLists.txt` sets `ENABLE_BOOST=ON` + `FETCH_AUTO=ON` before
`add_subdirectory(vendor/libOTe)`: `cryptoToolsDepHelper.cmake`'s `FIND_COPROTO`/
`FETCH_COPROTO_IMPL` path pulls in `vendor/libOTe/out/coproto` (already vendored at
the pinned SHA above) via `add_subdirectory`, which — because `COPROTO_ENABLE_BOOST`/
`COPROTO_FETCH_BOOST` are propagated from `ENABLE_BOOST` — runs coproto's own
`thirdparty/getBoost.cmake` at configure time. That script downloads and builds (via
`bootstrap.sh` + `b2`, NOT CMake) the following exact Boost release, entirely
independent of Homebrew:

| Dependency | Version | Source | SHA256 |
|---|---|---|---|
| Boost (boost.org, official release tarball) | `1.90.0` | `https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2` | `49551aff3b22cbc5c5a9ed3dbc92f0e23ea50a0f7325b0d198b705e8ee3fc305` |

Components built: `system, thread, atomic, filesystem, regex, date_time` (per
`getBoost.cmake`'s `B2_ARGS`). Installed under
`vendor/libOTe/out/install/osx` (`OC_THIRDPARTY_HINT`/`COPROTO_STAGE`), alongside the
already-vendored SEAL/function2 installs. This is the "prefer coproto/libOTe's own
boost integration" path from the controller ruling — the Homebrew fallback (ruling
order 2) was **not** needed; the fetch-and-build succeeded on macOS/ARM inside the
~45-minute bound (see task-4-report.md for the timed run).

## New CMake knobs (task-4 brief, obligation (c)) — set via `CACHE ... FORCE` in the
## top-level `CMakeLists.txt` before `add_subdirectory(vendor/libOTe)`

| Knob | Value | Why |
|---|---|---|
| `SYMPSICA_BUILD_LIBOTE` | `ON` (default) | sympsica-local option gating the real libOTe build (vs. a future header-only/mock mode) |
| `ENABLE_BOOST` | `ON` | required for coproto's TCP (`AsioSocket`) backend — obligation (b) |
| `FETCH_AUTO` | `ON` | lets libOTe/cryptoTools's own dep helpers reuse the already-vendored `out/{coproto,macoro,function2,libdivide,seal-4.1.1}` checkouts instead of failing a `find_package` |
| `ENABLE_SIMPLESTOT` | `ON` | Chou-Orlandi base OTs — spec-mandated (brief text), and `RegularDpf`'s `DpfMult` multiplier is built on top of base OTs |
| `ENABLE_SILENTOT` | `ON` | Silent OT extension (needed transitively for `ENABLE_PPRF`, and for the SilentOT triples Phase 2 will use) |
| `ENABLE_SOFTSPOKEN_OT` | `ON` (overriding libOTe's own default `OFF`, `cmake/buildOptions.cmake:88`) | **required for `ENABLE_SILENTOT` to be usable at runtime, not just linkable** (added by task-5). `SilentOtExt{Sender,Receiver}::genBaseCors()` — the path taken whenever `setBaseCors()` is not called by hand — is compiled inside `#if defined(ENABLE_SOFTSPOKEN_OT) && defined(LIBOTE_HAS_BASE_OT)`; with SoftSpoken off, the `#else` arm is a bare `throw std::runtime_error("KOS or base OTs must be enabled")` (`Silent/SilentOtExtSender.cpp:180`). With it on, silent OT bootstraps from `SimplestOT` through SoftSpokenOT. Task-4's knob set left silent OT linkable but throwing on first use |
| `ENABLE_SILENT_VOLE` | `ON` | Silent VOLE (also sets `ENABLE_PPRF`) — NoisyVole is the non-silent core `SilentVole` composes with |
| `ENABLE_REGULAR_DPF` | `ON` | `oc::RegularDpf` (the DKG protocol Phase 2/W2.x targets) — also pulls in `Dpf/SumDmpf.cpp` |
| `ENABLE_LOGVOLE` | `OFF` (overriding libOTe's own default `ON`) | LogVole is SEAL-backed and out of scope for sympsica; turning it off avoids an unnecessary heavy dependency even though SEAL 4.1.1 is already vendored/built |
| `ENABLE_SSE` | mirrors `SYMPSICA_SSE` (OFF on this host's arm64) | maps sympsica's existing architecture-conditional option onto libOTe/cryptoTools's own SSE knob, per the W0.3 comment's stated intent |

## `vendor/` git-ignore choice

`vendor/**` is committed to disk (with each vendored repo's own `.git` directory
intact, since the W0.4 CMake guard needs `git -C vendor/libOTe rev-parse HEAD` to
work) but the outer `sympsica` repo `.gitignore`s `vendor/` so it does not try to
commit gigabytes of vendored git history. This file (the SHAs above) plus the W0.4
configure-time guard are the reproducibility record — re-vendor via the pinned SHAs
above rather than expecting `vendor/` to be tracked by git.

## google/distributed_point_functions (task-7 brief, W2.3 part (b))

Vendored for the ZT-4 cross-implementation SEMANTIC check's part (b) — a
Bazel/bzlmod-only project (no `CMakeLists.txt` anywhere in the repo), so it
is built and run entirely outside the CMake project, from its own small
bzlmod workspace at `test/gates/oracle_dpf/google_dpf/` (see that
directory's `MODULE.bazel` for the exact rationale). The CMake configure-time
SHA guard (`CMakeLists.txt`) checks this checkout's `HEAD` against the pin
below whenever `vendor/distributed_point_functions/` exists, mirroring the
libOTe/cryptoTools guard, but does not require the checkout to exist (this
vendored tree is not a build dependency of the CMake project itself).

```
git clone https://github.com/google/distributed_point_functions vendor/distributed_point_functions
git -C vendor/distributed_point_functions checkout <sha below>
```

| Repo | SHA |
|---|---|
| distributed_point_functions (google/distributed_point_functions) | `859cafa71fc1e139c7b76d4d4c0f23438688a8ad` |

Machine-readable pin (parsed verbatim by the same CMake guard function as the
libOTe/cryptoTools lines above):

- distributed_point_functions: 859cafa71fc1e139c7b76d4d4c0f23438688a8ad

Commit date `2026-01-05` (per `git log -1 --format='%ci'`); no tagged release
exists upstream (`git tag -l` returns only `v0.0.0`), so the tip of the
default branch was pinned.

### Build path taken: 2 (bazelisk), with two host-toolchain pins

Per task-7-brief.md's fallback ladder: (1) the library's own CMake build was
tried first and is definitionally unavailable — there is no `CMakeLists.txt`
anywhere in the checkout (`find . -iname CMakeLists.txt` returns nothing;
confirmed by attempting `cmake -S vendor/distributed_point_functions -B ...`,
which fails immediately with "no CMakeLists.txt found"). (2) `brew install
bazelisk` (host pin: **bazelisk 1.29.0**, `bazel --version` reports **9.2.0**
as bazelisk's *default* resolved release) was used next, and — after two
host-toolchain pins below — succeeded: both `//dpf:distributed_point_function`
(the full DPF keygen/eval library) and this task's own driver
(`test/gates/oracle_dpf/google_dpf:oracle_check`) build and run cleanly.
Rung 3 (STOP, write an unrun driver) was **not** needed.

1. **Bazel release pinned to 7.4.1 via `USE_BAZEL_VERSION=7.4.1`** (a
   bazelisk-native environment variable — no file inside the vendored
   checkout is touched). Root cause, verified by direct inspection: bazelisk's
   default Bazel release (9.2.0) has fully removed the native `cc_proto_library`
   global rule (`dpf/BUILD:138` calls `cc_proto_library(...)` without any
   `load(..., "cc_proto_library")` — every other rule in that file IS
   explicitly loaded, e.g. `load("@rules_cc//cc:cc_library.bzl",
   "cc_library")`, so this is a genuine gap in the vendored file, not a
   version-resolution artifact: deleting and letting bzlmod regenerate
   `MODULE.bazel.lock` from scratch reproduces the identical error). Bazel
   7.4.1 still resolves `cc_proto_library` as a native global, so the same
   `dpf/BUILD` builds unmodified. This is a build-tool version choice, not a
   change to the vendored checkout — `vendor/distributed_point_functions/`
   itself was never edited (verified: `git status` in that checkout shows
   only `MODULE.bazel.lock` touched, from bazel's own dependency-resolution
   cache regeneration, and that file was restored to its pristine
   post-clone state afterward; see below).
2. **`--copt=-DHWY_DISABLED_TARGETS=25952256`** — the full DPF library
   (`dpf/internal/evaluate_prg_hwy.cc`, `dpf/internal/aes_128_fixed_key_hash_hwy.h`)
   depends on `@highway//:hwy` (google/highway 1.2.0, pinned by
   `distributed_point_functions`'s own `MODULE.bazel`) for its SIMD dispatch.
   Without this flag, compiling for ARM's SVE/SVE2 dispatch targets fails
   with `error: requested alignment is less than minimum alignment of 16 for
   type 'absl::uint128'` (`HWY_ALIGN` resolves to `alignas(8)` for those
   targets in this highway version, narrower than `absl::uint128` needs) —
   this is exactly the ARM incompatibility task-7-brief.md's parenthetical
   ("ARM needs HWY_DISABLED_TARGETS") named. Apple Silicon implements NEON,
   not SVE/SVE2 (an ARM server-class extension), so disabling them is also
   the semantically correct choice, not just a workaround: `25952256` =
   `HWY_SVE2_128 (1<<18) | HWY_SVE_256 (1<<19) | HWY_SVE2 (1<<23) | HWY_SVE
   (1<<24)` (bit values from `external/highway~/hwy/detect_targets.h` in the
   fetched highway checkout). With this flag, `dpf->` reports "Highway is in
   NEON mode" at runtime (confirmed in the driver's own log output) and both
   targets build and pass.

Full reproduction, from `vendor/distributed_point_functions/`:

```
brew install bazelisk   # host pin: bazelisk 1.29.0
USE_BAZEL_VERSION=7.4.1 bazelisk build --copt=-DHWY_DISABLED_TARGETS=25952256 \
    //dpf:distributed_point_function
```

or, for this task's actual check (from
`test/gates/oracle_dpf/google_dpf/`):

```
USE_BAZEL_VERSION=7.4.1 bazelisk test --copt=-DHWY_DISABLED_TARGETS=25952256 \
    //:oracle_check
```

`MODULE.bazel.lock` note: a stray first `bazelisk build` attempt (made
before either pin above was known) regenerated this checked-out file as a
side effect of dependency resolution even though the subsequent build
failed; it was restored to the exact bytes present immediately after
`git clone` before this task finished (this is bazel's normal
dependency-resolution cache, analogous to `python3 build.py --setup`
regenerating libOTe's own `config.h` files in-tree — not a change to the
pinned commit's source).

## Phase 8 — baseline images and sources (phase-8-plan.md)

Re-verified 2026-08-21 (controller + Task 32 implementer); consumed by `baselines/bms24/`,
`baselines/fastupsi/`, `baselines/kunlun/` (stub), and `baselines/acns2147/` (derived,
transcription only — no source is built for it).

| What | Pin |
|---|---|
| BMS+24 image | `ghcr.io/ruidazeng/upsi-revisited@sha256:221cfc61a843c928a0fba256b85f35a5ace5dd7f1ff91e144359a8942144cd8d` (amd64; 1,246,019,325 B; prebuilt; WorkingDir `/home/upsi-user`) |
| BMS+24 source | `ruidazeng/upsi-revisited` @ `d6ddfb59bf068fdd702df118b4d3fbe0b36d866c` |
| FastUPSI image | `ghcr.io/qqqqyyy/fastupsi@sha256:47610af00af65d354f8523305ab8bb61649d53bece2fcc7b989ce9c2f94da586` (index, linux/amd64 only; 446,586,115 B; prebuilt; WorkingDir `/home/fastupsi`) |
| FastUPSI source | `qqqqyyy/FastUPSI` @ `86ab38ae8982d1b59b12b21acf37f4b583b9b3e2` (2026-04-13) |
| Kunlun source | `yuchen1024/Kunlun` @ `395cf8ec48e73d9ab6906b51a85e2dbb41929b92` (NOT probed — R8-KUNLUN) |
| volePSI source | `Visa-Research/volepsi` @ `ec76012ed516e25d3f460af9b8680e1140a5d491` (archived 2025-09-03) |
| minisketch source | canonical **`bitcoin-core/minisketch`** @ `4a179c61e3cbe3ac2b3c027764ce8eb5183155e1` (2026-08-14, tip-at-spec-time, not a release; the handoff's `sipa/minisketch` URL is a 301 redirect — stale) |
| eprint 2025/2147 PDF | SHA-256 `d479dccb9a7cee809321bce6c076e1d1d8fadaf015838222338ba1580686d172`, 701,656 B (`wc -c`), fetched 2026-08-21T01:49:40Z (NOT committed) |
| ACNS 2147 live repo | `gitlab.eurecom.fr/project-spring-2025/PSI` main @ `39558fde926b999d569e7a20661e55e5753e0772` |
| kunlun | NOT built (R8-KUNLUN); openssl tarball sha256: TBD at first build |

## CMake compatibility notes

- Host used for verification: macOS ARM (Darwin 25.5). cmake was upgraded from the
  environment's stock 4.4.0 to 4.4.2 (via `brew upgrade cmake`) while diagnosing the
  issue below; 4.4.2 did **not** resolve it, so the CMakeLists.txt workaround below
  is still required regardless of this exact cmake patch version.
- No `CMAKE_POLICY_VERSION_MINIMUM` override was needed: BLAKE3 1.8.6's `c/CMakeLists.txt`
  declares `cmake_minimum_required(VERSION 3.9...3.18 FATAL_ERROR)` and googletest
  v1.18.0 declares `cmake_minimum_required(VERSION 3.16)`; both are >= 3.5, which is
  the floor CMake 4.4 still supports without the override.
- A different, unanticipated compatibility issue *was* needed: this host's system
  Clang identifies as `AppleClang 21.0.0.21000101` (via Xcode Command Line Tools),
  which is newer than any AppleClang version cmake 4.4.x ships compiler-feature
  tables for. BLAKE3's `c/CMakeLists.txt` calls `target_compile_features(blake3
  PUBLIC c_std_99)` and `target_compile_features(blake3 PUBLIC cxx_std_20)`, and with
  an unrecognized AppleClang version cmake's internal `CMAKE_C_COMPILE_FEATURES` /
  `CMAKE_CXX_COMPILE_FEATURES` come back empty, which fails the *generate* step with
  `CMake Error ... No known features for C compiler` (blank compiler id/version in the
  message — likely because the error is raised while resolving standard requirements
  across all languages used by the `blake3` target, including its ASM sources).
  `CMakeLists.txt` works around this by pre-seeding `CMAKE_C_COMPILE_FEATURES` and
  `CMAKE_CXX_COMPILE_FEATURES` as `CACHE INTERNAL` entries (gated to
  `CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"` so other toolchains, e.g. the
  untested gcc:13 Linux leg, keep cmake's normal detection) before the C language is
  enabled. This is a genuine environment/toolchain compatibility gap, not a
  sympsica design choice — if this stops being needed once cmake ships AppleClang 21
  feature tables, the guarded block can be deleted.
- Separately, `add_compile_options(-std=gnu++20)` from the plan text had to be scoped
  to `$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>` (a generator expression) because it
  otherwise applies to every language in the project, including BLAKE3's C sources
  pulled in via `FetchContent`, which reject a C++ dialect flag
  (`invalid argument '-std=gnu++20' not allowed with 'C'`).
- W1.2 (`include/sympsica/utils/coeff_ctx.hpp`, Phase 1): the `sympsica` CMake
  target's include paths now also cover `vendor/libOTe`, `vendor/libOTe/cryptoTools`,
  and their CMake-generated `config.h` locations (`vendor/libOTe/out/build/osx/libOTe`,
  `vendor/libOTe/out/build/osx/cryptoTools`) so `coeff_ctx.hpp` can hard-include
  `libOTe/Tools/CoeffCtx.h`. The two `out/build/osx/...` paths are HEADER-ONLY paths
  into files that already exist on disk as a side effect of the `python3 build.py
  --setup` re-vendor step above (libOTe's own CMake configure runs during that step
  even though `--setup` skips the actual build) — no new external pin, no libOTe/
  cryptoTools library target is built or linked. **Portability gap**: `out/build/osx`
  is macOS-specific (libOTe's `build.py` names this directory after the host OS); an
  x86-64 Linux build will need `out/build/linux` (or whatever libOTe's `build.py`
  names it there) — unverified, since only the macOS/ARM leg has been exercised (same
  gap noted for W0.x below). If re-vendoring ever changes what `build.py --setup`
  generates here, these two include paths will need to move with it.
- W1.2 also surfaced that `libOTe/Tools/CoeffCtx.h`'s generic `binaryDecomposition()`
  constructs a real `osuCrypto::BitVector` via a constructor whose implementation
  lives in `cryptoTools/Common/BitVector.cpp` — not compiled/linked by this project
  (consistent with Phase 0's ruling to leave libOTe/cryptoTools unbuilt). `coeff_ctx.hpp`
  declares `binaryDecomposition()` (it compiles, since C++ templates are only
  instantiated when called), but nothing in this repo currently calls it. Phase 2,
  when it actually wires RegularDpf/NoisyVole, will need to either compile
  `BitVector.cpp` (and whatever it transitively needs) or provide a lighter
  alternative.
- W0.5 FC caveat (verified empirically, not fixed — `field.hpp`'s content is fixed
  verbatim by the task brief): on this host's toolchain (AppleClang 21 + libc++),
  `include/sympsica/utils/field.hpp`'s `static_assert` compiles successfully under
  plain `-std=c++20`, not only `-std=gnu++20`. `numeric_limits<unsigned __int128>`
  is apparently provided unconditionally by libc++, unlike libstdc++, which
  typically gates it behind `__STRICT_ANSI__`. So the plan's FC ("non-GNU dialect ->
  compile FAILS (W0.5)") does not actually hold on this host/toolchain combination;
  it may still hold with libstdc++ (e.g. the gcc:13 Linux leg, unverified here).

## Task 33 (W8.4+W8.5) — native driver images, built by image ID (colima x86 VM)

`baselines/volepsi/` and `baselines/minisketch/` are built locally on the colima `x86`
VM from the pinned sources already recorded above (`volePSI source` /
`minisketch source` rows); the images are executed BY DIGEST/local image ID
everywhere (`smoke.json` `"image"`, `run.sh`'s `check_image_label` gate), never by
tag — the tag (`sympsica/volepsi:ec76012`, `sympsica/minisketch:4a179c6`) is an
informational alias only, matched against the `sympsica.source_sha` LABEL baked
into each Dockerfile.

| What | Pin |
|---|---|
| volepsi driver image ID (colima build, 2026-08-22) | `sha256:7a053c82c6ac4a89c47051cfc9a11efbc4ebfa33c316bed592eace05c171ef7d` (2.79 GB) |
| minisketch driver image ID (colima build, 2026-08-21) | `sha256:99ec6bb3742a91dd44c4e282c64d9478ca9432bd111e443c8f5977e68305a83c` (840 MB) |

Minisketch canonical-URL note (re-verified at Task 33 implementation time): cloning
the handoff's original `https://github.com/sipa/minisketch` still succeeds — GitHub
serves an HTTP 301 redirect to `https://github.com/bitcoin-core/minisketch`, so the
old URL is not dead, only stale — but the Dockerfile clones the canonical
`bitcoin-core/minisketch` URL directly (no redirect hop) per this file's earlier
`minisketch source` pin. The checked-out commit `4a179c61e3cbe3ac2b3c027764ce8eb5183155e1`
is tip-at-spec-time (2026-08-14), not a release.

Two build-substrate gaps found by direct execution (real command output in
task-33-report.md), both fixed as Dockerfile/CMakeLists.txt corrections — neither
changes the pinned volePSI source commit or `rr22_driver.cpp`'s protocol logic:
1. `baselines/volepsi/Dockerfile`'s apt-get line needed `libtool autoconf automake
   pkg-config` added — cryptoTools's thirdparty fetch runs libsodium's own
   `autogen.sh -s`, which hard-fails without the autotools toolchain.
2. `baselines/volepsi/CMakeLists.txt`'s `find_package(volepsi ...)` /
   `visa::volepsi` never resolve: volePSI's own installed CMake package is named
   `volePSI` (mixed case) with exported target `visa::volePSI`, verified directly
   against the built `/usr/local/lib/cmake/volePSI/{volePSIConfig,volePSITargets}.cmake`
   — `find_package` is case-sensitive on Linux. Corrected to `find_package(volePSI
   REQUIRED HINTS ${VOLEPSI_HINT})` / `target_link_libraries(rr22_driver
   visa::volePSI)`.

Operational finding (colima `x86` VM, informational): repeatedly `kill -9`-ing a
`docker build` client mid-build (to recover from a stuck invocation) leaves its
BuildKit session dangling server-side; enough accumulated dangling sessions over a
long session degrade the daemon until new `docker build` invocations hang
indefinitely in `futex_wait_queue` regardless of cache state. `docker builder prune
-af` did not fix it; `sudo systemctl restart docker` did. Evidence and full timeline
in task-33-report.md.
