#!/bin/sh
# test/core/grep_guard_no_dpf_key_serialization.sh — task-24-brief.md
# W6.6(iii)/R6-DPFKEY: "DPF keys are never serialized outside the pool
# store" -- the sibling guard to test/core/grep_guard_no_open_callsites.sh
# (same idiom: a plain shell script invoked via `add_test(COMMAND sh
# <this> <srcdir>)`, not a gtest case or an embedded CMake grep pipeline).
#
# `RegularDpfKey` (vendor/libOTe/libOTe/Dpf/RegularDpf.h) exposes exactly
# TWO serialization entry points -- `toBytes(span<u8>)` / `fromBytes(span<u8>)`
# -- plus a `sizeBytes()` sizing helper. This guard asserts THREE independent
# facts under src/ + include/ (production code only; vendor/ is read-only
# and out of scope, test/** is intentionally out of scope for the same
# reason grep_guard_no_open_callsites.sh excludes it -- a key-serialization
# round-trip KAT, if one is ever written, legitimately calls these):
#
#  (1) NO call to `.toBytes(`/`.fromBytes(` anywhere in production code --
#      the DPF key's own serialization API is never invoked at all outside
#      the vendor header that defines it.
#  (2) `RegularDpfKey` never appears in utils/serdes.{hpp,cpp} -- the ONE
#      production wire/disk serialization module (write_fp/write_u64_vec
#      and friends) -- so a key could not flow through the wire format even
#      indirectly, via a serdes primitive that happened to accept one.
#  (3) `RegularDpfKey` never appears in core/state.{hpp,cpp} -- PartyState's
#      own save()/load(), the other place this project writes bytes to disk
#      -- confirming pools.hpp's own doc comment ("Pools ... are NOT part of
#      PartyState ... never serialized here") stays true, not just asserted
#      in a comment.
#
# Together: the only two places `RegularDpfKey` legitimately appears in
# production code are pools.hpp (the pool's own storage) and
# ztgate_pipeline.hpp/ztest.cpp (in-memory DKG output / online expand) --
# neither of which is a serialization path.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <repo-source-dir>" >&2
    exit 2
fi
SRC_DIR="$1"

fail=0

toBytes_hits=$(grep -rn -E '\.toBytes\(|\.fromBytes\(' "$SRC_DIR/src" "$SRC_DIR/include" \
    --include='*.hpp' --include='*.cpp' || true)
if [ -n "$toBytes_hits" ]; then
    echo "R6-DPFKEY FAIL: RegularDpfKey::toBytes()/fromBytes() called in production code:" >&2
    echo "$toBytes_hits" >&2
    fail=1
fi

serdes_hits=$(grep -n 'RegularDpfKey' \
    "$SRC_DIR/include/sympsica/utils/serdes.hpp" "$SRC_DIR/src/utils/serdes.cpp" 2>/dev/null || true)
if [ -n "$serdes_hits" ]; then
    echo "R6-DPFKEY FAIL: RegularDpfKey referenced inside utils/serdes.{hpp,cpp}:" >&2
    echo "$serdes_hits" >&2
    fail=1
fi

state_hits=$(grep -n 'RegularDpfKey' \
    "$SRC_DIR/include/sympsica/core/state.hpp" "$SRC_DIR/src/core/state.cpp" 2>/dev/null || true)
if [ -n "$state_hits" ]; then
    echo "R6-DPFKEY FAIL: RegularDpfKey referenced inside core/state.{hpp,cpp} (PartyState save/load):" >&2
    echo "$state_hits" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "R6-DPFKEY OK: no DPF-key serialization call site and no RegularDpfKey reference in serdes.*/state.* outside the pool store"
exit 0
