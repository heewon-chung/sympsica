cmake_minimum_required(VERSION 3.20)
project(sympsica LANGUAGES CXX)

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
# W0.4: guard against a mismatched vendored libOTe checkout.
# ---------------------------------------------------------------------------
file(STRINGS "${CMAKE_SOURCE_DIR}/PINS.md" _sympsica_pins_lines)
set(_sympsica_pinned_libote_sha "")
foreach(_line IN LISTS _sympsica_pins_lines)
    if(_line MATCHES "^- libOTe: ([0-9a-f]+)")
        set(_sympsica_pinned_libote_sha "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(_sympsica_pinned_libote_sha STREQUAL "")
    message(FATAL_ERROR "PINS.md: could not find a 'libOTe: <sha>' entry")
endif()

find_package(Git REQUIRED)
execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${CMAKE_SOURCE_DIR}/vendor/libOTe" rev-parse HEAD
    OUTPUT_VARIABLE _sympsica_actual_libote_sha
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _sympsica_libote_git_result
    ERROR_QUIET
)
if(NOT _sympsica_libote_git_result EQUAL 0)
    message(FATAL_ERROR
        "vendor/libOTe is not a valid git checkout. Re-vendor per PINS.md: "
        "git clone https://github.com/osu-crypto/libOTe vendor/libOTe && "
        "git -C vendor/libOTe checkout ${_sympsica_pinned_libote_sha}")
endif()
if(NOT _sympsica_actual_libote_sha STREQUAL _sympsica_pinned_libote_sha)
    message(FATAL_ERROR
        "vendor/libOTe HEAD (${_sympsica_actual_libote_sha}) does not match the pinned SHA "
        "(${_sympsica_pinned_libote_sha}) recorded in PINS.md. Re-vendor libOTe at the pinned SHA.")
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
# sympsica static library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SYMPSICA_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
add_library(sympsica STATIC ${SYMPSICA_SOURCES})
target_include_directories(sympsica PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(sympsica PUBLIC blake3)

# ---------------------------------------------------------------------------
# party app: apps/ is owned by Phase 5; only wired once it exists.
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/apps/party_main.cpp")
    add_executable(party "${CMAKE_SOURCE_DIR}/apps/party_main.cpp")
    target_link_libraries(party PRIVATE sympsica)
endif()

# ---------------------------------------------------------------------------
# sympsica_tests
# ---------------------------------------------------------------------------
enable_testing()
file(GLOB_RECURSE SYMPSICA_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/test/*.cpp")
add_executable(sympsica_tests ${SYMPSICA_TEST_SOURCES})
target_link_libraries(sympsica_tests PRIVATE sympsica gtest gtest_main)

include(GoogleTest)
gtest_discover_tests(sympsica_tests)
