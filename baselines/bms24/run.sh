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
# veth path. That same generic step also sends 20 real ICMP pings on the
# veth pair to measure base RTT (regardless of protocol), which is why
# jsonl_check.py's combined-loopback byte check tolerates a small
# VETH_CALIBRATION_NOISE_MAX_B ceiling rather than requiring the veth deltas
# to be exactly zero (see that file for the measured magnitude).
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
}

bms_lo_measure_base_us() {  # $1=ping count -> integer microseconds (real loopback RTT)
  local n=$1 avg_ms
  avg_ms=$(ip netns exec "$BMS_LO_NS" ping -c "$n" -i 0.2 -q 127.0.0.1 2>/dev/null \
           | sed -n 's#^rtt min/avg/max/mdev = [0-9.]*/\([0-9.]*\)/.*#\1#p')
  python3 -c 'import sys; print(int(round(float(sys.argv[1]) * 1000)))' "$avg_ms"
}

bms_lo_apply_netem() {  # $1=profile (LAN|WAN200|WAN50|WAN5) -- shapes `lo`, not veth
  local profile=$1 base_us delay_us
  ip netns exec "$BMS_LO_NS" tc qdisc del dev lo root 2>/dev/null || true
  base_us=$(bms_lo_measure_base_us 20)
  delay_us=$(( (${PROFILE_RTT_US[$profile]} - base_us) / 2 ))
  [ "$delay_us" -lt 0 ] && delay_us=0
  ip netns exec "$BMS_LO_NS" tc qdisc replace dev lo root netem delay ${delay_us}us rate ${PROFILE_RATE_KBIT[$profile]}kbit limit "$LIMIT"
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
  bms_lo_up "$image" "$mem"
  bms_lo_apply_netem "$network"

  phase total_workload_begin
  phase_zero setup_once
  phase_zero preprocessing

  # Combined byte accounting (A10 clause 2/3): measured from the ONE shared
  # `lo`'s tx delta, taken AFTER bms_lo_up/bms_lo_apply_netem's OWN calibration
  # pings (which must not be counted) and around the real protocol traffic.
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
  # "[PartyZero] CARDINALITY = <int>" (addition), plus (deletion only)
  # "Total Comm Sent(B):\t<int>".
  local result=na internal_b=""
  if [ -f "$RUN_WORKDIR/party_r.stdout" ]; then
    local m
    m=$(sed -nE 's/^\[PartyZero\] (CA\/SUM|CARDINALITY) = ([0-9]+)$/\2/p' "$RUN_WORKDIR/party_r.stdout" | tail -1)
    [ -n "$m" ] && result=$m
    internal_b=$(sed -nE 's/^Total Comm Sent\(B\):[[:space:]]*([0-9]+)$/\1/p' "$RUN_WORKDIR/party_r.stdout" | tail -1)
  fi

  local tokens=("result=$result" "bytes=combined-loopback" "combined_total_b=$combined_total")
  [ -n "$internal_b" ] && tokens+=("internal_sent_b_r=$internal_b")
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

main "$@"
