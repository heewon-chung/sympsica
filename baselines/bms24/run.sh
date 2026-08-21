#!/usr/bin/env bash
# baselines/bms24/run.sh -- Task 32 (W8.1, phase-8-plan.md § 32.2): the BMS+24
# (ruidazeng/upsi-revisited @ d6ddfb5) wrapper. Encodes exactly the flag
# contracts pinned by BMS24-IMAGE-EVIDENCE.md: `--func=CA` on all four
# invocations (both `run` binaries default to SUM); the stored-trees flag is
# spelled `--trees` on addition / `--import` on deletion; the addition
# binaries have NO delete concept at all, so the TV-F17 refusal below is
# entirely this wrapper's job, never the binary's. GC channel (deletion only)
# is DNAT'd in $NS_R -- `--gc_IP` is defined upstream but dead (party 0
# hardcodes 127.0.0.1), so it is never passed.
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SELF="$REPO/baselines/bms24/run.sh"
JSONL_CHECK="$REPO/bench/jsonl_check.py"
# shellcheck source=bench/lib.sh
source "$REPO/bench/lib.sh"

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

  local variant days delete_mode bin port trees_flag gc_flag
  variant=$(cfg_get "$config" variant)
  days=$(cfg_get "$config" days)
  delete_mode=$(cfg_get "$config" delete_mode)

  case "$variant" in
    add-only) bin=upsi/addition; port=10501; trees_flag=--trees; gc_flag="" ;;
    add-del)  bin=upsi/deletion; port=1026;  trees_flag=--import; gc_flag=--gc_port=1025 ;;
    *) echo "bms24 run: variant '$variant' not in {add-only, add-del}" >&2; exit 3 ;;
  esac

  phase total_workload_begin
  phase_zero setup_once
  phase_zero preprocessing

  if [ "$variant" = "add-del" ]; then
    # GC channel: party 0 (r) hardcodes 127.0.0.1:1025 -- DNAT in $NS_R to $ADDR_S:1025.
    ip netns exec "$NS_R" sysctl -qw net.ipv4.conf.all.route_localnet=1
    ip netns exec "$NS_R" iptables -t nat -A OUTPUT -d 127.0.0.1/32 -p tcp --dport 1025 -j DNAT --to-destination "$ADDR_S:1025"
    ip netns exec "$NS_R" iptables -t nat -A POSTROUTING -o veth_r -j MASQUERADE
  fi

  launch_party s "$CONTAINER_S" run bash -lc \
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
  if [ "$variant" = "add-del" ]; then
    wait_listen "$NS_S" 1025 600 || exit $?
  else
    wait_listen "$NS_S" "$port" 600 || exit $?
  fi

  # W8.1 pinned 3 s stagger: max(readiness, 3 s) since party-1 launch.
  while [ $(( $(mono_ns) - t1 )) -lt 3000000000 ]; do sleep 0.1; done

  phase online_begin

  launch_party r "$CONTAINER_R" run bash -lc \
    "cd /home/upsi-user && ./bazel-bin/$bin/run --party=0 --port=$ADDR_S:$port $gc_flag --days=$days --func=CA $trees_flag --data_dir=/state/data/ --out_dir=/state/out/" \
    || exit $?

  wait "$WAIT_r"; local rc_r=$?
  wait "$WAIT_s"; local rc_s=$?

  phase online_end
  phase total_workload_end

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

  local tokens=("result=$result")
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
