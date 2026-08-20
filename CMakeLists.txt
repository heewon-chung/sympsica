cmake_minimum_required(VERSION 3.20)
project(sympsica LANGUAGES CXX)

# task-6 brief, addendum 9 (controller ruling): default to a Release build
# when the user hasn't picked one. An unoptimised (-O0) default build is
# ~50x slower for this project's protocol tests (task-5-report.md measured
# 36.2 s vs 0.76 s for the integration suite) -- fine for W2.1's single test
# file, but it would breach the ~60 s per-file SC budget by roughly the third
# protocol-test file added, and any benchmark taken from a default build
# would be meaningless for W8.0. Only fires when CMAKE_BUILD_TYPE is unset
# (single-config generators); multi-config generators (Xcode, Ninja Multi-
# Config) pick the config at build time and are unaffected.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE not set; defaulting to Release "
                    "(override with -DCMAKE_BUILD_TYPE=Debug|RelWithDebInfo|... )")
endif()

# Workaround (see PINS.md): this host's AppleClang (21.0.0) is newer than the
# compiler-feature tables shipped with cmake 4.4.2, which otherwise makes
# BLAKE3's target_compile_features(... c_std_99 / cxx_std_20) calls fail at
# generate time with "No known features for <lang> compiler". Seed the known
# feature lists as CACHE entries before the C language is enabled (which
# happens later, inside FetchContent_MakeAvailable(blake3 ...)).
#
# UPDATE (task-9, Linux/x86-64 leg): this bug is NOT AppleClang-specific. It
# reproduces identically on cmake 3.25.1 + GCC 13.4.0 on Linux (BLAKE3's
# c/CMakeLists.txt enables ASM alongside C for its hand-written amd64 SIMD
# sources, and CMake's generate-time feature resolution across a mixed
# C+ASM(+CXX) target fails with a blank compiler id/version on that toolchain
# too) -- it is compiler-generic, tied to the C+ASM target shape rather than
# to any one vendor. Widened to also cover GNU; verified end-to-end
# (configure + full build + 44/46 gates green) inside a linux/amd64 gcc:13
# container. See task-9-report.md.
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_C_COMPILE_FEATURES "c_std_90;c_std_99;c_std_11;c_std_17;c_std_23"
        CACHE INTERNAL "workaround: AppleClang 21 not yet in cmake's feature tables")
    set(CMAKE_CXX_COMPILE_FEATURES
        "cxx_std_98;cxx_std_11;cxx_std_14;cxx_std_17;cxx_std_20;cxx_std_23"
        CACHE INTERNAL "workaround: AppleClang 21 not yet in cmake's feature tables")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Scoped to CXX only (generator expression) because FetchContent also pulls in
# BLAKE3's C sources, which reject a C++ dialect flag.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>)

# W0.3: SSE default follows host architecture; maps to libOTe's ENABLE_SSE
# once libOTe is wired into the build (Phase 2).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i386|i686)$")
    option(SYMPSICA_SSE "Enable SSE (maps to libOTe ENABLE_SSE)" ON)
else()
    option(SYMPSICA_SSE "Enable SSE (maps to libOTe ENABLE_SSE)" OFF)
endif()

# ---------------------------------------------------------------------------
# W0.4 (+ task-4 obligation (e)): guard against a mismatched vendored libOTe
# and cryptoTools checkout. Both SHAs are read verbatim from PINS.md and
# checked against the actual `git rev-parse HEAD` of the on-disk checkout;
# a mismatch is a FATAL_ERROR at configure time either way.
# ---------------------------------------------------------------------------
find_package(Git REQUIRED)

function(sympsica_check_vendored_sha _pin_name _checkout_dir)
    file(STRINGS "${CMAKE_SOURCE_DIR}/PINS.md" _sympsica_pins_lines)
    set(_pinned_sha "")
    foreach(_line IN LISTS _sympsica_pins_lines)
        if(_line MATCHES "^- ${_pin_name}: ([0-9a-f]+)")
            set(_pinned_sha "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(_pinned_sha STREQUAL "")
        message(FATAL_ERROR "PINS.md: could not find a '${_pin_name}: <sha>' entry")
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${_checkout_dir}" rev-parse HEAD
        OUTPUT_VARIABLE _actual_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_result
        ERROR_QUIET
    )
    if(NOT _git_result EQUAL 0)
        message(FATAL_ERROR
            "${_checkout_dir} is not a valid git checkout. Re-vendor ${_pin_name} per "
            "PINS.md at the pinned SHA ${_pinned_sha}.")
    endif()
    if(NOT _actual_sha STREQUAL _pinned_sha)
        message(FATAL_ERROR
            "${_checkout_dir} HEAD (${_actual_sha}) does not match the pinned SHA "
            "(${_pinned_sha}) recorded in PINS.md for '${_pin_name}'. Re-vendor at the "
            "pinned SHA.")
    endif()
endfunction()

sympsica_check_vendored_sha("libOTe" "${CMAKE_SOURCE_DIR}/vendor/libOTe")
sympsica_check_vendored_sha("cryptoTools" "${CMAKE_SOURCE_DIR}/vendor/libOTe/cryptoTools")

# task-7 brief, W2.3 part (b): google/distributed_point_functions is vendored
# for the ZT-4 cross-implementation SEMANTIC check's part (b), but it is its
# own separate Bazel/bzlmod project (test/gates/oracle_dpf/google_dpf/), not
# a CMake dependency of this build -- so, unlike libOTe/cryptoTools above,
# its presence is optional here: only guard its SHA when the checkout
# exists, so a tree without vendor/distributed_point_functions/ (e.g. a
# fresh clone that has not run part (b)'s vendoring step) still configures
# cleanly.
if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/distributed_point_functions")
    sympsica_check_vendored_sha("distributed_point_functions" "${CMAKE_SOURCE_DIR}/vendor/distributed_point_functions")
endif()

# ---------------------------------------------------------------------------
# Dependencies fetched via FetchContent (exact tags; see PINS.md)
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    blake3
    GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3.git
    GIT_TAG 1.8.6
    SOURCE_SUBDIR c
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
)
FetchContent_MakeAvailable(blake3 googletest)

# ---------------------------------------------------------------------------
# task-4 obligation (c): wire vendored libOTe as a REAL, option-controlled
# CMake dependency (add_subdirectory + link), replacing Phase 1's
# header-only include-path shim into vendor/libOTe/out/build/osx. See
# PINS.md ("New CMake knobs") for why each knob below is set.
#
# EXCLUDE_FROM_ALL: libOTe's own top-level CMakeLists.txt unconditionally
# add_subdirectory()s libOTe_Tests/ and frontend/ in addition to libOTe/
# itself; EXCLUDE_FROM_ALL propagates to every target defined transitively
# under vendor/libOTe, so only `libOTe` (and its dependency `cryptoTools`) —
# the targets our own `sympsica`/test binaries actually link — are part of
# the default build; libOTe's own tests/frontend are configured (so a
# configure-time error there would still surface) but never compiled.
# ---------------------------------------------------------------------------
# R-NOFILTER (task-11-brief.md): test-only flag that disables Update::apply's
# entire three-stage step-1 filter (src/protocols/update.cpp's #ifdef
# SYMPSICA_NO_FILTER region) so a SEPARATE build dir (build-nofilter/,
# configured with -DSYMPSICA_NO_FILTER=ON) can demonstrate TV-F5. Defaults
# OFF -- the default build (this one) MUST compile the flag off, per
# R-NOFILTER's binding requirement.
option(SYMPSICA_NO_FILTER "TEST-ONLY: disable Update::apply's step-1 filter (TV-F5 negative build)" OFF)

# task-19-brief.md R-FCFLAGS: three more TEST-ONLY wrong-construction flags,
# same SYMPSICA_NO_FILTER precedent (a separate build dir, never defined in
# the default build) -- FC1/FC2/FC5's Phase-5 schedule/E2E-scale legs.
option(SYMPSICA_WRONG_SIGN "TEST-ONLY: Sender uses +row instead of -row (FC1/TV-F8 negative build)" OFF)
option(SYMPSICA_WRONG_CONVERT "TEST-ONLY: receiver-form convert() applied on both roles (FC2/TV-F9 negative build)" OFF)
option(SYMPSICA_STALE_SIZES "TEST-ONLY: round-0's exchanged counterpart size is discarded (FC5 negative build)" OFF)
option(SYMPSICA_WRONG_BOUNDARY "TEST-ONLY: SwitchRule::decide's boundary relaxed to >= (FC4/TV-F12 negative build, R-FC4-REAL)" OFF)

# task-20-brief.md M1 fix round 1 (R6-NOTAUTO): same SYMPSICA_NO_FILTER
# precedent -- a TEST-ONLY flag, default OFF, reproducing the literal FC3
# bug ("double-applying the SAME commit delta drifts t_share") inside
# commit()'s real incremental branch (src/protocols/query.cpp), so
# kat_query.cpp's structural-shape assertion can be demonstrated FAILING
# against a real (not hand-derived) wrong value instead of shipping a
# tautological closed-form check. Not a TV-F row (query.cpp's own #ifdef,
# same as SYMPSICA_STALE_SIZES) -- used for a one-off local verification
# build, not a persistent build-*/ dir (see task-20-report.md Fix round 1).
option(SYMPSICA_DOUBLE_APPLY_COMMIT "TEST-ONLY: commit()'s incremental delta applied twice (M1 fix round 1 negative)" OFF)

option(SYMPSICA_BUILD_LIBOTE "Build vendored libOTe as a real CMake dependency" ON)
if(SYMPSICA_BUILD_LIBOTE)
    # W0.3's SYMPSICA_SSE maps onto libOTe/cryptoTools's own ENABLE_SSE.
    set(ENABLE_SSE ${SYMPSICA_SSE} CACHE BOOL "compile with SSE instructions" FORCE)

    # obligation (b), controller ruling order 1: let coproto's own thirdparty
    # fetch supply boost (see PINS.md, "Boost" section, for the exact
    # version/SHA256 that gets fetched+built).
    set(ENABLE_BOOST ON CACHE BOOL "compile with BOOST networking integration" FORCE)
    # Reuse the already-vendored out/{coproto,macoro,function2,libdivide,seal-4.1.1}
    # checkouts (PINS.md) instead of requiring an installed find_package.
    set(FETCH_AUTO ON CACHE BOOL "automatically download and build dependencies" FORCE)

    # Base OTs + extensions needed by RegularDpf (DKG) + NoisyVole + SilentOT.
    set(ENABLE_SIMPLESTOT ON CACHE BOOL "Chou-Orlandi base OT" FORCE)
    set(ENABLE_SILENTOT ON CACHE BOOL "Silent OT extension" FORCE)
    # REQUIRED by ENABLE_SILENTOT at runtime, not just for its own protocol:
    # SilentOtExt{Sender,Receiver}::genBaseCors() — the path taken whenever
    # setBaseCors() was not called by hand — is compiled inside
    # `#if defined(ENABLE_SOFTSPOKEN_OT) && defined(LIBOTE_HAS_BASE_OT)`; with
    # SOFTSPOKEN off the #else arm just throws
    # "KOS or base OTs must be enabled" (Silent/SilentOtExtSender.cpp:180,
    # SilentOtExtReceiver.cpp likewise). libOTe's own default for this knob is
    # OFF (cmake/buildOptions.cmake:88), so task-4's knob set left silent OT
    # linkable but unusable. Turning it ON lets genBaseCors bootstrap from
    # SimplestOT (ENABLE_SIMPLESTOT, above) through SoftSpokenOT. See PINS.md.
    set(ENABLE_SOFTSPOKEN_OT ON CACHE BOOL "SoftSpokenOT (silent OT base correlations)" FORCE)
    set(ENABLE_SILENT_VOLE ON CACHE BOOL "Silent VOLE" FORCE)
    set(ENABLE_REGULAR_DPF ON CACHE BOOL "oc::RegularDpf" FORCE)

    # LogVole is SEAL-backed and out of sympsica's scope; skip it even though
    # SEAL is already vendored (overrides libOTe's own default of ON).
    set(ENABLE_LOGVOLE OFF CACHE BOOL "LogVole (SEAL-backed, unused by sympsica)" FORCE)

    add_subdirectory("${CMAKE_SOURCE_DIR}/vendor/libOTe" EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# sympsica static library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SYMPSICA_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
add_library(sympsica STATIC ${SYMPSICA_SOURCES})
target_include_directories(sympsica PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(sympsica PUBLIC blake3)
if(SYMPSICA_NO_FILTER)
    target_compile_definitions(sympsica PRIVATE SYMPSICA_NO_FILTER)
endif()
if(SYMPSICA_WRONG_SIGN)
    target_compile_definitions(sympsica PRIVATE SYMPSICA_WRONG_SIGN)
endif()
if(SYMPSICA_WRONG_CONVERT)
    target_compile_definitions(sympsica PRIVATE SYMPSICA_WRONG_CONVERT)
endif()
if(SYMPSICA_STALE_SIZES)
    target_compile_definitions(sympsica PRIVATE SYMPSICA_STALE_SIZES)
endif()
if(SYMPSICA_WRONG_BOUNDARY)
    target_compile_definitions(sympsica PRIVATE SYMPSICA_WRONG_BOUNDARY)
endif()
if(SYMPSICA_DOUBLE_APPLY_COMMIT)
    target_compile_definitions(sympsica PRIVATE SYMPSICA_DOUBLE_APPLY_COMMIT)
endif()
if(SYMPSICA_BUILD_LIBOTE)
    # `libOTe` is the actual target name add_subdirectory() defines (see
    # vendor/libOTe/libOTe/CMakeLists.txt — no `oc::` ALIAS exists outside
    # libOTe's own install()-exported package config, which this project
    # does not use). Pulls in cryptoTools (incl. BitVector.cpp, needed by
    # CoeffCtxFp61::binaryDecomposition per the W1.2 PINS.md note) and
    # coproto/boost transitively via libOTe's own PUBLIC link.
    target_link_libraries(sympsica PUBLIC libOTe)
endif()

# ---------------------------------------------------------------------------
# party app: apps/ is owned by Phase 5; only wired once it exists.
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/apps/party_main.cpp")
    add_executable(party "${CMAKE_SOURCE_DIR}/apps/party_main.cpp")
    target_link_libraries(party PRIVATE sympsica)
endif()

# task-19-brief.md SC3/R-CRASHDRV: the crash-matrix helper binary (see
# apps/crash_probe.cpp's own top comment for why this exists alongside
# `party`).
if(EXISTS "${CMAKE_SOURCE_DIR}/apps/crash_probe.cpp")
    add_executable(crash_probe "${CMAKE_SOURCE_DIR}/apps/crash_probe.cpp")
    target_link_libraries(crash_probe PRIVATE sympsica)
endif()

# ---------------------------------------------------------------------------
# test targets — task-4 obligation (a): per-directory binaries instead of one
# GLOB_RECURSE over all of test/, so `ctest -R integration` matches ONLY real
# integration tests instead of silently matching nothing while still exiting
# zero. A directory with no *.cpp sources gets NO target at all (guarded on
# the GLOB result being non-empty) rather than an empty binary.
#
# BINDING: every gating `ctest` invocation in this project (CI, local
# verification, task briefs' Success Criteria) MUST pass `--no-tests=error`
# so that a test-selector regex matching zero tests is a hard failure
# (nonzero exit) instead of a silently-green no-op. This is what makes
# `ctest -R integration` fail loudly today (no integration tests exist yet)
# instead of reporting false success.
# ---------------------------------------------------------------------------
enable_testing()
include(GoogleTest)

function(sympsica_add_test_target _dir_name _test_prefix)
    # GLOB_RECURSE (task-7 brief, W2.3): test/gates/ owns nested
    # subdirectories per test group (test/gates/oracle_dpf/), unlike
    # test/utils/ and test/integration/, which stay flat. A plain GLOB would
    # silently miss sources one level down; GLOB_RECURSE is a superset that
    # still matches exactly the same files for the existing flat dirs. The
    # *.cpp-only pattern also deliberately excludes
    # test/gates/oracle_dpf/google_dpf/oracle_check.cc (part (b)'s Bazel-only
    # driver, `.cc` per google/distributed_point_functions' own convention,
    # never a CMake target -- see that directory's MODULE.bazel).
    file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/test/${_dir_name}/*.cpp")
    # Extra sources shared in from another test directory's implementation
    # file (task-7 brief: test/gates/oracle_dpf/kat_oracle_libote.cpp reuses
    # test/integration/ztgate_pipeline.cpp's actual implementation, not just
    # its header, so it must be compiled into this target too).
    list(APPEND _srcs ${ARGN})
    if(_srcs STREQUAL "")
        return()
    endif()

    set(_target "sympsica_tests_${_dir_name}")
    add_executable(${_target} ${_srcs})
    target_link_libraries(${_target} PRIVATE sympsica gtest gtest_main)
    # task-3-brief.md W1.8: fixture files are read by absolute path (resolved
    # against the source tree, not ctest's working directory).
    target_compile_definitions(${_target} PRIVATE
        SYMPSICA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    # task-8 brief, requirement 4: the W2.4 pool-scale gate
    # (test/integration/w24_pool_gate.cpp) may take up to ~10 min worst case
    # over real TCP; set a 900s ctest TIMEOUT so a hang aborts loudly instead
    # of stalling the suite indefinitely. gtest_discover_tests defers test
    # discovery to build time (it writes the test list into a generated
    # CTestTestfile.cmake via a POST_BUILD step), so there is no per-test
    # hook to target only that one test at configure time; PROPERTIES here
    # applies to every test discovered from this call. Scoped to the
    # `integration` target only (harmless elsewhere: every other integration
    # test already finishes in well under a second, so a 900s ceiling is
    # purely a safety net, never the expected runtime).
    # task-15-brief.md SC2/SC5: the NetExchange deadlock-stress and
    # production-path tests (test/utils/net_exchange.cpp) must FAIL loudly
    # via ctest TIMEOUT instead of hanging if Channel::exchange()'s
    # concurrent send/recv composition ever regresses to a blocking
    # send()-then-recv() pattern. Scoped to the whole `utils` target (same
    # idiom as `integration` below) rather than per-test: gtest_discover_tests
    # defers discovery to build time, so there is no per-test hook at
    # configure time (see the `integration` comment below); every OTHER test
    # in this target (net_smoke.cpp, kat_field.cpp, kat_encoding.cpp) already
    # finishes in well under a second over a small loopback buffer, so a 60s
    # ceiling is purely a safety net for them, never the expected runtime.
    set(_extra_test_properties "")
    if(_dir_name STREQUAL "integration")
        set(_extra_test_properties PROPERTIES TIMEOUT 900)
    elseif(_dir_name STREQUAL "utils")
        set(_extra_test_properties PROPERTIES TIMEOUT 60)
    elseif(_dir_name STREQUAL "protocols_heavy")
        # task-19-brief.md R-SCALE19 requirement 3: every heavy test needs
        # ctest LABEL "heavy" (so `ctest -L heavy` runs the gate set) AND an
        # explicit TIMEOUT ~3x its measured runtime. gtest_discover_tests'
        # PROPERTIES apply per-TARGET (every test discovered from ONE
        # add_executable), which is exactly why these three genuinely-heavy
        # files (kat_schedule100.cpp: ALL 100 seeds per R-SCALE19-AMEND,
        # measured 436.2s; kat_tvf12_schedule.cpp ~144s; kat_clmb.cpp ~1.4s
        # but plan-mandated to carry the label regardless) live in their OWN
        # directory/target rather than mixed into test/protocols/ alongside
        # every sub-second KAT there. TIMEOUT 1500s is ~3.4x the slowest
        # measured leg (kat_schedule100, 436.2s) with margin; see
        # task-19-report.md's measured-runtime table.
        set(_extra_test_properties PROPERTIES LABELS "heavy" TIMEOUT 1500)
    endif()
    gtest_discover_tests(${_target} TEST_PREFIX "${_test_prefix}." ${_extra_test_properties})
endfunction()

# test/utils/*.cpp -> sympsica_tests_utils, TEST_PREFIX "utils." (task-3-brief.md's
# binding `ctest -R utils` selector keeps matching, unchanged).
sympsica_add_test_target(utils utils)
# test/core/*.cpp -> sympsica_tests_core, TEST_PREFIX "core." (task-10-brief.md,
# Phase 3a: core/{share,table,state,pools} + protocols/update). Also covers
# Update's local-filter self-checks (task-10 brief: "do not create an empty
# update target"). Task 11's formal `update.*` KATs now live in their own
# test/protocols/ target below (design-tree-correct location), registered
# with TEST_PREFIX "update." instead of "protocols." so the binding
# `ctest -R 'core|update'` selector still matches -- see that target's own
# comment for why.
sympsica_add_test_target(core core)

# R-GREP (task-11-brief.md): grep-guard asserting core/share.hpp's open()
# has no call sites outside test/** and core/share.hpp/.cpp itself (see
# test/core/grep_guard_no_open_callsites.sh for the full rationale). A
# native `add_test` (not a gtest case) invoking the script directly, named
# with the "core." prefix by hand so `ctest -R core` catches it exactly
# like every gtest_discover_tests-registered test in this directory.
add_test(NAME core.GrepGuard_NoOpenCallSitesOutsideShare
    COMMAND sh "${CMAKE_SOURCE_DIR}/test/core/grep_guard_no_open_callsites.sh" "${CMAKE_SOURCE_DIR}")

# task-24-brief.md W6.5 CG-A(ii)/R6-CGA-RUNTIME: the STATIC leg of "Channel
# object was never constructed" on apps/party_main.cpp's --update-only path
# (see test/core/grep_guard_no_channel_update_only.sh's own header for the
# two independent checks it runs). Same native-add_test idiom as the R-GREP
# guard above.
add_test(NAME core.GrepGuard_NoChannelInUpdateOnly
    COMMAND sh "${CMAKE_SOURCE_DIR}/test/core/grep_guard_no_channel_update_only.sh" "${CMAKE_SOURCE_DIR}")

# task-24-brief.md W6.6(iii)/R6-DPFKEY: sibling to the R-GREP guard above --
# "DPF keys are never serialized outside the pool store" (see
# test/core/grep_guard_no_dpf_key_serialization.sh's own header for the
# three independent checks it runs).
add_test(NAME core.GrepGuard_NoDpfKeySerialization
    COMMAND sh "${CMAKE_SOURCE_DIR}/test/core/grep_guard_no_dpf_key_serialization.sh" "${CMAKE_SOURCE_DIR}")

# test/protocols/*.cpp -> sympsica_tests_protocols, TEST_PREFIX "update." --
# task-11-brief.md requirement 1: the design tree names
# test/protocols/kat_update.cpp, but the per-directory-name TEST_PREFIX
# convention above would give "protocols." (capital-agnostic mismatch: the
# binding SC selector `ctest -R 'core|update'` is case-sensitive and
# "protocols.Update.UPD1_..." does not contain lowercase "update"). This
# helper's TWO arguments are independent (dir_name for the glob path/target
# name, test_prefix for TEST_PREFIX) precisely to allow this decoupling --
# passing "update" as the second argument makes every discovered test name
# start with "update." (e.g. "update.Update.UPD1_..."), which DOES contain
# "update" and keeps the plan's selector matching with no changes to test
# suite/case naming.
sympsica_add_test_target(protocols update)
# test/protocols_heavy/*.cpp -> sympsica_tests_protocols_heavy, TEST_PREFIX
# "heavy." (task-19-brief.md R-SCALE19 requirement 3: see the LABELS/TIMEOUT
# comment inside sympsica_add_test_target above for why these three files
# live in their own target). `ctest -R heavy` and `ctest -L heavy` both
# select this whole set.
sympsica_add_test_target(protocols_heavy heavy)

# test/integration/*.cpp -> sympsica_tests_integration, TEST_PREFIX "integration."
# (task-6 brief, addendum 10(i): sources now exist under test/integration/;
# the original "no sources exist yet" comment describing obligation (a)'s
# guard is stale and has been removed).
sympsica_add_test_target(integration integration)
# test/gates/{oracle_dpf,}/*.cpp -> sympsica_tests_gates, TEST_PREFIX "gates."
# (task-7-brief.md, W2.3: cross-implementation SEMANTIC check / ZT-4). Used to
# pass test/integration/{ztgate_pipeline,vole_beaver}.cpp in explicitly as
# extra sources (Task 5/6/13): those implementation files now live in
# src/protocols/detail/ (task-16-brief.md, W5.1, R-HOST consolidation) and
# are compiled into the `sympsica` library every test target already links
# against, so the extra-source ARGN is no longer needed here; the headers
# under test/integration/ are now thin forwarding shims to
# include/sympsica/protocols/detail/.
sympsica_add_test_target(gates gates)

# ---------------------------------------------------------------------------
# task-20-brief.md W6.1 (SC1): test/failure_suite.cpp -- the TV-F negative
# registry. A standalone translation unit registered directly (NOT via
# sympsica_add_test_target, which globs a whole test/<dir>/ subtree -- the
# brief's own file list names test/failure_suite.cpp at test/'s top level,
# not inside a subdirectory). Needs SYMPSICA_SOURCE_DIR (SC3's deferred-row
# guard resolves paths like data/parse_ofac.py against the repo root), the
# same define every other test target gets.
# ---------------------------------------------------------------------------
add_executable(sympsica_tests_failure_suite "${CMAKE_SOURCE_DIR}/test/failure_suite.cpp")
target_link_libraries(sympsica_tests_failure_suite PRIVATE sympsica gtest gtest_main)
target_compile_definitions(sympsica_tests_failure_suite PRIVATE
    SYMPSICA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# TEST_PREFIX "failure." keeps every discovered test name matchable via
# `-R 'failure|Failure|FSuite'` (requirement 3) without disturbing any
# existing selector (core|update, query|Query, setup|Setup, heavy).
gtest_discover_tests(sympsica_tests_failure_suite TEST_PREFIX "failure.")

# ---------------------------------------------------------------------------
# task-18-brief.md R-E2E-DRIVER (SC6): the two-OS-process smoke E2E for
# apps/party_main.cpp. NOT a gtest binary (test/e2e/run_smoke.py is plain
# python3, stdlib only) -- registered directly via add_test, same idiom as
# the R-GREP grep-guard above. Only wired when the `party` target actually
# exists (apps/party_main.cpp is Phase-5-owned, same EXISTS guard the
# executable's own definition above uses); the binary path is threaded in
# via $<TARGET_FILE:party> (a build-time generator expression), never a
# hardcoded path, so it tracks wherever the build actually places it across
# platforms/generators.
# ---------------------------------------------------------------------------
if(TARGET party)
    add_test(NAME e2e.SmokeTwoProcess
        COMMAND python3 "${CMAKE_SOURCE_DIR}/test/e2e/run_smoke.py"
                --party-bin "$<TARGET_FILE:party>"
                --reference "${CMAKE_SOURCE_DIR}/ref/reference.py"
                --fixtures-dir "${CMAKE_SOURCE_DIR}/test/e2e/fixtures"
                --workdir "${CMAKE_CURRENT_BINARY_DIR}/e2e_smoke_workdir"
                --timeout-s 90)
    # Generous-but-real ctest-level TIMEOUT (task-18-brief.md R-E2E-DRIVER),
    # strictly larger than the script's own --timeout-s=90 internal budget
    # so the script's own kill-and-FAIL path is what fires first in the
    # normal case; this ctest TIMEOUT is the last-resort safety net if the
    # python3 driver process itself somehow wedges.
    set_tests_properties(e2e.SmokeTwoProcess PROPERTIES TIMEOUT 120)
endif()

# ---------------------------------------------------------------------------
# task-19-brief.md SC2 [E2E-1..4]: four seed-fixed, two-OS-process schedules
# at n~2^10 ids/party (test/e2e/fixtures/e2e_seed{0..3}_{r,s}.json).
# R-SCALE19: "Expect minutes per schedule" -- LABEL "heavy" + a generous
# TIMEOUT (measured runtimes recorded in task-19-report.md).
# ---------------------------------------------------------------------------
if(TARGET party)
    foreach(_e2e_seed 0 1 2 3)
        add_test(NAME e2e.E2E${_e2e_seed}_TwoProcessN1024
            COMMAND python3 "${CMAKE_SOURCE_DIR}/test/e2e/run_e2e_gate.py"
                    --party-bin "$<TARGET_FILE:party>"
                    --reference "${CMAKE_SOURCE_DIR}/ref/reference.py"
                    --schedule-r "${CMAKE_SOURCE_DIR}/test/e2e/fixtures/e2e_seed${_e2e_seed}_r.json"
                    --schedule-s "${CMAKE_SOURCE_DIR}/test/e2e/fixtures/e2e_seed${_e2e_seed}_s.json"
                    --workdir "${CMAKE_CURRENT_BINARY_DIR}/e2e_seed${_e2e_seed}_workdir"
                    --timeout-s 900)
        set_tests_properties(e2e.E2E${_e2e_seed}_TwoProcessN1024 PROPERTIES LABELS "heavy" TIMEOUT 900)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# task-19-brief.md SC3 [CRASH-MATRIX]/R-CRASHDRV: the 5-point deterministic
# crash matrix, driven by real subprocesses of `crash_probe`.
# ---------------------------------------------------------------------------
if(TARGET crash_probe)
    add_test(NAME e2e.CrashMatrixFivePoint
        COMMAND python3 "${CMAKE_SOURCE_DIR}/test/e2e/run_crash_matrix.py"
                --probe-bin "$<TARGET_FILE:crash_probe>"
                --workdir "${CMAKE_CURRENT_BINARY_DIR}/crash_matrix_workdir"
                --timeout-s 60)
    set_tests_properties(e2e.CrashMatrixFivePoint PROPERTIES LABELS "heavy" TIMEOUT 180)
endif()

# ---------------------------------------------------------------------------
# task-21-brief.md W6.3 (SC4/SC5, R6-OVFDEMO/R6-OVFSCOPE): the planted-
# overflow demo. detect_overflow is PYTHON-ONLY (no C++ port exists -- R-ORACLE-
# AGNOSTIC, task-18-brief.md), so there is no production C++ call site to
# exercise from a gtest binary; test/overflow/gen_overflow_demo.py IS the
# test (loads the committed fixture, recomputes fresh via reference.py,
# asserts SC4/FC3/FC4, then regenerates docs/overflow_demo.md) -- same
# native-add_test-running-python3 idiom as the E2E drivers and the R-GREP
# guard above, for the same reason (a Python-only check with nothing to
# gtest_discover_tests). Named with an "overflow." prefix (not tied to any
# existing sympsica_add_test_target directory) so `-R 'overflow|Overflow'`
# matches unambiguously without incidentally widening any other selector.
# ---------------------------------------------------------------------------
add_test(NAME overflow.PlantedFiveIdsFlagAtT7AndClearAfterRefresh
    COMMAND python3 "${CMAKE_SOURCE_DIR}/test/overflow/gen_overflow_demo.py")

# ---------------------------------------------------------------------------
# task-24-brief.md W6.6(i)/R6-MASKSTAT: the one-shot 10^5-mask-uniformity
# campaign generator. A standalone executable (same idiom as `party`/
# `crash_probe` above, not a gtest binary -- test/campaign/mask_campaign_gen.cpp
# has its own plain main()), registered via native `add_test` so it is
# discoverable/documented via `ctest -R campaign` / `ctest -L campaign`, but
# DISABLED by default (R6-MASKSTAT: "EXCLUDED from the default ctest set") --
# a ~1.5-2h test must never run inside a normal `ctest --no-tests=error`
# invocation. To actually (re)generate the committed fixture: reconfigure
# with -DSYMPSICA_ENABLE_MASK_CAMPAIGN=ON, rebuild, then
# `ctest --test-dir build -R campaign.GenerateMaskUniformityFixture1e5`
# (or invoke the built `mask_campaign_gen` binary directly -- see
# task-24-report.md for the exact invocation line and measured wall-clock).
# ---------------------------------------------------------------------------
add_executable(mask_campaign_gen "${CMAKE_SOURCE_DIR}/test/campaign/mask_campaign_gen.cpp")
target_link_libraries(mask_campaign_gen PRIVATE sympsica)

option(SYMPSICA_ENABLE_MASK_CAMPAIGN
    "Enable the one-shot 10^5-mask-uniformity generation campaign (task-24-brief.md R6-MASKSTAT) as a runnable ctest entry; OFF by default so a normal ctest invocation never runs this ~1.5-2h test"
    OFF)

add_test(NAME campaign.GenerateMaskUniformityFixture1e5
    COMMAND mask_campaign_gen
            --out "${CMAKE_SOURCE_DIR}/test/fixtures/mask_uniformity_1e5.fixture"
            --count 100000)
# TIMEOUT 10800s (3h) matches R6-MASKSTAT's own escalation bound ("if
# generation would exceed 3 hours, escalate before trimming N") -- this
# TIMEOUT is the automated version of that same bound, not an arbitrary
# safety margin.
set_tests_properties(campaign.GenerateMaskUniformityFixture1e5
    PROPERTIES LABELS "heavy;campaign" TIMEOUT 10800)
if(NOT SYMPSICA_ENABLE_MASK_CAMPAIGN)
    set_tests_properties(campaign.GenerateMaskUniformityFixture1e5 PROPERTIES DISABLED TRUE)
endif()

# ---------------------------------------------------------------------------
# task-24-brief.md W6.5 CG-A: zero-communication Update claim gate. NOT a
# gtest binary (test/e2e/run_cga_gate.py is plain python3, stdlib only) --
# same native-add_test-running-python3 idiom as the other E2E drivers. Only
# wired once `party` exists (Phase-5-owned, same EXISTS guard used
# throughout this file); the binary path is threaded in via
# $<TARGET_FILE:party>, never hardcoded.
# ---------------------------------------------------------------------------
if(TARGET party)
    add_test(NAME e2e.CGAZeroCommunicationUpdate
        COMMAND python3 "${CMAKE_SOURCE_DIR}/test/e2e/run_cga_gate.py"
                --party-bin "$<TARGET_FILE:party>"
                --workdir "${CMAKE_CURRENT_BINARY_DIR}/cga_gate_workdir")
    set_tests_properties(e2e.CGAZeroCommunicationUpdate PROPERTIES TIMEOUT 120)
endif()
