#!/usr/bin/env bash
# bench/selftest_wrapper.sh -- Phase 8 W8.7 (phase-8-plan.md § H, § 31-B.3):
# the harness's own self-test wrapper. Implements the uniform wrapper action
# contract (validate/prepare/run) plus the PUBLIC no-action form
# (.handoff/sympsica-plan.md:376). Native-only (exec_mode "native"): two
# python parties talk over the sympsica_r/sympsica_s veth pair on port 5020
# (§ A). TEST-ONLY env knobs (SELFTEST_*) are documented inline; none are
# used by any real baseline wrapper.
set -u

SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/selftest_wrapper.sh
REPO=$(dirname "$(dirname "$SELF")")
JSONL_CHECK="$REPO/bench/jsonl_check.py"
# shellcheck source=bench/lib.sh
source "$REPO/bench/lib.sh"
LISTENER_PY=/tmp/sympsica-bench/selftest_listener.py
CONNECTOR_PY=/tmp/sympsica-bench/selftest_connector.py

run_config_check() {  # $1 = config path; exit 3 on failure
  local err
  err=$(python3 "$JSONL_CHECK" config --file "$1" 2>&1 1>/dev/null)
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "$err" >&2
    exit 3
  fi
}

# ---------------------------------------------------------------------------
# validate --config C
# ---------------------------------------------------------------------------
action_validate() {
  local config=""
  while [ $# -gt 0 ]; do case "$1" in --config) config=$2; shift 2 ;; *) shift ;; esac; done
  run_config_check "$config"
  echo "selftest_wrapper: validate OK: $config"
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
  mkdir -p "$state_dir"
  echo 0 > "$state_dir/counter"
}

# ---------------------------------------------------------------------------
# the two pinned python parties, written fresh from heredocs EVERY run
# ---------------------------------------------------------------------------
write_selftest_pyfiles() {
  mkdir -p /tmp/sympsica-bench
  cat > "$LISTENER_PY" <<'PYEOF'
import os, socket, sys, threading, time

argv = sys.argv[1:]
gate = argv[argv.index("--start-gate") + 1] if "--start-gate" in argv else None

if os.environ.get("SELFTEST_TWO_THREADS") == "1":
    threading.Thread(target=time.sleep, args=(30,), daemon=True).start()

if gate is not None:
    deadline = time.time() + 30
    while not os.path.exists(gate):
        if time.time() > deadline:
            sys.stderr.write("start gate %s not created within 30 s\n" % gate)
            sys.exit(4)
        time.sleep(0.05)

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("10.99.0.2", 5020))
s.listen(1)
conn, _ = s.accept()

if os.environ.get("SELFTEST_FAIL_PARTY") == "1":
    sys.exit(3)

data = b""
while len(data) < 1024:
    chunk = conn.recv(1024 - len(data))
    if not chunk:
        break
    data += chunk

time.sleep(float(os.environ.get("SELFTEST_SLEEP_S", "1")))
sys.exit(0)
PYEOF

  cat > "$CONNECTOR_PY" <<'PYEOF'
import os, socket, sys, time

argv = sys.argv[1:]
gate = argv[argv.index("--start-gate") + 1] if "--start-gate" in argv else None

if gate is not None:
    deadline = time.time() + 30
    while not os.path.exists(gate):
        if time.time() > deadline:
            sys.stderr.write("start gate %s not created within 30 s\n" % gate)
            sys.exit(4)
        time.sleep(0.05)

conn = None
for _ in range(50):
    try:
        conn = socket.create_connection(("10.99.0.2", 5020), timeout=1)
        break
    except OSError:
        time.sleep(0.1)
if conn is None:
    sys.stderr.write("selftest_connector: could not connect to 10.99.0.2:5020\n")
    sys.exit(5)

try:
    conn.sendall(b"\x00" * 1024)
except OSError:
    pass
sys.exit(0)
PYEOF
}

write_partial() {  # $1=out_path $2=rc_r $3=rc_s
  python3 - "$1" "$2" "$3" <<'PYEOF'
import json, sys
path, rc_r, rc_s = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(path, "w") as f:
    json.dump({"notes_tokens": ["result=na"], "party_exit": {"r": rc_r, "s": rc_s}}, f)
PYEOF
}

wait_for_comm() {  # $1 pid $2 expected comm -> 0/1, retry <=50x0.1s
  local pid=$1 expected=$2 i
  for i in $(seq 1 50); do
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "$expected" ] && return 0
    sleep 0.1
  done
  return 1
}

# ---------------------------------------------------------------------------
# run --config C --out P --workdir W
# ---------------------------------------------------------------------------
action_run() {
  [ "${SYMPSICA_MEASURE:-}" = "1" ] || { echo "run.sh must execute inside bench/measure.sh" >&2; exit 4; }
  : "${NS_R:?}" "${NS_S:?}" "${CORE_R:?}" "${CORE_S:?}" "${STATE_WORK:?}" "${RUN_WORKDIR:?}"

  local config="" out="" workdir=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --config) config=$2; shift 2 ;;
      --out) out=$2; shift 2 ;;
      --workdir) workdir=$2; shift 2 ;;
      *) shift ;;
    esac
  done

  local counter_file="$STATE_WORK/counter"
  local counter
  counter=$(cat "$counter_file" 2>/dev/null || echo "")

  if [ "$counter" != "0" ]; then
    echo "selftest: state not pristine (counter=$counter) -- snapshot/restore broken" >&2
    write_partial "$out" 0 0
    exit 5
  fi

  write_selftest_pyfiles

  phase total_workload_begin
  phase_zero setup_once
  phase_zero preprocessing

  local skip_pids="${SELFTEST_SKIP_PIDS:-0}"

  # party s (listener) -- launches first (§ C listener-first boundary rule)
  local listener_args=("$LISTENER_PY")
  local gate_s="$RUN_WORKDIR/gate_s"
  [ "$skip_pids" != "1" ] && listener_args+=(--start-gate "$gate_s")
  ip netns exec "$NS_S" taskset -c "$CORE_S" python3 "${listener_args[@]}" \
    >"$RUN_WORKDIR/party_s.stdout" 2>"$RUN_WORKDIR/party_s.stderr" &
  local pid_s=$!
  if [ "$skip_pids" != "1" ]; then
    wait_for_comm "$pid_s" python3
    echo "s=$pid_s" >> "$RUN_WORKDIR/pids"
  fi

  wait_listen "$NS_S" 5020 40

  phase online_begin

  # party r (connector)
  local connector_args=("$CONNECTOR_PY")
  local gate_r="$RUN_WORKDIR/gate_r"
  [ "$skip_pids" != "1" ] && connector_args+=(--start-gate "$gate_r")
  ip netns exec "$NS_R" taskset -c "$CORE_R" python3 "${connector_args[@]}" \
    >"$RUN_WORKDIR/party_r.stdout" 2>"$RUN_WORKDIR/party_r.stderr" &
  local pid_r=$!
  if [ "$skip_pids" != "1" ]; then
    wait_for_comm "$pid_r" python3
    echo "r=$pid_r" >> "$RUN_WORKDIR/pids"
  fi

  wait "$pid_r"; local rc_r=$?
  wait "$pid_s"; local rc_s=$?

  phase online_end
  phase total_workload_end

  if [ "$rc_r" -eq 0 ] && [ "$rc_s" -eq 0 ]; then
    echo 1 > "$counter_file"
  fi
  write_partial "$out" "$rc_r" "$rc_s"

  if [ "$rc_r" -eq 0 ] && [ "$rc_s" -eq 0 ]; then
    exit 0
  fi
  exit 5
}

# ---------------------------------------------------------------------------
# PUBLIC no-action form: run.sh --config C --out O [--env E] [--trials N] [--warmup K]
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
      *) echo "selftest_wrapper.sh: unknown argument $1" >&2; exit 3 ;;
    esac
  done
  [ -n "$config" ] && [ -n "$out" ] || { echo "selftest_wrapper.sh: --config and --out are required" >&2; exit 3; }

  local state_dir
  state_dir=$(python3 "$JSONL_CHECK" config --file "$config" --get state_dir)
  local key
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
    check-result) shift; echo "selftest_wrapper.sh: check-result not applicable to selftest" >&2; exit 3 ;;
    *) action_public "$@" ;;
  esac
}

main "$@"
