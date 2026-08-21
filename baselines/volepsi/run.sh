#!/usr/bin/env bash
# baselines/volepsi/run.sh -- Task 33 (W8.4, phase-8-plan.md § 33.1): the
# volePSI RR22 driver (Visa-Research/volepsi @ ec76012) wrapper. volePSI has
# no cardinality output of its own, so this row is labeled
# "RR22-CA(convention)": the receiver-side |PSI| count stands in for CA under
# BMS+24's own plain-PSI-as-CA convention footnote.
#
# FINDING (Task 33, first real smoke of this driver), controller ruling
# amendment A9: rr22_driver's coproto/macoro async socket runtime spawns 3 OS
# threads per party even with numThreads=1 passed to both RsPsiReceiver and
# RsPsiSender::init() -- library-internal thread count we do not control
# without patching pinned volePSI source (forbidden). thread_regime is
# therefore "runtime-helpers", not "strict" -- the one-core taskset pin still
# holds, so compute parallelism is 1, which is the substrate property this
# baseline actually claims. rr22_driver still accepts a cooperative
# `--start-gate PATH` (it blocks until that file exists) and this wrapper
# still passes one per side, since nothing about A9 changes the sampler's
# need for a clean first-sample race closure. Static-row overlap convention
# (pinned, plan § 33.1): u = |A(sym)B| split evenly, inter = n - u/2;
# `validate` enforces it.
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SELF="$REPO/baselines/volepsi/run.sh"
JSONL_CHECK="$REPO/bench/jsonl_check.py"
# shellcheck source=bench/lib.sh
source "$REPO/bench/lib.sh"

VOLEPSI_PINNED_SHA=ec76012ed516e25d3f460af9b8680e1140a5d491

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
# image label gate (33.1: run.sh asserts the built image's recorded source
# SHA label before trusting it): a mismatch is an environment failure -- the
# wrong image would silently execute against the wrong upstream revision.
# ---------------------------------------------------------------------------
check_image_label() {  # $1=image -> 0 ok, 4 + stderr message on mismatch
  local image=$1 got
  got=$(docker image inspect "$image" -f '{{index .Config.Labels "sympsica.source_sha"}}' 2>/dev/null)
  if [ "$got" != "$VOLEPSI_PINNED_SHA" ]; then
    echo "volepsi: image $image label sympsica.source_sha='$got' != pinned $VOLEPSI_PINNED_SHA" >&2
    return 4
  fi
  return 0
}

action_check_image() {
  local config=""
  while [ $# -gt 0 ]; do case "$1" in --config) config=$2; shift 2 ;; *) shift ;; esac; done
  local image="${IMAGE_OVERRIDE:-$(cfg_get "$config" image)}"
  check_image_label "$image" || exit 4
  echo "volepsi: check-image OK: $image"
}

# ---------------------------------------------------------------------------
# check-result --file F --expected N (also used internally by action_run)
# ---------------------------------------------------------------------------
volepsi_check_result() {  # $1=file $2=expected_count -> prints count, 0/5
  local file=$1 expected=$2 count
  count=$(sed -nE 's/^RESULT:count=([0-9]+)$/\1/p' "$file" 2>/dev/null | tail -1)
  if [ -z "$count" ] || [ "$count" -ne "$expected" ]; then
    echo "volepsi: RESULT count ${count:-0} != expected $expected" >&2
    return 5
  fi
  echo "$count"
  return 0
}

action_check_result() {
  local file="" expected=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --file) file=$2; shift 2 ;;
      --expected) expected=$2; shift 2 ;;
      *) shift ;;
    esac
  done
  local count
  count=$(volepsi_check_result "$file" "$expected") || exit 5
  echo "volepsi: check-result OK: count=$count"
}

# ---------------------------------------------------------------------------
# validate --config C (V6 subject: inter == n - u/2, the static-row convention)
# ---------------------------------------------------------------------------
action_validate() {
  local config=""
  while [ $# -gt 0 ]; do case "$1" in --config) config=$2; shift 2 ;; *) shift ;; esac; done
  run_config_check "$config"

  local n u inter want
  n=$(cfg_get "$config" n); u=$(cfg_get "$config" u); inter=$(cfg_get "$config" inter)
  want=$(( n - u / 2 ))
  if [ "$inter" -ne "$want" ]; then
    echo "volepsi: inter $inter != n - u/2 $want (static-cell convention)" >&2
    exit 3
  fi

  echo "volepsi: validate OK: $config"
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

  local n inter seed image
  n=$(cfg_get "$config" n)
  inter=$(cfg_get "$config" inter)
  seed=$(cfg_get "$config" seed)
  image=$(cfg_get "$config" image)

  check_image_label "$image" || exit 4

  phase total_workload_begin
  phase_zero setup_once
  phase_zero preprocessing

  # receiver listens on $ADDR_R:5002 (§ A); both sides address it there.
  launch_party r "$CONTAINER_R" rr22_driver /driver/build/rr22_driver \
    --role receiver --address "$ADDR_R:5002" --n "$n" --inter "$inter" --seed "$seed" \
    --start-gate /runwork/gate_r || exit $?

  wait_listen "$NS_R" 5002 40 || exit $?

  phase online_begin

  launch_party s "$CONTAINER_S" rr22_driver /driver/build/rr22_driver \
    --role sender --address "$ADDR_R:5002" --n "$n" --inter "$inter" --seed "$seed" \
    --start-gate /runwork/gate_s || exit $?

  wait "$WAIT_r"; local rc_r=$?
  wait "$WAIT_s"; local rc_s=$?

  phase online_end
  phase total_workload_end

  local count=""
  local tokens=("variant=RR22-CA(convention)" "ca-convention=plain-PSI-count-receiver-side(BMS+24-footnote)")
  if [ "$rc_r" -eq 0 ] && [ "$rc_s" -eq 0 ]; then
    if ! count=$(volepsi_check_result "$RUN_WORKDIR/party_r.stdout" "$inter"); then
      write_partial "$out" "$rc_r" "$rc_s" "result=na" "${tokens[@]}"
      exit 5
    fi
  fi

  write_partial "$out" "$rc_r" "$rc_s" "result=${count:-na}" "${tokens[@]}"

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
    check-result) shift; action_check_result "$@" ;;
    check-image) shift; action_check_image "$@" ;;
    *) action_public "$@" ;;
  esac
}

main "$@"
