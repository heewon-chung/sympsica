#!/bin/sh
# test/core/grep_guard_no_open_callsites.sh — R-GREP (task-11-brief.md):
# asserts core/share.hpp's open(Channel&, Share) has no call sites under
# src/ or include/ outside its own declaration (share.hpp) and definition
# (share.cpp). plan W3.1, binding: open() is used ONLY by the final output
# opening and by tests -- every intermediate reconstruction inside a gate
# must instead be a masked opening. This grep-guard is what enforces that
# for src/+include/ as those directories grow through later phases; test/**
# is intentionally OUT of scope here (tests are allowed to call open()
# directly -- e.g. SaveLoadRoundTrip-style KATs that need a real
# reconstructed value), and so is the eventual final-output opening
# (Phase 5+, not written yet -- when it lands, add its file to the exclude
# list below with a comment explaining why it's a legitimate call site).
#
# Mechanism (R-GREP: "a test ... asserting open( from core/share.hpp has no
# call sites ... implement as ... a CMake add_test invoking grep directly"):
# a plain shell script invoked via `add_test(COMMAND sh <this> <srcdir>)` in
# CMakeLists.txt, rather than a one-line grep pipeline embedded directly in
# CMake (which needs fragile multi-level shell-quoting to express the
# exclude + grep -v pipeline) or a gtest case shelling out via std::system
# (this is the "cleaner" CMake-native path R-GREP names explicitly).
#
# Match is a WORD-BOUNDARY "open(" (grep -E '\bopen\(') so this does not
# false-positive on is_open()/fopen()/etc. (both already appear in
# src/core/state.cpp's save()/load()).
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <repo-source-dir>" >&2
    exit 2
fi
SRC_DIR="$1"

matches=$(grep -rn -E '\bopen\(' "$SRC_DIR/src" "$SRC_DIR/include" \
    --include='*.hpp' --include='*.cpp' \
    | grep -v '/core/share\.hpp:' \
    | grep -v '/core/share\.cpp:' \
    || true)

if [ -n "$matches" ]; then
    echo "R-GREP FAIL: open() call site(s) found outside core/share.hpp/.cpp:"
    echo "$matches"
    exit 1
fi

echo "R-GREP OK: no open() call sites outside core/share.hpp/.cpp"
exit 0
