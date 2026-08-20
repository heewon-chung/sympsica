#!/bin/sh
# test/core/grep_guard_no_channel_update_only.sh — task-24-brief.md
# W6.5 CG-A(ii) / R6-CGA-RUNTIME: the STATIC leg of "Channel object was
# never constructed" on the --update-only path. Paired with the RUNTIME
# leg (Channel::construction_count(), utils/net.{hpp,cpp}) that
# test/e2e/run_cga_gate.py asserts reads exactly 0 for a real
# `party --role r --update-only` run -- R6-CGA-RUNTIME is explicit that
# EITHER leg alone is only half the check.
#
# Two independent checks, both must find ZERO matches:
#
#  (1) apps/party_main.cpp's BEGIN-UPDATE-ONLY-NO-CHANNEL /
#      END-UPDATE-ONLY-NO-CHANNEL block (the --update-only branch of
#      main(), see that file's own comment at the marker) must contain no
#      `Channel` token at all, and no `connect_channel(` call, EXCEPT the
#      one whitelisted static-accessor call
#      `Channel::construction_count()` (which the block legitimately calls
#      to print the runtime leg's own counter). The whitelisted call is
#      stripped from the block's text FIRST, then the remainder is grepped
#      for the bare word `Channel` -- deliberately NOT a narrower pattern
#      like `Channel(` or `Channel\s*\(`, which would (and, in an earlier
#      draft of this script, DID -- caught only by testing it against a
#      deliberately-injected `Channel fake_ch(...)` named-variable
#      construction, see task-24-report.md) miss the MOST COMMON C++
#      construction syntax, `Channel <name>(<args>)`. A bare-word match
#      after whitelist-stripping catches every construction syntax
#      uniformly (named variable, temporary, `make_unique<Channel>`,
#      `new Channel`, a `Channel&`/`Channel*` local, etc.) because all of
#      them contain the literal token `Channel`.
#
#  (2) include/sympsica/protocols/update.hpp and src/protocols/update.cpp
#      -- the ONLY production code the update-only block calls into
#      (Update::apply) -- never `#include ".../net.hpp"` at all. A
#      comment-proof signal (unlike grepping for the bare word "Channel",
#      which legitimately appears in this project's OWN prose -- e.g.
#      update.hpp's doc comment literally says "no Channel parameter"):
#      if net.hpp is never included, the Channel TYPE is not nameable in
#      either file, full stop, so no construction is possible there
#      regardless of what any comment says. This is what makes "no Channel
#      construction is reachable from the update-only path" a real claim
#      about the whole reachable call graph, not just the one call site in
#      party_main.cpp.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <repo-source-dir>" >&2
    exit 2
fi
SRC_DIR="$1"
PARTY_MAIN="$SRC_DIR/apps/party_main.cpp"

if [ ! -f "$PARTY_MAIN" ]; then
    echo "grep_guard_no_channel_update_only: FAIL: $PARTY_MAIN not found" >&2
    exit 1
fi

# --- check (1): the marked block in party_main.cpp -------------------------
BLOCK=$(sed -n '/BEGIN-UPDATE-ONLY-NO-CHANNEL/,/END-UPDATE-ONLY-NO-CHANNEL/p' "$PARTY_MAIN")
if [ -z "$BLOCK" ]; then
    echo "grep_guard_no_channel_update_only: FAIL: BEGIN/END-UPDATE-ONLY-NO-CHANNEL markers not found in $PARTY_MAIN" >&2
    exit 1
fi

block_stripped=$(printf '%s\n' "$BLOCK" | sed -E 's/Channel::construction_count/CGA_WHITELISTED_ACCESSOR/g')
block_matches=$(printf '%s\n' "$block_stripped" | grep -n -E 'Channel|connect_channel[[:space:]]*\(' || true)
if [ -n "$block_matches" ]; then
    echo "grep_guard_no_channel_update_only: FAIL: 'Channel' token (or connect_channel() call) found inside the --update-only block, outside the whitelisted Channel::construction_count() accessor:" >&2
    echo "$block_matches" >&2
    exit 1
fi

# --- check (2): Update::apply's own two files never even #include net.hpp --
# A stronger and comment-proof signal than grepping for the bare word
# "Channel" (which legitimately appears in this project's OWN prose -- e.g.
# update.hpp's doc comment says "no Channel parameter"): if net.hpp is never
# included, the Channel TYPE is not nameable there at all, full stop, so no
# construction is possible regardless of what any comment says.
update_hits=$(grep -n -E '#include[[:space:]]*"sympsica/utils/net\.hpp"' \
    "$SRC_DIR/include/sympsica/protocols/update.hpp" "$SRC_DIR/src/protocols/update.cpp" 2>/dev/null || true)
if [ -n "$update_hits" ]; then
    echo "grep_guard_no_channel_update_only: FAIL: net.hpp is #included in Update::apply's own files (Channel would become nameable there):" >&2
    echo "$update_hits" >&2
    exit 1
fi

echo "grep_guard_no_channel_update_only: OK: no Channel-constructing call reachable from --update-only (block + Update::apply's own files)"
exit 0
