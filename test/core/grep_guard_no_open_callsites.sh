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
#
# task-18-brief.md R-OPEN: the final count opening has landed --
# src/protocols/query.cpp's `Query::open_count()` is the ONE legitimate
# production call site (both parties call it, symmetrically, to learn the
# public count after Query::run/SaltManager::refresh). Whitelisted
# file-precise (not a directory blanket): query.cpp contains exactly this
# one `open(` call, so excluding the file is equivalent to excluding the
# exact call site.
#
# task-24-brief.md W6.6(iii)/R6-GREPRECON (reconciliation note, no
# behavior change): the plan's own W6.6(iii) wording asks for a guard that
# "asserts `open(` appears only in gates/ (masked openings), the final
# output conversion, and tests". THIS guard is already STRICTER than that:
# it forbids `open(` under src/+include/ ENTIRELY except the two exact
# whitelisted files above (core/share.{hpp,cpp}'s own declaration/
# definition and query.cpp's one legitimate call site) -- it does NOT
# additionally whitelist a `gates/` directory, because there is nothing to
# whitelist there: the gates under src/gates/ perform MASKED openings
# (calling gate-internal reconstruction helpers, never `Channel&, Share`'s
# raw `open()` directly), so this guard already finds ZERO `open(` hits
# anywhere under src/gates/ or include/sympsica/gates/ today, verified by
# running it. Do NOT "fix" this guard to add a `gates/` exclusion to match
# the plan's looser phrasing -- doing so would WEAKEN it, by legalizing a
# raw `open()` call site inside a gate that does not exist yet and, per
# plan W3.1, must never exist.
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
    | grep -v '/protocols/query\.cpp:' \
    || true)

if [ -n "$matches" ]; then
    echo "R-GREP FAIL: open() call site(s) found outside core/share.hpp/.cpp:"
    echo "$matches"
    exit 1
fi

echo "R-GREP OK: no open() call sites outside core/share.hpp/.cpp"
exit 0
