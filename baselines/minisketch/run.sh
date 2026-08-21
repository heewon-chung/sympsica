#!/usr/bin/env bash
# baselines/minisketch/run.sh -- Task 33 (W8.5, phase-8-plan.md § 33.2): the
# minisketch (bitcoin-core/minisketch @ 4a179c6) two-party symmetric-difference
# exchange wrapper. INADMISSIBLE-labeled reference row: a non-private sketch
# exchange, not a PSI protocol -- every record carries
# `inadmissible=non-private-sketch-reference`. `exchange` is a strict-
# thread-regime binary WE author (A2(iii)/A5): it accepts a cooperative
# `--start-gate PATH` and blocks until that file exists, so this wrapper
# always passes one per side. Static-cell convention (pinned, plan § 33.1/2):
# capacity == u and 2*(n-inter) == u -- `validate` enforces both.
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SELF="$REPO/baselines/minisketch/run.sh"
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
  echo "$side=$p" >> "$RUN_WORKDIR/pids"   # the sampler samples it, then creates gate_$side (A5)
}

# ---------------------------------------------------------------------------
# check-result --file F --expected-symdiff K --expected-bytes B (also used
# internally by action_run)
# ---------------------------------------------------------------------------
minisketch_check_result() {  # $1=file $2=exp_symdiff $3=exp_bytes -> prints "symdiff bytes", 0/5
  local file=$1 exp_symdiff=$2 exp_bytes=$3 symdiff bytes
  symdiff=$(sed -nE 's/^RESULT:symdiff=([0-9]+)$/\1/p' "$file" 2>/dev/null | tail -1)
  bytes=$(sed -nE 's/^RESULT:bytes=([0-9]+)$/\1/p' "$file" 2>/dev/null | tail -1)
  if [ -z "$symdiff" ] || [ "$symdiff" -ne "$exp_symdiff" ]; then
    echo "minisketch: symdiff ${symdiff:-0} != expected $exp_symdiff" >&2
    return 5
  fi
  if [ -z "$bytes" ] || [ "$bytes" -ne "$exp_bytes" ]; then
    echo "minisketch: bytes ${bytes:-0} != 8*capacity $exp_bytes" >&2
    return 5
  fi
  echo "$symdiff $bytes"
  return 0
}

action_check_result() {
  local file="" exp_symdiff="" exp_bytes=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --file) file=$2; shift 2 ;;
      --expected-symdiff) exp_symdiff=$2; shift 2 ;;
      --expected-bytes) exp_bytes=$2; shift 2 ;;
      *) shift ;;
    esac
  done
  local res
  res=$(minisketch_check_result "$file" "$exp_symdiff" "$exp_bytes") || exit 5
  echo "minisketch: check-result OK: symdiff=${res%% *} bytes=${res##* }"
}

# ---------------------------------------------------------------------------
# validate --config C (V7/V8 subjects: capacity == u; 2*(n-inter) == u)
# ---------------------------------------------------------------------------
action_validate() {
  local config=""
  while [ $# -gt 0 ]; do case "$1" in --config) config=$2; shift 2 ;; *) shift ;; esac; done
  run_config_check "$config"

  local n u inter capacity k
  n=$(cfg_get "$config" n); u=$(cfg_get "$config" u)
  inter=$(cfg_get "$config" inter); capacity=$(cfg_get "$config" capacity)

  if [ "$capacity" -ne "$u" ]; then
    echo "minisketch: capacity $capacity != u $u (W8.5 pins capacity=u)" >&2
    exit 3
  fi

  k=$(( 2 * (n - inter) ))
  if [ "$k" -ne "$u" ]; then
    echo "minisketch: 2*(n-inter) $k != u $u (static-cell convention)" >&2
    exit 3
  fi

  echo "minisketch: validate OK: $config"
}

# ---------------------------------------------------------------------------
# prepare --config C --state-dir D (no cache to build; touch the sentinel)
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
  mkdir -p "$state_dir"
  touch "$state_dir/.cache-ok"
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

  local n inter capacity
  n=$(cfg_get "$config" n)
  inter=$(cfg_get "$config" inter)
  capacity=$(cfg_get "$config" capacity)

  phase total_workload_begin
  phase_zero setup_once
  phase_zero preprocessing

  # bob (receiver) binds $ADDR_R:5003 (§ A); alice (sender) connects there.
  launch_party r "$CONTAINER_R" exchange /driver/exchange \
    --role bob --address "$ADDR_R" --port 5003 --n "$n" --inter "$inter" --capacity "$capacity" \
    --start-gate /runwork/gate_r || exit $?

  wait_listen "$NS_R" 5003 40 || exit $?

  phase online_begin

  launch_party s "$CONTAINER_S" exchange /driver/exchange \
    --role alice --address "$ADDR_R" --port 5003 --n "$n" --inter "$inter" --capacity "$capacity" \
    --start-gate /runwork/gate_s || exit $?

  wait "$WAIT_r"; local rc_r=$?
  wait "$WAIT_s"; local rc_s=$?

  phase online_end
  phase total_workload_end

  local exp_symdiff=$(( 2 * (n - inter) )) exp_bytes=$(( 8 * capacity ))
  local tokens=("variant=minisketch-symdiff" "inadmissible=non-private-sketch-reference")
  local res symdiff bytes ca

  if [ "$rc_r" -eq 0 ] && [ "$rc_s" -eq 0 ]; then
    if res=$(minisketch_check_result "$RUN_WORKDIR/party_r.stdout" "$exp_symdiff" "$exp_bytes"); then
      symdiff=${res%% *}; bytes=${res##* }
      ca=$(( (2 * n - symdiff) / 2 ))
      write_partial "$out" "$rc_r" "$rc_s" "symdiff=$symdiff" "ca_from_symdiff=$ca" "result=$ca" "${tokens[@]}"
      exit 0
    fi
    write_partial "$out" "$rc_r" "$rc_s" "result=na" "${tokens[@]}"
    exit 5
  fi

  write_partial "$out" "$rc_r" "$rc_s" "result=na" "${tokens[@]}"
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
    check-result) shift; action_check_result "$@" ;;
    *) action_public "$@" ;;
  esac
}

main "$@"
