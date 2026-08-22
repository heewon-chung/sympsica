#!/usr/bin/env bash
# baselines/bms24/run.sh -- Task 32 (W8.1, phase-8-plan.md § 32.2), A10-revised
# by Task 34: the BMS+24 (ruidazeng/upsi-revisited @ d6ddfb5) wrapper. Encodes
# exactly the flag contracts pinned by BMS24-IMAGE-EVIDENCE.md: `--func=CA` on
# all four invocations (both `run` binaries default to SUM); the stored-trees
# flag is spelled `--trees` on addition / `--import` on deletion; the addition
# binaries have NO delete concept at all, so the TV-F17 refusal below is
# entirely this wrapper's job, never the binary's.
#
# A10 (controller amendment, USER DECISION 2026-08-22): BMS+24's gRPC uses
# LOCAL CREDENTIALS, which accept ONLY the literal address 127.0.0.1 (measured:
# 10.99.0.2 and 127.0.1.2 both REJECTED -- an exact-address check, not a 127/8
# range check; 127.0.0.1+DNAT passes the client check then dies server-side;
# both parties in ONE netns on real loopback SUCCEEDS end to end). So NO
# per-party netns pair can satisfy this on both ends at once, and BOTH variants
# here run their own ad hoc docker container PAIR sharing exactly one real
# network namespace via `--network container:<peer>` -- see the bms_lo_*
# helpers below. `CONTAINER_R`/`CONTAINER_S` (measure.sh's generic per-party
# containers, each in their OWN netns) are left completely unused by this
# wrapper; measure.sh tears them down as usual, harmlessly idle. This also
# means the GC channel's old DNAT/route_localnet workaround is GONE: party 0's
# hardcoded `127.0.0.1:1025` now genuinely reaches party 1 directly, since both
# processes share the same real loopback.
#
# Byte accounting is therefore the "combined" regime (bench/jsonl_check.py):
# bytes.r_out=bytes.s_out=0, bytes.total=bytes.external_total measured from
# the shared `lo`'s tx delta (established by real measurement, task-34-report:
# a ONE-WAY known 10,485,760 B loopback transfer produced a `lo` tx delta of
# 10,512,972 B -- i.e. a one-way send increments `lo` tx ONCE, no halving/
# doubling. The bidirectional claim is a GENERALIZATION from that one-way
# result, not a separate bidirectional measurement: every send by either
# party increments the same `lo` tx counter once regardless of direction, so
# the two parties' sends sum to the combined total with each byte counted
# once. `combined_total_b` therefore includes real TCP/IP framing, not just
# payload -- ~0.26% overhead in the one-way test, immaterial against BMS+24's
# 45.7 MB +-10% window but disclosed here rather than left for someone to
# discover later). netem shaping is applied directly to `lo` inside the ad
# hoc netns, using bench/netem.sh's OWN PROFILE_RTT_US/PROFILE_RATE_KBIT/
# LIMIT tables (sourced below, never duplicated) with the project's existing
# halving convention (delay_us = max(0, (target_rtt_us - measured_base_us)/2),
# applied ONCE since `lo` is traversed twice per round trip -- the same
# convention FastUPSI's own `network_setup.sh on 40` encodes for an 80 ms RTT).
#
# DISCLOSURE (Phase-10 caption obligation, like `shaping=whole-run(...)` for
# fastupsi): measure.sh's own generic step 8 still applies netem to the
# STANDARD veth pair (sympsica_r/sympsica_s) for every docker-mode trial,
# bms24 included -- but bms24's traffic never touches that pair, so that
# shaping is INERT for bms24 rows. The shaping of record for bms24 is the
# `lo` qdisc this wrapper applies (bms_lo_apply_netem below); a reader
# should not assume a bms24 row's profile shaping came from the standard
# veth path. The veth pair is not perfectly silent even so (real, if small,
# byte deltas), which is why jsonl_check.py's combined-loopback byte check
# tolerates a small VETH_CALIBRATION_NOISE_MAX_B ceiling rather than
# requiring the veth deltas to be exactly zero -- see that file for the
# measured provenance (M1, gate fix round 1: it is IPv6 router/neighbor
# solicitation and multicast-listener-report traffic from interface bring-up,
# NOT the base-RTT calibration pings a prior comment here incorrectly named;
# measure.sh snapshots the veth counters only after those pings already
# finished, so they cannot appear in the delta at all).
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SELF="$REPO/baselines/bms24/run.sh"
JSONL_CHECK="$REPO/bench/jsonl_check.py"
# shellcheck source=bench/lib.sh
source "$REPO/bench/lib.sh"
# shellcheck source=bench/netem.sh
source "$REPO/bench/netem.sh"   # sourceable via its BASH_SOURCE==$0 guard (A10) -- PROFILE_RTT_US/PROFILE_RATE_KBIT/LIMIT

# ---------------------------------------------------------------------------
# A10: single shared-loopback-netns topology (ad hoc container pair, own trap)
# ---------------------------------------------------------------------------
BMS_LO_NS=sympsica_lo
BMS_LO_CTR_R=sympsica_bms_lo_r
BMS_LO_CTR_S=sympsica_bms_lo_s

bms_lo_teardown() {
  docker rm -f "$BMS_LO_CTR_S" "$BMS_LO_CTR_R" >/dev/null 2>&1 || true
  rm -f "/var/run/netns/$BMS_LO_NS" 2>/dev/null || true
}

bms_lo_up() {  # $1=image $2=mem (may be empty)
  local image=$1 mem=$2
  bms_lo_teardown
  docker run -d --name "$BMS_LO_CTR_R" --network none --cpuset-cpus="$CORE_R" \
    ${mem:+--memory "$mem"} -v "$STATE_WORK:/state:rw" -v "$RUN_WORKDIR:/runwork:rw" \
    "$image" sleep infinity >/dev/null
  docker run -d --name "$BMS_LO_CTR_S" --network "container:$BMS_LO_CTR_R" --cpuset-cpus="$CORE_S" \
    ${mem:+--memory "$mem"} -v "$STATE_WORK:/state:rw" -v "$RUN_WORKDIR:/runwork:rw" \
    "$image" sleep infinity >/dev/null
  local pid_r
  pid_r=$(docker inspect -f '{{.State.Pid}}' "$BMS_LO_CTR_R")
  mkdir -p /var/run/netns
  ln -sfT "/proc/$pid_r/ns/net" "/var/run/netns/$BMS_LO_NS"
  ip netns exec "$BMS_LO_NS" ip link set lo up
  # Task 1 (gate fix round 1, C1): `lo` defaults to MTU 65536, which gives it a
  # much higher un-shaped throughput cap (MTU*8/delay) than the standard veth
  # pair's 1500 -- set BEFORE any shaping so bms24's packetisation matches the
  # substrate the standard calibration measured.
  ip netns exec "$BMS_LO_NS" ip link set lo mtu 1500
}

bms_lo_measure_base_us() {  # $1=ping count -> integer microseconds (real loopback RTT); exit 5 on unparseable ping
  local n=$1 avg_ms
  avg_ms=$(ip netns exec "$BMS_LO_NS" ping -c "$n" -i 0.2 -q 127.0.0.1 2>/dev/null \
           | sed -n 's#^rtt min/avg/max/mdev = [0-9.]*/\([0-9.]*\)/.*#\1#p')
  # C4 (Codex review, Task 34 fix round): an unparseable/empty avg_ms must NOT silently
  # become an empty string that arithmetic later treats as zero (that's the
  # silently-wrong-measurement class -- worse than a crash: it would apply zero
  # compensation and run at the wrong delay under a correct-looking profile label).
  [[ "$avg_ms" =~ ^[0-9]+(\.[0-9]+)?$ ]] || {
    echo "bms_lo_measure_base_us: could not parse a numeric RTT from ping (got '$avg_ms')" >&2
    return 5
  }
  python3 -c 'import sys; print(int(round(float(sys.argv[1]) * 1000)))' "$avg_ms"
}

bms_lo_apply_netem() {  # $1=profile (LAN|WAN200|WAN50|WAN5) $2=port [$3=--wrong-combined-netem]
  # Shapes `lo`, not veth; exit 5 on any failure.
  #
  # I1 (gate fix round 1): a single aggregate limiter on `lo` is NOT the same
  # substrate as the standard harness's two independent per-party egress
  # limiters (one full-duplex rate available in EACH direction). This function
  # keys on the party-1 listen port ($2, the wrapper's own `port` variable --
  # 10501 for add-only, 1026 for add-del) to classify traffic by direction:
  #   dport == port  -- r (party 0, dialer) -> s (party 1, listener) -- class 1:10
  #   sport == port  -- s -> r                                        -- class 1:20
  #   anything else (the GC channel on add-del, etc.)                 -- class 1:30
  # `tc prio` does pure classification (no rate metering of its own -- an
  # `htb` root here was measured to under-deliver by ~30% under this delayed,
  # classified-loopback traffic pattern, see the task report); the per-class
  # child is netem(delay)+tbf(rate), the SAME split apply_split_shaper
  # (bench/lib.sh, Task 1) uses on a whole device -- applied per class instead
  # of per device because I1 requires the rate to be independent PER
  # DIRECTION, which a single whole-device apply_split_shaper call cannot
  # provide. Burst/latency are sized to ~one full RTT (2*delay_us) at the
  # profile rate, not apply_split_shaper's rate/250 constant: netem's delay is
  # fixed (unjittered), so it releases a whole RTT's worth of queued packets
  # in one batch, and a bucket sized for a single 250 Hz tick (right for the
  # veth pair's UNCLASSIFIED single-flow-per-device path) drops/retransmits
  # that batch on this shared, classified `lo` path (measured: rate/250 burst
  # -> WAN200 forward 174.2 Mbit / simultaneous reverse 137.0 Mbit, both
  # outside the 180-220 Mbit window; RTT-sized burst -> both inside).
  local profile=$1 port=$2 wrong_combined=0
  [ "${3:-}" = "--wrong-combined-netem" ] && wrong_combined=1
  local base_us delay_us
  ip netns exec "$BMS_LO_NS" tc qdisc del dev lo root 2>/dev/null || true
  base_us=$(bms_lo_measure_base_us 20) || return $?
  delay_us=$(( (${PROFILE_RTT_US[$profile]} - base_us) / 2 ))
  [ "$delay_us" -lt 0 ] && delay_us=0
  local rate_kbit=${PROFILE_RATE_KBIT[$profile]}
  local limit=${LIMIT:-10000}

  if [ "$wrong_combined" = 1 ]; then
    # Task 4 (C1, R6-NOTAUTO): TEST-ONLY -- verbatim the pre-fix defective
    # construction (bench/aws/run_calib.sh wires this into PHASE 2; never
    # called by a real trial).
    echo "WRONG-CONSTRUCTION: combined netem delay+rate on lo (TEST-ONLY, --wrong-combined-netem)" >&2
    ip netns exec "$BMS_LO_NS" tc qdisc replace dev lo root netem delay ${delay_us}us rate ${rate_kbit}kbit limit "$limit" || {
      echo "bms_lo_apply_netem: tc qdisc replace (wrong-combined, TEST-ONLY) failed for profile $profile" >&2
      return 5
    }
    return 0
  fi

  # burst = max(rate/250-tick floor, one-RTT-at-rate) -- at near-zero delay
  # (LAN) the RTT term collapses towards 0, so the rate/250 term (the SAME
  # floor apply_split_shaper uses -- e.g. 500000 B at 1 Gbit) still has to
  # carry the bucket; at high delay (WAN*) the RTT term dominates and is what
  # the classified-loopback path needs (see the function header comment).
  # Measured: RTT-term-only burst -- LAN forward capped at 310 Mbit (well
  # outside the 900-1100 Mbit window) because it floored to the 3000 B
  # minimum instead of 500000 B.
  local burst_tick=$(( rate_kbit * 1000 / 8 / 250 ))
  local burst_rtt=$(( rate_kbit * 1000 / 8 * 2 * delay_us / 1000000 ))
  local burst_bytes=$(( burst_tick > burst_rtt ? burst_tick : burst_rtt ))
  [ "$burst_bytes" -lt 3000 ] && burst_bytes=3000
  local latency_ms=$(( 2 * delay_us / 1000 ))
  [ "$latency_ms" -lt 50 ] && latency_ms=50

  {
    ip netns exec "$BMS_LO_NS" tc qdisc replace dev lo root handle 1: prio bands 3 priomap 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 &&
    ip netns exec "$BMS_LO_NS" tc qdisc add dev lo parent 1:1 handle 10: netem delay ${delay_us}us limit "$limit" &&
    ip netns exec "$BMS_LO_NS" tc qdisc add dev lo parent 10: handle 100: tbf rate ${rate_kbit}kbit burst ${burst_bytes} latency ${latency_ms}ms &&
    ip netns exec "$BMS_LO_NS" tc qdisc add dev lo parent 1:2 handle 20: netem delay ${delay_us}us limit "$limit" &&
    ip netns exec "$BMS_LO_NS" tc qdisc add dev lo parent 20: handle 200: tbf rate ${rate_kbit}kbit burst ${burst_bytes} latency ${latency_ms}ms &&
    ip netns exec "$BMS_LO_NS" tc qdisc add dev lo parent 1:3 handle 30: netem delay ${delay_us}us limit "$limit" &&
    ip netns exec "$BMS_LO_NS" tc qdisc add dev lo parent 30: handle 300: tbf rate ${rate_kbit}kbit burst ${burst_bytes} latency ${latency_ms}ms &&
    ip netns exec "$BMS_LO_NS" tc filter add dev lo parent 1: protocol ip prio 1 u32 match ip dport "$port" 0xffff flowid 1:1 &&
    ip netns exec "$BMS_LO_NS" tc filter add dev lo parent 1: protocol ip prio 1 u32 match ip sport "$port" 0xffff flowid 1:2
  } || {
    echo "bms_lo_apply_netem: tc construction (per-direction) failed for profile $profile port $port (delay ${delay_us}us rate ${rate_kbit}kbit)" >&2
    return 5
  }

  # C4/Task 2: STRUCTURAL check of the ACTUAL applied qdisc/class/filter tree
  # before any party launches -- necessary but not sufficient (it catches
  # no-op/partial apply, not a mis-shaped-but-present construction); Task 3's
  # bms_lo_verify_throughput adds the real measurement this alone cannot
  # provide (that is precisely what C1 found missing).
  local dump_q dump_c dump_f
  dump_q=$(ip netns exec "$BMS_LO_NS" tc qdisc show dev lo 2>&1)
  dump_c=$(ip netns exec "$BMS_LO_NS" tc class show dev lo 2>&1)
  dump_f=$(ip netns exec "$BMS_LO_NS" tc filter show dev lo parent 1: 2>&1)
  python3 - "$dump_q" "$dump_f" "$rate_kbit" "$port" <<'PYEOF'
import re, sys
dump_q, dump_f, want_rate_kbit, port = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
def fail(msg):
    sys.stderr.write("bms_lo_apply_netem: structural check FAILED -- %s\n" % msg)
    sys.exit(1)
if "prio" not in dump_q:
    fail("no prio root qdisc: %r" % dump_q)
mult = {"": 1, "K": 1000, "M": 1000000, "G": 1000000000}
for parent, handle in (("1:1", "10:"), ("1:2", "20:"), ("1:3", "30:")):
    if not re.search(r"netem %s parent %s" % (re.escape(handle), re.escape(parent)), dump_q):
        fail("no netem qdisc %s parented to %s: %r" % (handle, parent, dump_q))
    m = re.search(r"tbf [0-9]+: parent %s.*?rate ([0-9.]+)([KMG]?)bit" % re.escape(handle), dump_q)
    if not m:
        fail("no tbf child of %s with a rate: %r" % (handle, dump_q))
    got_kbit = (float(m.group(1)) * mult[m.group(2)]) / 1000.0
    if abs(got_kbit - want_rate_kbit) > 0.01 * want_rate_kbit:
        fail("tbf child of %s rate %.1f kbit != expected %d kbit" % (handle, got_kbit, want_rate_kbit))
# tc's default u32 dump has no human-readable dport/sport text; it prints the
# raw 32-bit match word instead -- dport 0xPPPP sits in the low half (mask
# 0000ffff), sport in the high half (mask ffff0000) of the word at offset 20.
port_hex = "%04x" % int(port)
if not re.search(r"match 0000%s/0000ffff at 20" % port_hex, dump_f, re.I):
    fail("no dport %s filter (flowid 1:1): %r" % (port, dump_f))
if not re.search(r"match %s0000/ffff0000 at 20" % port_hex, dump_f, re.I):
    fail("no sport %s filter (flowid 1:2): %r" % (port, dump_f))
if "flowid 1:1" not in dump_f or "flowid 1:2" not in dump_f:
    fail("filter flowid assignment missing: %r" % dump_f)
PYEOF
  [ $? -eq 0 ] || return 5
}

bms_lo_verify_throughput() {  # $1=profile $2=port -- REAL measured throughput on `lo`, both
  # directions and simultaneously (Task 3, C1/I1): the structural read-back
  # above cannot detect a present-but-wrong-shape construction (that is
  # exactly how the pre-fix combined netem+rate passed its own structural
  # check). exit 5 on any failure.
  local profile=$1 port=$2
  local rate_kbit=${PROFILE_RATE_KBIT[$profile]}
  local win_lo win_hi
  win_lo=$(python3 -c "print(int($rate_kbit*0.90))")
  win_hi=$(python3 -c "print(int($rate_kbit*1.10))")

  _bms_lo_iperf_server_ready() {
    local i
    for i in $(seq 1 25); do
      ip netns exec "$BMS_LO_NS" ss -ltn 2>/dev/null | grep -q ":$port " && return 0
      sleep 0.2
    done
    return 1
  }
  _bms_lo_read_sum_sent_kbit() {  # $1=json $2=key ("sum_sent") -> kbit, or exit 1 with a message
    # A missing key MUST be loud: on the first AWS run this function raised KeyError,
    # printed nothing, and the empty string sailed through the window check below
    # (`[ "" -lt N ]` is false, so "outside the window" was never true). That was a
    # silent pass on the simultaneous-reverse leg -- the exact assertion-integrity
    # class the Phase-8 gate exists to catch.
    python3 - "$1" "$2" <<'PY' || return 1
import json, sys
d = json.loads(sys.argv[1]); k = sys.argv[2]
try:
    print(int(round(d["end"][k]["bits_per_second"] / 1000.0)))
except KeyError:
    sys.stderr.write("bms_lo_verify_throughput: iperf3 JSON has no end.%s (iperf %s)\n" % (k, d.get("start", {}).get("version", "?")))
    sys.exit(1)
PY
  }
  _bms_lo_read_bidir_leg_kbit() {  # $1=json $2=forward|reverse -> sender-side kbit of that --bidir leg
    # Version-independent: iperf 3.9 (Ubuntu 22.04) has no end.sum_sent_bidir_reverse
    # (added in 3.10), but every version lists both legs under end.streams[], the
    # reverse (server->client) leg being the one whose sender.sender flag is false.
    python3 - "$1" "$2" <<'PY' || return 1
import json, sys
d = json.loads(sys.argv[1]); want = (sys.argv[2] == "forward")
legs = [s for s in d["end"]["streams"] if bool(s.get("sender", {}).get("sender")) == want]
if len(legs) != 1:
    sys.stderr.write("bms_lo_verify_throughput: --bidir JSON has %d %s leg(s) in end.streams, expected 1 (iperf %s)\n"
                     % (len(legs), sys.argv[2], d.get("start", {}).get("version", "?")))
    sys.exit(1)
print(int(round(legs[0]["sender"]["bits_per_second"] / 1000.0)))
PY
  }
  _bms_lo_check_window() {  # $1=label $2=kbit -- a non-integer (e.g. empty) is a FAILURE, never a pass
    case "$2" in ''|*[!0-9]*)
      echo "bms_lo_verify_throughput FAILED: profile $profile $1 throughput is not a number ('$2') -- measurement missing" >&2
      return 1 ;;
    esac
    if [ "$2" -lt "$win_lo" ] || [ "$2" -gt "$win_hi" ]; then
      echo "bms_lo_verify_throughput FAILED: profile $profile $1 throughput $2 kbit outside ${win_lo}..${win_hi} kbit (profile rate $rate_kbit kbit)" >&2
      return 1
    fi
  }

  # forward: client dials server:$port -- classified dport==$port -> class 1:10
  ip netns exec "$BMS_LO_NS" iperf3 -s -1 -p "$port" >/dev/null 2>&1 &
  _bms_lo_iperf_server_ready || { echo "bms_lo_verify_throughput: iperf3 server not ready on port $port (forward) after 5 s" >&2; return 5; }
  local json_fwd kbit_fwd
  json_fwd=$(ip netns exec "$BMS_LO_NS" iperf3 -c 127.0.0.1 -p "$port" -t 10 -O 2 -J) || { echo "bms_lo_verify_throughput: forward iperf3 client failed" >&2; return 5; }
  kbit_fwd=$(_bms_lo_read_sum_sent_kbit "$json_fwd" sum_sent) || return 5
  _bms_lo_check_window forward "$kbit_fwd" || return 5

  # reverse: -R -- server sends, client receives; classified sport==$port -> class 1:20
  ip netns exec "$BMS_LO_NS" iperf3 -s -1 -p "$port" >/dev/null 2>&1 &
  _bms_lo_iperf_server_ready || { echo "bms_lo_verify_throughput: iperf3 server not ready on port $port (reverse) after 5 s" >&2; return 5; }
  local json_rev kbit_rev
  json_rev=$(ip netns exec "$BMS_LO_NS" iperf3 -c 127.0.0.1 -p "$port" -t 10 -O 2 -J -R) || { echo "bms_lo_verify_throughput: reverse iperf3 client failed" >&2; return 5; }
  kbit_rev=$(_bms_lo_read_sum_sent_kbit "$json_rev" sum_sent) || return 5
  _bms_lo_check_window reverse "$kbit_rev" || return 5

  # simultaneous (I1's actual full-duplex property): --bidir runs a SINGLE
  # real duplex TCP connection with both directions transferring at once,
  # every packet still classified purely by port (dport==$port forward leg,
  # sport==$port reverse leg) -- a stronger simultaneity guarantee than two
  # separately-launched client processes racing to start together.
  ip netns exec "$BMS_LO_NS" iperf3 -s -1 -p "$port" >/dev/null 2>&1 &
  _bms_lo_iperf_server_ready || { echo "bms_lo_verify_throughput: iperf3 server not ready on port $port (simultaneous) after 5 s" >&2; return 5; }
  local json_bidir kbit_bidir_fwd kbit_bidir_rev
  json_bidir=$(ip netns exec "$BMS_LO_NS" iperf3 -c 127.0.0.1 -p "$port" -t 10 -O 2 -J --bidir) || { echo "bms_lo_verify_throughput: simultaneous (--bidir) iperf3 client failed" >&2; return 5; }
  kbit_bidir_fwd=$(_bms_lo_read_bidir_leg_kbit "$json_bidir" forward) || return 5
  kbit_bidir_rev=$(_bms_lo_read_bidir_leg_kbit "$json_bidir" reverse) || return 5
  _bms_lo_check_window "simultaneous-forward" "$kbit_bidir_fwd" || return 5
  _bms_lo_check_window "simultaneous-reverse" "$kbit_bidir_rev" || return 5

  echo "bms_lo_verify_throughput OK: profile $profile forward=$kbit_fwd reverse=$kbit_rev simultaneous(fwd=$kbit_bidir_fwd,rev=$kbit_bidir_rev) kbit, window ${win_lo}..${win_hi}" >&2
}

cfg_get() { python3 "$JSONL_CHECK" config --file "$1" --get "$2"; }

run_config_check() {  # $1 = config path; exit 3 on failure
  local err
  err=$(python3 "$JSONL_CHECK" config --file "$1" 2>&1 1>/dev/null)
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "$err" >&2
    exit 3
  fi
}

write_partial() {  # $1=out_path $2=rc_r $3=rc_s $4...=notes tokens
  local out=$1 rc_r=$2 rc_s=$3; shift 3
  python3 - "$out" "$rc_r" "$rc_s" "$@" <<'PYEOF'
import json, sys
path, rc_r, rc_s = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
tokens = sys.argv[4:]
with open(path, "w") as f:
    json.dump({"notes_tokens": tokens, "party_exit": {"r": rc_r, "s": rc_s}}, f)
PYEOF
}

# ---------------------------------------------------------------------------
# § I docker wiring: Party launch + PID discovery in docker mode (pinned
# helper, used by every docker wrapper -- phase-8-plan.md § I).
# ---------------------------------------------------------------------------
launch_party() {  # $1 side r|s, $2 container, $3 comm (<=15 chars), $4... command
  local side=$1 ctr=$2 comm=$3; shift 3
  docker exec -w /state "$ctr" stdbuf -oL -eL "$@" \
    >"$RUN_WORKDIR/party_$side.stdout" 2>"$RUN_WORKDIR/party_$side.stderr" &
  eval "WAIT_$side=$!"
  local p=""; for i in $(seq 1 100); do
    p=$(docker top "$ctr" -o pid,comm | awk -v c="$comm" 'NR>1 && $2==c {print $1; exit}')
    [ -n "$p" ] && break; sleep 0.1; done
  [ -n "$p" ] || { echo "party $side pid not found (comm=$comm)" >&2; return 5; }
  echo "$side=$p" >> "$RUN_WORKDIR/pids"
}

# ---------------------------------------------------------------------------
# validate --config C  (TV-F17 subject)
# ---------------------------------------------------------------------------
action_validate() {
  local config=""
  while [ $# -gt 0 ]; do case "$1" in --config) config=$2; shift 2 ;; *) shift ;; esac; done
  run_config_check "$config"

  local variant del_size delete_mode
  variant=$(cfg_get "$config" variant)
  del_size=$(cfg_get "$config" del_size)
  delete_mode=$(cfg_get "$config" delete_mode)

  case "$variant" in
    add-only|add-del) ;;
    *) echo "bms24: variant '$variant' not in {add-only, add-del}" >&2; exit 3 ;;
  esac

  # TV-F17: the addition binaries have no delete concept at all -- a
  # delete-bearing add-only config can only be dropped by OUR wrapper.
  if [ "$variant" = "add-only" ] && [ "${del_size:-0}" -gt 0 ] && [ "$delete_mode" != "reinsert-accumulate" ]; then
    echo "bms24-addonly: config contains deletions (del_size=$del_size); REFUSING -- Scenario-U add-only runs must set \"delete_mode\":\"reinsert-accumulate\" explicitly (TV-F17; plan W8.1)" >&2
    exit 2
  fi

  echo "bms24: validate OK: $config"
}

# ---------------------------------------------------------------------------
# prepare --config C --state-dir D
# ---------------------------------------------------------------------------
action_prepare() {
  local config="" state_dir=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --config) config=$2; shift 2 ;;
      --state-dir) state_dir=$2; shift 2 ;;
      *) shift ;;
    esac
  done

  local variant days daily_size start_size image bin
  variant=$(cfg_get "$config" variant)
  days=$(cfg_get "$config" days)
  daily_size=$(cfg_get "$config" daily_size)
  start_size=$(cfg_get "$config" start_size)
  image=$(cfg_get "$config" image)

  case "$variant" in
    add-only) bin=upsi/addition ;;
    add-del) bin=upsi/deletion ;;
    *) echo "bms24 prepare: variant '$variant' not in {add-only, add-del}" >&2; exit 3 ;;
  esac

  # FINDING (Task 32, first real docker-path exercise): the setup binary does
  # NOT create its own party subdirectories -- it hardcodes <data_dir>/p0/,
  # <data_dir>/p1/, <out_dir>/p0/, <out_dir>/p1/ (undocumented in --help;
  # discovered by direct execution: "NOT_FOUND: Open failed: Error opening
  # file work/out/p0//elgamal.pub" without them). Pre-create them.
  # SECOND FINDING: measure.sh (and this wrapper) always run as root, so
  # these directories are created root:root mode 755 -- but the image runs
  # as its own unprivileged `upsi-user` (uid 1000, confirmed via `docker run
  # ... id`), which then gets a bare "Permission denied" writing into a
  # root-owned bind mount (empirically reproduced and isolated: a plain
  # `touch` from the same image into a root:root 755 host dir fails the
  # same way; into a non-root-owned 775 dir it succeeds). world-writable
  # perms on this throwaway /tmp state_dir are the harness-level fix.
  mkdir -p "$state_dir/data/p0" "$state_dir/data/p1" "$state_dir/out/p0" "$state_dir/out/p1"
  chmod -R a+rwx "$state_dir"
  docker run --rm --cpuset-cpus=2 -v "$state_dir:/home/upsi-user/work" "$image" bash -lc \
    "cd /home/upsi-user && ./bazel-bin/$bin/setup --func=CA --days=$days --daily_size=$daily_size --start_size=$start_size --data_dir=work/data/ --out_dir=work/out/"
  local rc=$?
  [ "$rc" -eq 0 ] || exit 5
}

# ---------------------------------------------------------------------------
# run --config C --out P --workdir W
# ---------------------------------------------------------------------------
action_run() {
  [ "${SYMPSICA_MEASURE:-}" = "1" ] || { echo "run.sh must execute inside bench/measure.sh" >&2; exit 4; }
  : "${NS_R:?}" "${NS_S:?}" "${ADDR_R:?}" "${ADDR_S:?}" "${CORE_R:?}" "${CORE_S:?}" \
    "${STATE_WORK:?}" "${CONTAINER_R:?}" "${CONTAINER_S:?}" "${RUN_WORKDIR:?}"

  local config="" out="" workdir=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --config) config=$2; shift 2 ;;
      --out) out=$2; shift 2 ;;
      --workdir) workdir=$2; shift 2 ;;
      *) shift ;;
    esac
  done

  local variant days delete_mode bin port trees_flag gc_flag network image mem
  variant=$(cfg_get "$config" variant)
  days=$(cfg_get "$config" days)
  delete_mode=$(cfg_get "$config" delete_mode)
  network=$(cfg_get "$config" network)
  image=$(cfg_get "$config" image)
  mem=$(cfg_get "$config" mem)

  case "$variant" in
    add-only) bin=upsi/addition; port=10501; trees_flag=--trees; gc_flag="" ;;
    add-del)  bin=upsi/deletion; port=1026;  trees_flag=--import; gc_flag=--gc_port=1025 ;;
    *) echo "bms24 run: variant '$variant' not in {add-only, add-del}" >&2; exit 3 ;;
  esac

  # A10: build the ad hoc single-netns loopback pair for THIS trial; torn down
  # unconditionally on process exit (this trap covers every exit path in this
  # function, including the various `exit $?` early-outs below -- it does NOT
  # survive a SIGKILL from measure.sh's own timeout, which is a known residual
  # gap noted in the task report; the NEXT trial's bms_lo_up() removes any
  # stale containers before creating fresh ones, and the on-instance runbook
  # carries a belt-and-suspenders `docker rm -f` of these exact names too).
  trap bms_lo_teardown EXIT
  bms_lo_up "$image" "$mem" || exit $?
  bms_lo_apply_netem "$network" "$port" || exit $?
  # Task 3 (C1/I1, gate fix round 1): REAL measured throughput on the actual
  # `lo` path, both directions and simultaneously, before either party
  # launches -- the structural read-back inside bms_lo_apply_netem cannot
  # detect a present-but-wrong-shape construction (that is exactly how the
  # pre-fix combined netem+rate passed its own structural check and still
  # shipped a mislabeled AWS timing row). ~35 s cost, accepted (task brief).
  bms_lo_verify_throughput "$network" "$port" || exit $?

  phase total_workload_begin
  phase_zero setup_once
  phase_zero preprocessing

  # Combined byte accounting (A10 clause 2/3): measured from the ONE shared
  # `lo`'s tx delta, taken AFTER bms_lo_up/bms_lo_apply_netem/
  # bms_lo_verify_throughput's OWN calibration and iperf3 traffic (which must
  # not be counted) and around the real protocol traffic.
  local lo_tx_before
  lo_tx_before=$(tx_bytes "$BMS_LO_NS" lo)

  launch_party s "$BMS_LO_CTR_S" run bash -lc \
    "cd /home/upsi-user && ./bazel-bin/$bin/run --party=1 --port=0.0.0.0:$port $gc_flag --days=$days --func=CA $trees_flag --data_dir=/state/data/ --out_dir=/state/out/" \
    || exit $?

  local t1
  t1=$(mono_ns)

  # A7 (controller ruling, task-32-report.md): 60 s was measured insufficient for bms24's
  # tree-deserialization time on this VM (173 s observed for add-only n=2^16) -- wait_listen
  # sits inside total_workload, strictly outside online (see C's boundary rule), so widening
  # it cannot move any measured number, it only changes the give-up threshold. 600 s = ~3.5x
  # headroom over the 173 s measurement, sized for Task 34's native-AWS run at n=2^20 (16x
  # the elements) rather than this figure.
  #
  # DEADLOCK-BY-CONSTRUCTION FINDING (task-32-report.md), NOT a timeout question for
  # add-del: party 1's deletion/run binds+listens on the GC port (1025) immediately/fast,
  # independent of party 0 -- but its gRPC port ($port, 1026) does NOT open until AFTER the
  # GC handshake with party 0 completes (empirically isolated with a real netns+DNAT pair:
  # party 1 alone leaves 1026 closed and its process CPU-idle indefinitely; with party 0
  # launched and connected via GC, 1026 opened ~14 s later). So for add-del, pre-waiting on
  # $port before party 0 even exists can never succeed at ANY timeout -- the correct (and
  # only possible) pre-launch readiness gate is the GC port. For add-only there is no GC
  # channel at all, and $port genuinely does come up on its own (confirmed: party 1 alone
  # prints "[PartyOne] listening" without party 0), so $port remains the right gate there.
  # A10: both waits now target $BMS_LO_NS (the shared loopback netns), not $NS_S.
  if [ "$variant" = "add-del" ]; then
    wait_listen "$BMS_LO_NS" 1025 600 || exit $?
  else
    wait_listen "$BMS_LO_NS" "$port" 600 || exit $?
  fi

  # W8.1 pinned 3 s stagger: max(readiness, 3 s) since party-1 launch.
  while [ $(( $(mono_ns) - t1 )) -lt 3000000000 ]; do sleep 0.1; done

  phase online_begin

  # A10: party 0 dials the peer's REAL shared loopback address -- both parties
  # are literally in the same netns now, so BMS+24's gRPC local credentials
  # (exact-match 127.0.0.1, measured -- see the module header) accept the
  # connection on both ends. The GC channel's own hardcoded 127.0.0.1:1025
  # (party 0, upstream, deletion only) now reaches party 1 directly; no DNAT
  # is applied.
  launch_party r "$BMS_LO_CTR_R" run bash -lc \
    "cd /home/upsi-user && ./bazel-bin/$bin/run --party=0 --port=127.0.0.1:$port $gc_flag --days=$days --func=CA $trees_flag --data_dir=/state/data/ --out_dir=/state/out/" \
    || exit $?

  wait "$WAIT_r"; local rc_r=$?
  wait "$WAIT_s"; local rc_s=$?

  phase online_end
  phase total_workload_end

  local lo_tx_after combined_total
  lo_tx_after=$(tx_bytes "$BMS_LO_NS" lo)
  combined_total=$(( lo_tx_after - lo_tx_before ))

  # Party 0 (r) prints "[PartyZero] CA/SUM = <int>" (deletion) or
  # "[PartyZero] CARDINALITY = <int>" (addition).
  #
  # I2 (gate fix round 1, Task 5): the internal comm-byte counter is parsed
  # from BOTH parties' stdout, not only party_r, and tagged by MEASURED scope
  # (see bench/jsonl_check.py build_record for the comparison rule):
  #   - add-only: party 1 (s) prints "Total Comm (B):\t<int>" -- measured
  #     against combined_total_b in the same colima smoke run: 2,681,284 B
  #     vs 2,689,437 B (ratio 0.997), i.e. it is ALREADY the combined total,
  #     not a directional half -- emitted as internal_comm_b.
  #   - add-del: party 0 (r) prints "Total Comm Sent(B):\t<int>" -- the label
  #     says "Sent", i.e. that party's own outgoing bytes only (directional);
  #     party 1 never prints an equivalent line, so this can only ever be a
  #     lone directional half -- emitted as internal_b_r (never compared
  #     alone; build_record marks it unavailable(scope-mismatch)).
  local result=na internal_b_r="" internal_comm_b=""
  if [ -f "$RUN_WORKDIR/party_r.stdout" ]; then
    local m
    m=$(sed -nE 's/^\[PartyZero\] (CA\/SUM|CARDINALITY) = ([0-9]+)$/\2/p' "$RUN_WORKDIR/party_r.stdout" | tail -1)
    [ -n "$m" ] && result=$m
    internal_b_r=$(sed -nE 's/^Total Comm Sent\(B\):[[:space:]]*([0-9]+)$/\1/p' "$RUN_WORKDIR/party_r.stdout" | tail -1)
  fi
  if [ -f "$RUN_WORKDIR/party_s.stdout" ]; then
    internal_comm_b=$(sed -nE 's/^\[PartyOne\] Total Comm \(B\):[[:space:]]*([0-9]+)$/\1/p' "$RUN_WORKDIR/party_s.stdout" | tail -1)
  fi

  local tokens=("result=$result" "bytes=combined-loopback" "combined_total_b=$combined_total"
                "lo_mtu=1500" "shaping=lo-per-direction(netem-tbf)")
  [ -n "$internal_b_r" ] && tokens+=("internal_b_r=$internal_b_r")
  [ -n "$internal_comm_b" ] && tokens+=("internal_comm_b=$internal_comm_b")
  [ -n "$delete_mode" ] && tokens+=("delete_mode=$delete_mode")

  write_partial "$out" "$rc_r" "$rc_s" "${tokens[@]}"

  if [ "$rc_r" -eq 0 ] && [ "$rc_s" -eq 0 ]; then
    exit 0
  fi
  exit 5
}

# ---------------------------------------------------------------------------
# PUBLIC no-action form: run.sh --config C --out O [--env E] [--trials N] [--warmup K]
# (.handoff/sympsica-plan.md:376)
# ---------------------------------------------------------------------------
action_public() {
  local config="" out="" env=LOCAL trials="" warmup=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --config) config=$2; shift 2 ;;
      --out) out=$2; shift 2 ;;
      --env) env=$2; shift 2 ;;
      --trials) trials=$2; shift 2 ;;
      --warmup) warmup=$2; shift 2 ;;
      *) echo "run.sh: unknown argument $1" >&2; exit 3 ;;
    esac
  done
  [ -n "$config" ] && [ -n "$out" ] || { echo "run.sh: --config and --out are required" >&2; exit 3; }

  local state_dir key
  state_dir=$(cfg_get "$config" state_dir)
  key=$(python3 "$JSONL_CHECK" config --file "$config" --prepare-key)
  if [ ! -f "$state_dir/.prepared" ] || [ "$(cat "$state_dir/.prepared" 2>/dev/null)" != "$key" ]; then
    bash "$REPO/bench/measure.sh" prepare --wrapper "$SELF" --config "$config" --env "$env"
  fi

  local run_args=(run --wrapper "$SELF" --config "$config" --out "$out" --env "$env")
  [ -n "$trials" ] && run_args+=(--trials "$trials")
  [ -n "$warmup" ] && run_args+=(--warmup "$warmup")
  exec bash "$REPO/bench/measure.sh" "${run_args[@]}"
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
main() {
  case "${1:-}" in
    validate) shift; action_validate "$@" ;;
    prepare) shift; action_prepare "$@" ;;
    run) shift; action_run "$@" ;;
    check-result) shift; echo "baselines/bms24/run.sh: check-result not applicable to bms24" >&2; exit 3 ;;
    *) action_public "$@" ;;
  esac
}

# Sourceable (gate fix round 1, Task 4): bench/aws/run_calib.sh's --wrong-combined-netem
# expected-negative sources this file to reach bms_lo_apply_netem/bms_lo_verify_throughput
# directly (a lightweight netns, no docker pair, is enough to exercise the shaping/verify
# path) without invoking main(). Same convention as bench/netem.sh's own guard --
# BASH_SOURCE[0] equals $0 only when the file is executed directly.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
