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

# apply_split_shaper -- Phase-8 gate fix round 1 (Task 1, C1): the ONE proven
# netem-root + tbf-child construction (bench/netem.sh's step 4 comment has the
# full measurement: a single netem carrying both `delay` and `rate` dequeues
# ~one packet per DELAY interval on the AWS kernel, capping 1 Gbit at 142.7
# Mbit; delay and tbf-rate as separate qdiscs fixes it, 952.6 Mbit). Factored
# out so bench/netem.sh (the veth pair) and any caller needing the identical
# single-interface construction share ONE definition instead of two copies
# that can silently diverge (exactly what happened to the pre-fix bms24 `lo`
# path). `$LIMIT` (netem queue depth, packets) follows the caller's own global
# if set -- both bench/netem.sh and baselines/bms24/run.sh (via `source
# bench/netem.sh`) already define LIMIT=10000 before any shaping call.
#   apply_split_shaper <netns> <dev> <delay_us> <rate_kbit>
# returns nonzero on any tc failure.
apply_split_shaper() {
  local ns=$1 dev=$2 delay_us=$3 rate_kbit=$4
  local limit=${LIMIT:-10000}
  # tbf burst: a token bucket that cannot hold one timer tick's worth of
  # tokens under-delivers, so burst >= rate/HZ; sized at rate/250 (HZ=250)
  # with a 3000 B floor to keep the low-rate profiles' buckets sane -- the
  # exact formula bench/netem.sh step 4 used inline (pinned by AWS
  # measurement; unchanged here).
  local burst_bytes=$(( rate_kbit * 1000 / 8 / 250 ))
  [ "$burst_bytes" -lt 3000 ] && burst_bytes=3000
  ip netns exec "$ns" tc qdisc replace dev "$dev" root handle 1: netem delay ${delay_us}us limit "$limit" || return 1
  ip netns exec "$ns" tc qdisc replace dev "$dev" parent 1: handle 10: tbf \
    rate ${rate_kbit}kbit burst ${burst_bytes} latency 50ms || return 1
}
