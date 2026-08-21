#!/usr/bin/env bash
# baselines/fastupsi/run.sh -- Task 32 (W8.2, phase-8-plan.md § 32.4): the
# FastUPSI (qqqqyyy/FastUPSI @ 86ab38a) wrapper. FASTUPSI-IMAGE-EVIDENCE.md
# pins the two decisions this wrapper hard-codes: (1) `-LAN`/`-WAN` are NEVER
# passed -- the binary self-shapes container loopback via
# `./../network_setup.sh`, which conflicts with our external netns veth
# shaping; our config schema simply has no LAN/WAN key, so an attempt to add
# one is already rejected as an unknown key by `jsonl_check.py config`. (2)
# whole-run external shaping (one regime per record; racing tc against the
# authors' in-process barrier was rejected), token
# `shaping=whole-run(authors-shaped-online-only)`. Startup order follows the
# AUTHORS' own script.sh (connector-before-acceptor: party 0, sleep 0.3,
# party 1), which corrects the master plan's "simultaneously" -- the single,
# justified connector-first exception to the harness's listener-first
# boundary rule (§ C).
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SELF="$REPO/baselines/fastupsi/run.sh"
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

  local prot del protocol_avg n u days add_size del_size start_size
  prot=$(cfg_get "$config" prot)
  del=$(cfg_get "$config" del)
  protocol_avg=$(cfg_get "$config" protocol_avg)
  n=$(cfg_get "$config" n); u=$(cfg_get "$config" u)
  days=$(cfg_get "$config" days)
  add_size=$(cfg_get "$config" add_size)
  del_size=$(cfg_get "$config" del_size)
  start_size=$(cfg_get "$config" start_size)

  if [ "$prot" = "okvs" ] && [ "$del" = "true" ]; then
    echo "fastupsi: okvs does not support deletion (del=true) -- REFUSING" >&2
    exit 2
  fi

  # The FULL protocol_avg workload identity (round-5 I5): the recorded
  # (n,u,protocol_avg) cell must determine the executed workload exactly.
  case "$protocol_avg" in
    last8)
      [ "$prot" = "tree" ] || { echo "fastupsi: last8 requires prot==tree (got $prot)" >&2; exit 3; }
      [ "$del" = "false" ] || { echo "fastupsi: last8 requires del==false (got $del)" >&2; exit 3; }
      [ "$days" -eq 8 ] || { echo "fastupsi: last8 requires days==8 (got $days)" >&2; exit 3; }
      [ "$add_size" -eq "$u" ] || { echo "fastupsi: last8 requires add_size==u (got add_size=$add_size, u=$u)" >&2; exit 3; }
      [ "$del_size" -eq 0 ] || { echo "fastupsi: last8 requires del_size==0 (got $del_size)" >&2; exit 3; }
      local want=$(( n - 8*u - 1 ))
      [ "$start_size" -eq "$want" ] || { echo "fastupsi: last8 requires start_size == n-8*u-1 (got $start_size, want $want)" >&2; exit 3; }
      ;;
    amortizedNn)
      case "$prot" in
        okvs|cuckoo) ;;
        *) echo "fastupsi: amortizedNn requires prot in {okvs,cuckoo} (got $prot)" >&2; exit 3 ;;
      esac
      [ "$del" = "false" ] || { echo "fastupsi: amortizedNn requires del==false (got $del)" >&2; exit 3; }
      [ $(( n % u )) -eq 0 ] || { echo "fastupsi: amortizedNn requires n % u == 0 (got n=$n, u=$u)" >&2; exit 3; }
      local want_days=$(( n / u ))
      [ "$days" -eq "$want_days" ] || { echo "fastupsi: amortizedNn requires days == n/u (got days=$days, want $want_days)" >&2; exit 3; }
      [ "$add_size" -eq "$u" ] || { echo "fastupsi: amortizedNn requires add_size==u (got add_size=$add_size, u=$u)" >&2; exit 3; }
      [ "$del_size" -eq 0 ] || { echo "fastupsi: amortizedNn requires del_size==0 (got $del_size)" >&2; exit 3; }
      [ "$start_size" -eq 0 ] || { echo "fastupsi: amortizedNn requires start_size==0 (got $start_size)" >&2; exit 3; }
      ;;
    del8)
      case "$prot" in
        tree|cuckoo) ;;
        *) echo "fastupsi: del8 requires prot in {tree,cuckoo} (got $prot)" >&2; exit 3 ;;
      esac
      [ "$del" = "true" ] || { echo "fastupsi: del8 requires del==true (got $del)" >&2; exit 3; }
      [ "$days" -eq 8 ] || { echo "fastupsi: del8 requires days==8 (got $days)" >&2; exit 3; }
      [ "$add_size" -eq 0 ] || { echo "fastupsi: del8 requires add_size==0 (got $add_size)" >&2; exit 3; }
      [ "$del_size" -eq "$u" ] || { echo "fastupsi: del8 requires del_size==u (got del_size=$del_size, u=$u)" >&2; exit 3; }
      local want=$(( n - 1 ))
      [ "$start_size" -eq "$want" ] || { echo "fastupsi: del8 requires start_size == n-1 (got $start_size, want $want)" >&2; exit 3; }
      ;;
    *)
      echo "fastupsi: protocol_avg '$protocol_avg' not in {last8, amortizedNn, del8}" >&2
      exit 3
      ;;
  esac

  echo "fastupsi: validate OK: $config"
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

  local start_size add_size del_size days image
  start_size=$(cfg_get "$config" start_size)
  add_size=$(cfg_get "$config" add_size)
  del_size=$(cfg_get "$config" del_size)
  days=$(cfg_get "$config" days)
  image=$(cfg_get "$config" image)

  mkdir -p "$state_dir"
  # Absolute binary path; CWD /state so party0.txt/party1.txt land in the cache.
  docker run --rm --cpuset-cpus=2 -w /state -v "$state_dir:/state" "$image" \
    /home/fastupsi/build/frontend/setup -start_size "$start_size" -add_size "$add_size" -del_size "$del_size" -days "$days"
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

  local prot days del protocol_avg del_flag
  prot=$(cfg_get "$config" prot)
  days=$(cfg_get "$config" days)
  del=$(cfg_get "$config" del)
  protocol_avg=$(cfg_get "$config" protocol_avg)
  del_flag=""
  [ "$del" = "true" ] && del_flag="-del"

  phase total_workload_begin
  phase_zero setup_once
  phase_zero preprocessing

  # Connector-first exception (§ C): online_begin immediately before party 0
  # (the connector), then the authors' 0.3 s stagger, then party 1 (the
  # acceptor). asio's connector retries, which is what makes this order safe.
  phase online_begin

  launch_party r "$CONTAINER_R" main /home/fastupsi/build/frontend/main \
    -party 0 -prot "$prot" -days "$days" -ip "$ADDR_S:5001" $del_flag || exit $?
  sleep 0.3
  launch_party s "$CONTAINER_S" main /home/fastupsi/build/frontend/main \
    -party 1 -prot "$prot" -days "$days" -ip "$ADDR_S:5001" $del_flag || exit $?

  wait "$WAIT_r"; local rc_r=$?
  wait "$WAIT_s"; local rc_s=$?

  phase online_end
  phase total_workload_end

  write_partial "$out" "$rc_r" "$rc_s" \
    "protocol_avg=$protocol_avg" "online_includes_inproc_setup" \
    "shaping=whole-run(authors-shaped-online-only)" "result=na"

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
    check-result) shift; echo "baselines/fastupsi/run.sh: check-result not applicable to fastupsi" >&2; exit 3 ;;
    *) action_public "$@" ;;
  esac
}

main "$@"
