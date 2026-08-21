#!/usr/bin/env bash
# baselines/kunlun/run.sh -- STUB (R8-KUNLUN): UNVALIDATED BY CONSTRUCTION --
# image not built, wrapper not smoked. Task 32 (W8.3-stub, phase-8-plan.md
# § 32.5): authored from source reads of yuchen1024/Kunlun @ 395cf8e and its
# README only, never an executed build. The controller deliberately did NOT
# probe Kunlun's upstream or build anything -- this wrapper MUST NOT be built,
# run, or cited as working; CZZ+24 stays a derived-labeled row (see
# baselines/acns2147/). Untested assumption (marked inline below): `prepare`
# feeds the receiver role to generate the pp/testcase pair, then the pinned
# `timeout` unblocks it before it accepts a connection, leaving the files
# behind for `run` to reuse.
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SELF="$REPO/baselines/kunlun/run.sh"
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
# validate --config C
# ---------------------------------------------------------------------------
action_validate() {
  local config=""
  while [ $# -gt 0 ]; do case "$1" in --config) config=$2; shift 2 ;; *) shift ;; esac; done
  run_config_check "$config"

  local log_item_num
  log_item_num=$(cfg_get "$config" log_item_num)
  if [ "$log_item_num" -lt 10 ] || [ "$log_item_num" -gt 24 ]; then
    echo "kunlun: log_item_num must be in 10..24 (got $log_item_num)" >&2
    exit 3
  fi

  echo "kunlun: validate OK: $config (STUB -- R8-KUNLUN, UNVALIDATED BY CONSTRUCTION)"
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

  local image
  image=$(cfg_get "$config" image)
  mkdir -p "$state_dir"
  # Precautionary, not upstream-probed knowledge (R8-KUNLUN still holds):
  # measure.sh always runs as root, so this directory is created root:root;
  # Task 32's bms24 wrapper hit a real "Permission denied" from an
  # unprivileged image user writing into a root-owned bind mount. Kunlun's
  # image user was never inspected (forbidden by R8-KUNLUN), so this applies
  # the same general fix defensively rather than leaving a known failure
  # mode unaddressed in an authored-but-unvalidated artifact.
  chmod -R a+rwx "$state_dir"
  # Untested assumption (R8-KUNLUN): feeding "receiver" generates the pp +
  # testcase pair and then blocks on accept; the pinned `timeout 600` ends
  # the process while leaving the generated files in place.
  docker run --rm -w /state -v "$state_dir:/state" "$image" bash -lc \
    'rm -f *.pp *.testcase; printf "receiver\n" | timeout 600 /kunlun/build/test_mqrpmt_psi_card >/dev/null 2>&1 || true'
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

  phase total_workload_begin
  phase_zero setup_once
  phase_zero preprocessing

  # Receiver first: binds "":8080 in sympsica_r.
  launch_party r "$CONTAINER_R" test_mqrpmt_psi bash -lc \
    'echo receiver | /kunlun/build/test_mqrpmt_psi_card' || exit $?

  wait_listen "$NS_R" 8080 60 || exit $?

  # Sender hardcodes 127.0.0.1:8080 -- DNAT in $NS_S to $ADDR_R:8080.
  ip netns exec "$NS_S" sysctl -qw net.ipv4.conf.all.route_localnet=1
  ip netns exec "$NS_S" iptables -t nat -A OUTPUT -d 127.0.0.1/32 -p tcp --dport 8080 -j DNAT --to-destination "$ADDR_R:8080"
  ip netns exec "$NS_S" iptables -t nat -A POSTROUTING -o veth_s -j MASQUERADE

  phase online_begin

  # Untested assumption (R8-KUNLUN): the sender-side stdin role prompt is
  # inferred as "sender" by symmetry with the "receiver" prompt used in
  # `prepare`/above -- never confirmed against the real binary.
  launch_party s "$CONTAINER_S" test_mqrpmt_psi bash -lc \
    'echo sender | /kunlun/build/test_mqrpmt_psi_card' || exit $?

  wait "$WAIT_r"; local rc_r=$?
  wait "$WAIT_s"; local rc_s=$?

  phase online_end
  phase total_workload_end

  local result=na
  if [ -f "$RUN_WORKDIR/party_s.stdout" ]; then
    local m
    m=$(sed -nE 's/^Intersection cardinality \(test\) = ([0-9]+)$/\1/p' "$RUN_WORKDIR/party_s.stdout" | tail -1)
    [ -n "$m" ] && result=$m
  fi

  write_partial "$out" "$rc_r" "$rc_s" "result=$result"

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
    check-result) shift; echo "baselines/kunlun/run.sh: check-result not applicable to kunlun" >&2; exit 3 ;;
    *) action_public "$@" ;;
  esac
}

main "$@"
