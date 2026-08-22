#!/usr/bin/env bash
# bench/netem.sh -- Phase 8 W8.7 (phase-8-plan.md "netem spec"; master plan
# W8.7, design line 62): the netem shaping and verification harness. Subject
# of TV-F16 (test/failure_suite.cpp) -- see the --wrong-one-sided TEST-ONLY
# flag below.
#
# All flags whose long name starts with --wrong- are TEST-ONLY: they exist
# solely to construct the deliberately-wrong inputs the R6-NOTAUTO evidence
# rule requires, and are never used by measure.sh or any baseline wrapper.
#
# Exit codes (Global constraint 11): 0 ok; 2 verification/assertion failure;
# 3 usage/config error; 4 environment missing (not root, no sch_netem, no
# netns pair, iperf3 not ready).
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(dirname "$SCRIPT_DIR")
# shellcheck source=bench/lib.sh
source "$SCRIPT_DIR/lib.sh"

declare -A PROFILE_RTT_US=(["LAN"]=200 ["WAN200"]=80000 ["WAN50"]=80000 ["WAN5"]=80000)
declare -A PROFILE_RATE_KBIT=(["LAN"]=1000000 ["WAN200"]=200000 ["WAN50"]=50000 ["WAN5"]=5000)
LIMIT=10000
APPLIED_STATE_FILE=/tmp/sympsica-bench/netem-applied.json
CALIB_DIR_AWS="$REPO_ROOT/bench/calib"
CALIB_DIR_LOCAL=/tmp/sympsica-bench/calib

# FT12 -- verbatim, printed by --help and archived in every calibration record.
FT12_SENTENCE="delay values are per-interface egress on a veth PAIR; the target is ROUND-TRIP time; never apply these numbers to loopback, which traverses the qdisc twice."

# ---------------------------------------------------------------------------
# preconditions
# ---------------------------------------------------------------------------
require_root() {
  [ "$(id -u)" = "0" ] || { echo "netem.sh must run as root" >&2; exit 4; }
}
require_netem_module() {
  modprobe sch_netem 2>/dev/null || { echo "netem.sh: modprobe sch_netem failed" >&2; exit 4; }
}
require_netns_pair() {
  ip netns exec sympsica_r true 2>/dev/null && ip netns exec sympsica_s true 2>/dev/null \
    || { echo "netem.sh: netns pair sympsica_r/sympsica_s not found -- run 'netns-up' first" >&2; exit 4; }
}
valid_profile() {
  case "$1" in LAN|WAN200|WAN50|WAN5) return 0 ;; *) return 1 ;; esac
}

# ---------------------------------------------------------------------------
# netns topology (§ I)
# ---------------------------------------------------------------------------
cmd_netns_up() {
  ip netns del sympsica_r 2>/dev/null
  ip netns del sympsica_s 2>/dev/null
  ip netns add sympsica_r && ip netns add sympsica_s
  ip link add veth_r type veth peer name veth_s
  ip link set veth_r netns sympsica_r && ip link set veth_s netns sympsica_s
  # M1 (gate fix round 1, Task 7): disable IPv6 BEFORE bringing the link up.
  # This traffic was misattributed in a prior comment to netem.sh's own
  # calibration pings; that is impossible (measure.sh snapshots veth byte
  # counters AFTER apply/verify complete, i.e. after the pings already
  # finished). tcpdump on veth_r identified the real source: IPv6 router
  # solicitation / neighbor solicitation / multicast-listener-report traffic
  # the kernel generates when the interface transitions up. Measured
  # (colima, 3 trials each, 30 s idle window): 140/252/210 B WITHOUT this
  # sysctl; 0/0/42 B WITH it applied here (the residual is the unavoidable
  # link-exists-but-sysctl-not-yet-applied race, not a leftover of this
  # mechanism failing).
  ip netns exec sympsica_r sysctl -q net.ipv6.conf.veth_r.disable_ipv6=1
  ip netns exec sympsica_s sysctl -q net.ipv6.conf.veth_s.disable_ipv6=1
  ip netns exec sympsica_r ip addr add 10.99.0.1/24 dev veth_r
  ip netns exec sympsica_s ip addr add 10.99.0.2/24 dev veth_s
  ip netns exec sympsica_r ip link set lo up; ip netns exec sympsica_r ip link set veth_r up
  ip netns exec sympsica_s ip link set lo up; ip netns exec sympsica_s ip link set veth_s up
}

cmd_netns_down() {
  ip netns del sympsica_r 2>/dev/null || true
  ip netns del sympsica_s 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# clear
# ---------------------------------------------------------------------------
cmd_clear() {
  ip netns exec sympsica_r tc qdisc del dev veth_r root 2>/dev/null || true
  ip netns exec sympsica_s tc qdisc del dev veth_s root 2>/dev/null || true
  rm -f "$APPLIED_STATE_FILE"
}

# ---------------------------------------------------------------------------
# RTT measurement (shared by apply/verify/calibrate) -- the pinned parse+round
# ---------------------------------------------------------------------------
measure_rtt_us() {  # $1 = ping count -> prints integer microseconds
  local count=$1 avg_ms
  avg_ms=$(ip netns exec sympsica_r ping -c "$count" -i 0.2 -q 10.99.0.2 2>/dev/null \
           | sed -n 's#^rtt min/avg/max/mdev = [0-9.]*/\([0-9.]*\)/.*#\1#p')
  python3 -c 'import sys; print(int(round(float(sys.argv[1]) * 1000)))' "$avg_ms"
}

# ---------------------------------------------------------------------------
# apply --profile P [--wrong-one-sided] [--wrong-rate-double]
# ---------------------------------------------------------------------------
cmd_apply() {
  local profile="" wrong_one_sided=0 wrong_rate_double=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --profile) profile=$2; shift 2 ;;
      --wrong-one-sided) wrong_one_sided=1; shift ;;
      --wrong-rate-double) wrong_rate_double=1; shift ;;
      *) echo "netem apply: unknown argument $1" >&2; exit 3 ;;
    esac
  done
  [ -n "$profile" ] || { echo "netem apply: --profile is required" >&2; exit 3; }
  valid_profile "$profile" || { echo "netem apply: unknown profile '$profile'" >&2; exit 3; }
  require_netns_pair

  # step 1: clear is UNCONDITIONAL -- base RTT is always measured with no qdisc.
  cmd_clear

  # step 2: base_us
  local base_us
  base_us=$(measure_rtt_us 20)

  # step 3: delay_us = max(0, (RTT_us - base_us) / 2); rate_kbit, doubled under TEST-ONLY flag
  local rtt_us=${PROFILE_RTT_US[$profile]}
  local delay_us=$(( (rtt_us - base_us) / 2 ))
  [ "$delay_us" -lt 0 ] && delay_us=0
  local rate_kbit=${PROFILE_RATE_KBIT[$profile]}
  local rate_double=false
  if [ "$wrong_rate_double" = 1 ]; then
    rate_kbit=$(( rate_kbit * 2 ))
    rate_double=true
    echo "WRONG-CONSTRUCTION: rate doubled (TEST-ONLY)" >&2
  fi

  # step 3b: tbf burst. A token bucket that cannot hold one timer tick's worth of
  # tokens under-delivers, so burst >= rate/HZ; sized at rate/250 (HZ=250 here) with a
  # 2*MTU floor to keep the low-rate profiles' buckets sane.
  local burst_bytes=$(( rate_kbit * 1000 / 8 / 250 ))
  [ "$burst_bytes" -lt 3000 ] && burst_bytes=3000

  # step 4: shape both veth ends -- except one-sided under the TEST-ONLY flag.
  #
  # The delay and the rate MUST live in separate qdiscs. A single netem instance
  # carrying both `delay` and `rate` dequeues roughly one packet per DELAY interval
  # instead of pipelining, capping throughput near MTU/delay whatever rate is asked
  # for. Measured on c6i.8xlarge / 6.8.0-1061-aws (2026-08-22), LAN, target 1 Gbit:
  #   netem delay only ............ 16554.6 Mbit
  #   netem rate only .............   934.6 Mbit
  #   netem delay + rate together .   142.7 Mbit  <- 88.8 us/pkt against delay 96us
  #   netem delay + tbf rate ......   952.6 Mbit  <- this construction
  # Each element is correct alone; only their combination in one netem is broken.
  # (Not reproducible under colima/6.8.0-117-generic, where the combined form is
  # accurate -- so this is host-dependent and the split is the portable form.)
  # apply_split_shaper (bench/lib.sh, Task 1 of the Phase-8 gate fix round) is
  # the SAME function baselines/bms24/run.sh uses for its own `lo` path, so
  # this construction can never again drift between the two paths the way it
  # did before (C1).
  apply_split_shaper sympsica_r veth_r "$delay_us" "$rate_kbit" || {
    echo "netem apply: tc qdisc replace failed on veth_r" >&2
    exit 2
  }
  local one_sided=false
  if [ "$wrong_one_sided" = 1 ]; then
    one_sided=true
    echo "WRONG-CONSTRUCTION: one-sided delay applied (TEST-ONLY)" >&2
  else
    apply_split_shaper sympsica_s veth_s "$delay_us" "$rate_kbit" || {
      echo "netem apply: tc qdisc replace failed on veth_s" >&2
      exit 2
    }
  fi

  # step 5: applied-state JSON
  mkdir -p "$(dirname "$APPLIED_STATE_FILE")"
  local boot_id applied_ns
  boot_id=$(cat /proc/sys/kernel/random/boot_id)
  applied_ns=$(mono_ns)
  python3 - "$APPLIED_STATE_FILE" "$profile" "$rtt_us" "$base_us" "$delay_us" "$rate_kbit" \
    "$LIMIT" "$one_sided" "$rate_double" "$boot_id" "$applied_ns" "$burst_bytes" <<'PYEOF'
import json, sys
(path, profile, rtt_us, base_us, delay_us, rate_kbit, limit,
 one_sided, rate_double, boot_id, applied_ns, burst_bytes) = sys.argv[1:13]
data = {
    "profile": profile, "rtt_target_us": int(rtt_us), "base_us": int(base_us),
    "delay_us": int(delay_us), "rate_kbit": int(rate_kbit), "limit": int(limit),
    "one_sided": one_sided == "true", "rate_double": rate_double == "true",
    "boot_id": boot_id, "applied_ns": int(applied_ns),
    # shaper structure is provenance: delay and rate are deliberately in separate
    # qdiscs (see the comment at step 4) -- a record made under the combined form
    # is not comparable with one made under this form.
    "shaper": "netem-delay+tbf-rate", "burst_bytes": int(burst_bytes),
}
tmp = path + ".tmp"
with open(tmp, "w") as f:
    json.dump(data, f)
import os
os.replace(tmp, path)
PYEOF
}

# ---------------------------------------------------------------------------
# check_rtt / check_throughput -- shared by verify and calibrate
# ---------------------------------------------------------------------------
check_rtt() {  # $1 profile, $2 ping count -> prints measured_us to stdout; exit 2 on failure
  local profile=$1 count=$2
  local rtt_us=${PROFILE_RTT_US[$profile]}
  local measured_us lo hi
  measured_us=$(measure_rtt_us "$count")
  lo=$(python3 -c "print(int($rtt_us*0.9))")
  hi=$(python3 -c "print(int($rtt_us*1.1))")
  if [ "$measured_us" -lt "$lo" ] || [ "$measured_us" -gt "$hi" ]; then
    printf 'netem verify FAILED: profile %s measured RTT %s ms outside %s..%s ms (target %s ms; delay must sit on BOTH veth ends -- one-sided shaping halves it)\n' \
      "$profile" "$(python3 -c "print($measured_us/1000.0)")" "$(python3 -c "print($lo/1000.0)")" \
      "$(python3 -c "print($hi/1000.0)")" "$(python3 -c "print($rtt_us/1000.0)")" >&2
    exit 2
  fi
  echo "$measured_us"
}

check_throughput() {  # $1 profile -> prints measured_kbit to stdout; exit 2/4 on failure
  local profile=$1
  local rate_kbit=${PROFILE_RATE_KBIT[$profile]}
  ip netns exec sympsica_s iperf3 -s -1 -p 5201 >/dev/null 2>&1 &
  local i ready=0
  for i in $(seq 1 25); do
    ip netns exec sympsica_s ss -ltn 2>/dev/null | grep -q ':5201 ' && { ready=1; break; }
    sleep 0.2
  done
  [ "$ready" = 1 ] || { echo "netem verify: iperf3 server not ready on port 5201 after 5 s" >&2; exit 4; }
  local json bps measured_kbit lo hi
  json=$(ip netns exec sympsica_r iperf3 -c 10.99.0.2 -p 5201 -t 15 -O 3 -J)
  # sum_sent, NOT sum_received. Both report the same byte count, but the receiver's
  # own elapsed-time denominator inflates at high rate on this host, which understates
  # the rate by the ratio of the two intervals. Measured on c6i.8xlarge (2026-08-22),
  # LAN profile, the same single iperf3 run:
  #   sum_sent      956.3 Mbit  bytes=1793064960  seconds=15.00  retr=0
  #   sum_received  574.6 Mbit  bytes=1801328112  seconds=25.08
  # The per-second sender series was flat at 954-965 Mbit for the whole transfer with
  # zero retransmits, so the data genuinely moved at ~956 Mbit and it is the receiver's
  # timer that is the artifact. WAN200 in the same run: 189.4 vs 190.0 over 15.00/15.09 s
  # -- the two agree wherever the receiver interval is sane, so this is safe for every
  # profile, not a LAN-only patch. Sender-side buffering cannot inflate this materially:
  # tbf latency is 50 ms, i.e. <=0.35% of a 15 s window.
  bps=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d["end"]["sum_sent"]["bits_per_second"])' "$json")
  measured_kbit=$(python3 -c 'import sys; print(int(round(float(sys.argv[1])/1000.0)))' "$bps")
  lo=$(python3 -c "print(int($rate_kbit*0.9))")
  hi=$(python3 -c "print(int($rate_kbit*1.1))")
  if [ "$measured_kbit" -lt "$lo" ] || [ "$measured_kbit" -gt "$hi" ]; then
    printf 'netem verify FAILED: profile %s throughput %s Mbit outside %s..%s Mbit (profile rate %s Mbit)\n' \
      "$profile" "$(python3 -c "print($measured_kbit/1000.0)")" "$(python3 -c "print($lo/1000.0)")" \
      "$(python3 -c "print($hi/1000.0)")" "$(python3 -c "print($rate_kbit/1000.0)")" >&2
    exit 2
  fi
  echo "$measured_kbit"
}

# ---------------------------------------------------------------------------
# structural verify -- the ONLY LAN mode on colima
# ---------------------------------------------------------------------------
verify_structural() {
  local profile=$1
  [ -f "$APPLIED_STATE_FILE" ] || { echo "netem structural verify FAILED: nothing applied" >&2; exit 2; }
  local dump_r dump_s
  dump_r=$(ip netns exec sympsica_r tc qdisc show dev veth_r 2>&1)
  dump_s=$(ip netns exec sympsica_s tc qdisc show dev veth_s 2>&1)
  python3 - "$APPLIED_STATE_FILE" "$dump_r" "$dump_s" <<'PYEOF'
import json, re, sys
state_path, dump_r, dump_s = sys.argv[1], sys.argv[2], sys.argv[3]
with open(state_path) as f:
    st = json.load(f)
delay_us, rate_kbit = st["delay_us"], st["rate_kbit"]
DELAY_RE = re.compile(r"delay ([0-9.]+)(us|ms|s)")
RATE_RE = re.compile(r"rate ([0-9.]+)([KMG]?)bit")
DELAY_MULT = {"us": 1, "ms": 1000, "s": 1000000}
RATE_MULT = {"": 1, "K": 1000, "M": 1000000, "G": 1000000000}

def fail(side, dump):
    print('netem structural verify FAILED: %s dump "%s" vs applied delay %dus rate %dkbit'
          % (side, dump.strip(), delay_us, rate_kbit))
    sys.exit(2)

def check(side, dump):
    if "netem" not in dump:
        fail(side, dump)
    dm = DELAY_RE.search(dump)
    if delay_us == 0:
        ok_delay = (dm is None) or (float(dm.group(1)) * DELAY_MULT[dm.group(2)] == 0)
    else:
        ok_delay = dm is not None and abs(float(dm.group(1)) * DELAY_MULT[dm.group(2)] - delay_us) <= 1
    if not ok_delay:
        fail(side, dump)
    rm = RATE_RE.search(dump)
    if rm is None:
        fail(side, dump)
    got_kbit = (float(rm.group(1)) * RATE_MULT[rm.group(2)]) / 1000.0
    if abs(got_kbit - rate_kbit) > 0.01 * rate_kbit:
        fail(side, dump)

check("r", dump_r)
check("s", dump_s)
PYEOF
  local rc=$?
  [ "$rc" -eq 0 ] || exit "$rc"
}

# ---------------------------------------------------------------------------
# verify --profile P [--rtt-only] [--quick] [--structural]
# ---------------------------------------------------------------------------
cmd_verify() {
  local profile="" rtt_only=0 quick=0 structural=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --profile) profile=$2; shift 2 ;;
      --rtt-only) rtt_only=1; shift ;;
      --quick) quick=1; shift ;;
      --structural) structural=1; shift ;;
      *) echo "netem verify: unknown argument $1" >&2; exit 3 ;;
    esac
  done
  [ -n "$profile" ] || { echo "netem verify: --profile is required" >&2; exit 3; }
  valid_profile "$profile" || { echo "netem verify: unknown profile '$profile'" >&2; exit 3; }
  require_netns_pair

  if [ "$structural" = 1 ]; then
    verify_structural "$profile"
    return 0
  fi

  local count=20
  [ "$quick" = 1 ] && count=5
  check_rtt "$profile" "$count" >/dev/null
  local rc=$?
  [ "$rc" -eq 0 ] || exit "$rc"

  [ "$rtt_only" = 1 ] && return 0

  check_throughput "$profile" >/dev/null
  rc=$?
  [ "$rc" -eq 0 ] || exit "$rc"
}

# ---------------------------------------------------------------------------
# calibrate --profile P --date D --env LOCAL|AWS [--rtt-only|--structural]
# ---------------------------------------------------------------------------
cmd_calibrate() {
  local profile="" date="" env="" mode_flag=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --profile) profile=$2; shift 2 ;;
      --date) date=$2; shift 2 ;;
      --env) env=$2; shift 2 ;;
      --rtt-only) mode_flag=rtt-only; shift ;;
      --structural) mode_flag=structural; shift ;;
      *) echo "netem calibrate: unknown argument $1" >&2; exit 3 ;;
    esac
  done
  [ -n "$profile" ] && [ -n "$date" ] && [ -n "$env" ] \
    || { echo "netem calibrate: --profile, --date and --env are required" >&2; exit 3; }
  valid_profile "$profile" || { echo "netem calibrate: unknown profile '$profile'" >&2; exit 3; }
  case "$date" in [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;; \
    *) echo "netem calibrate: --date must be YYYY-MM-DD" >&2; exit 3 ;; esac
  case "$env" in LOCAL|AWS) ;; *) echo "netem calibrate: --env must be LOCAL or AWS" >&2; exit 3 ;; esac

  local mode
  if [ "$env" = "AWS" ]; then
    [ -z "$mode_flag" ] || { echo "netem calibrate: AWS permits only full calibration (no --rtt-only/--structural)" >&2; exit 3; }
    mode="full"
  elif [ "$profile" = "LAN" ]; then
    [ "$mode_flag" = "structural" ] || { echo "netem calibrate: LOCAL LAN calibration requires --structural (full mode is unreachable on this substrate)" >&2; exit 3; }
    mode="structural"
  else
    [ "$mode_flag" = "rtt-only" ] || { echo "netem calibrate: LOCAL WAN* calibration requires --rtt-only (full mode needs AWS)" >&2; exit 3; }
    mode="rtt-only"
  fi

  require_netns_pair
  cmd_apply --profile "$profile"

  local rtt_us=${PROFILE_RTT_US[$profile]} rate_kbit=${PROFILE_RATE_KBIT[$profile]}
  local base_us delay_us
  base_us=$(python3 -c 'import json; print(json.load(open("'"$APPLIED_STATE_FILE"'"))["base_us"])')
  delay_us=$(python3 -c 'import json; print(json.load(open("'"$APPLIED_STATE_FILE"'"))["delay_us"])')

  local measured_rtt_us=null measured_thr_kbit=null
  case "$mode" in
    structural)
      verify_structural "$profile"
      ;;
    rtt-only)
      measured_rtt_us=$(check_rtt "$profile" 20)
      local rc=$?
      [ "$rc" -eq 0 ] || exit "$rc"
      ;;
    full)
      measured_rtt_us=$(check_rtt "$profile" 20)
      local rc=$?
      [ "$rc" -eq 0 ] || exit "$rc"
      measured_thr_kbit=$(check_throughput "$profile")
      rc=$?
      [ "$rc" -eq 0 ] || exit "$rc"
      ;;
  esac

  local dump_r dump_s
  dump_r=$(ip netns exec sympsica_r tc qdisc show dev veth_r 2>&1)
  dump_s=$(ip netns exec sympsica_s tc qdisc show dev veth_s 2>&1)

  local calib_dir
  if [ "$env" = "AWS" ]; then calib_dir="$CALIB_DIR_AWS"; else calib_dir="$CALIB_DIR_LOCAL"; fi
  mkdir -p "$calib_dir"
  local archive_path
  if [ "$mode" = "full" ]; then
    archive_path="$calib_dir/${profile}-${date}.json"
  else
    archive_path="$calib_dir/${profile}-${date}-${mode}.json"
  fi

  local boot_id
  boot_id=$(cat /proc/sys/kernel/random/boot_id)

  python3 - "$archive_path" "$profile" "$env" "$mode" "$date" "$boot_id" "$rtt_us" "$base_us" \
    "$delay_us" "$rate_kbit" "$LIMIT" "$measured_rtt_us" "$measured_thr_kbit" "$dump_r" "$dump_s" \
    "$FT12_SENTENCE" <<'PYEOF'
import json, os, sys
(archive_path, profile, env, mode, date, boot_id, rtt_us, base_us, delay_us,
 rate_kbit, limit, measured_rtt_us, measured_thr_kbit, dump_r, dump_s, note) = sys.argv[1:17]


def optint(s):
    return None if s == "null" else int(s)


data = {
    "profile": profile, "env": env, "mode": mode, "date": date, "boot_id": boot_id,
    "target_rtt_us": int(rtt_us), "base_rtt_us": int(base_us),
    "applied_delay_us": int(delay_us), "rate_kbit": int(rate_kbit), "limit": int(limit),
    "measured_rtt_us": optint(measured_rtt_us), "measured_throughput_kbit": optint(measured_thr_kbit),
    "qdisc_dump": {"r": dump_r, "s": dump_s},
    "loopback_note": note,
}
tmp = archive_path + ".tmp"
with open(tmp, "w") as f:
    json.dump(data, f)
os.replace(tmp, archive_path)
PYEOF

  local sentinel="/tmp/sympsica-netem-ok-${profile}-${boot_id}"
  python3 - "$sentinel" "$profile" "$mode" "$archive_path" "$boot_id" <<'PYEOF'
import json, os, sys
path, profile, mode, archive, boot_id = sys.argv[1:6]
tmp = path + ".tmp"
with open(tmp, "w") as f:
    json.dump({"profile": profile, "mode": mode, "archive": archive, "boot_id": boot_id}, f)
os.replace(tmp, path)
PYEOF
}

# ---------------------------------------------------------------------------
# show
# ---------------------------------------------------------------------------
cmd_show() {
  echo "=== veth_r (sympsica_r) ==="
  ip netns exec sympsica_r tc qdisc show dev veth_r 2>&1
  echo "=== veth_s (sympsica_s) ==="
  ip netns exec sympsica_s tc qdisc show dev veth_s 2>&1
  echo "=== applied-state file ($APPLIED_STATE_FILE) ==="
  if [ -f "$APPLIED_STATE_FILE" ]; then cat "$APPLIED_STATE_FILE"; echo; else echo "(none)"; fi
}

# ---------------------------------------------------------------------------
# --help
# ---------------------------------------------------------------------------
cmd_help() {
  cat <<EOF
bench/netem.sh -- Phase 8 netem shaping and verification harness (design line 62).

usage: bench/netem.sh <subcommand> [options]

subcommands:
  netns-up                                    create the sympsica_r/sympsica_s veth pair
  netns-down                                   delete it
  clear                                        remove any qdisc + the applied-state file
  apply --profile P [--wrong-one-sided] [--wrong-rate-double]
  verify --profile P [--rtt-only] [--quick] [--structural]
  calibrate --profile P --date YYYY-MM-DD --env LOCAL|AWS [--rtt-only|--structural]
  show                                         dump current qdiscs + applied-state file

profiles: LAN WAN200 WAN50 WAN5

$FT12_SENTENCE
EOF
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
main() {
  if [ $# -eq 0 ]; then
    echo "usage: netem.sh <netns-up|netns-down|clear|apply|verify|calibrate|show> [options] | --help" >&2
    exit 3
  fi
  case "$1" in
    --help|-h) cmd_help; exit 0 ;;
  esac

  require_root
  require_netem_module

  local sub="$1"; shift
  case "$sub" in
    netns-up) cmd_netns_up "$@" ;;
    netns-down) cmd_netns_down "$@" ;;
    clear) cmd_clear "$@" ;;
    apply) cmd_apply "$@" ;;
    verify) cmd_verify "$@" ;;
    calibrate) cmd_calibrate "$@" ;;
    show) cmd_show "$@" ;;
    *) echo "netem.sh: unknown subcommand '$sub'" >&2; exit 3 ;;
  esac
}

# Sourceable (controller amendment, Task 34/A10): bms24/run.sh sources this
# file to reuse PROFILE_RTT_US/PROFILE_RATE_KBIT/LIMIT (ONE definition, never
# duplicated) for its own single-netns-loopback netem application. This guard
# changes nothing about `bash bench/netem.sh ...` CLI behaviour -- BASH_SOURCE[0]
# equals $0 only when the file is executed directly, never when sourced.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
