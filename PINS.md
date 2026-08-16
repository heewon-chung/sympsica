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

Machine-readable pin consumed by the CMake W0.4 guard (`CMakeLists.txt`) — do not
reformat this line, it is parsed verbatim:

- libOTe: d644366cd6de1e47f8cfce49b372db90eeff0764

| Repo | SHA |
|---|---|
| libOTe (osu-crypto/libOTe) | `d644366cd6de1e47f8cfce49b372db90eeff0764` |

## Vendored via libOTe's own git submodule (pinned transitively by the libOTe SHA above)

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

## `vendor/` git-ignore choice

`vendor/**` is committed to disk (with each vendored repo's own `.git` directory
intact, since the W0.4 CMake guard needs `git -C vendor/libOTe rev-parse HEAD` to
work) but the outer `sympsica` repo `.gitignore`s `vendor/` so it does not try to
commit gigabytes of vendored git history. This file (the SHAs above) plus the W0.4
configure-time guard are the reproducibility record — re-vendor via the pinned SHAs
above rather than expecting `vendor/` to be tracked by git.

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
