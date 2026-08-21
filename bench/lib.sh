#!/usr/bin/env bash
# bench/lib.sh -- Phase 8 W8.7 (phase-8-plan.md, master plan W8.7, design line
# 62): shared shell helpers sourced by bench/netem.sh, bench/measure.sh, every
# baseline wrapper, and every interactive shell used in the phase-8 plan.
# Pinned verbatim (Global constraint 8).

LOGGED() {  # usage: LOGGED <logfile> -- <command...>; returns the PRODUCER's status
  local log=$1; shift; [ "$1" = "--" ] && shift
  mkdir -p "$(dirname "$log")"
  set -o pipefail; "$@" 2>&1 | tee "$log"; local rc=${PIPESTATUS[0]}; set +o pipefail
  echo "producer exit=$rc" | tee -a "$log"; return $rc
}
mono_ns() { python3 -c 'import time; print(time.monotonic_ns())'; }
phase()   { echo "PHASE:$1:$(mono_ns)" >&2; }
phase_zero() {  # an EMPTY segment: ONE clock read, both markers with the identical value
  local t; t=$(mono_ns)
  echo "PHASE:$1_begin:$t" >&2; echo "PHASE:$1_end:$t" >&2; }
wait_listen() {  # $1 netns, $2 port, $3 timeout_s -> 0 when a LISTEN socket on $2 exists
  local i; for i in $(seq 1 $(( $3 * 10 ))); do
    ip netns exec "$1" ss -ltnH 2>/dev/null | awk '{print $4}' | grep -q ":$2\$" && return 0
    sleep 0.1; done
  echo "wait_listen: no listener on $1:$2 after $3 s" >&2; return 5; }
tx_bytes() { ip netns exec "$1" ip -s -j link show dev "$2" \
             | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["stats64"]["tx"]["bytes"])'; }
rx_bytes() { ip netns exec "$1" ip -s -j link show dev "$2" \
             | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["stats64"]["rx"]["bytes"])'; }
