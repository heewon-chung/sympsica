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
# happens later, inside FetchContent_MakeAvailable(blake3 ...)). Gated to
# AppleClang only so other toolchains (e.g. the gcc:13 Linux leg) keep using
# cmake's normal, already-working detection.
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
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
    file(GLOB _srcs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/test/${_dir_name}/*.cpp")
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

    gtest_discover_tests(${_target} TEST_PREFIX "${_test_prefix}.")
endfunction()

# test/utils/*.cpp -> sympsica_tests_utils, TEST_PREFIX "utils." (task-3-brief.md's
# binding `ctest -R utils` selector keeps matching, unchanged).
sympsica_add_test_target(utils utils)
# test/integration/*.cpp -> sympsica_tests_integration, TEST_PREFIX "integration."
# (task-6 brief, addendum 10(i): sources now exist under test/integration/;
# the original "no sources exist yet" comment describing obligation (a)'s
# guard is stale and has been removed).
sympsica_add_test_target(integration integration)
