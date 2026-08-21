#!/usr/bin/env bash
# bench/measure.sh -- Phase 8 W8.7 (phase-8-plan.md § H "measure.sh <-> run.sh
# division of labor"; master plan W8.7, design line 62): the measurement
# driver. Owns the lock, config validation, netem sentinel/state-lifecycle
# gates, the per-trial netns/docker setup, the A2+A5+A6 thread sampler, byte
# counting, PHASE-marker segment parsing, and JSONL emission via
# bench/jsonl_check.py. Every baseline `run.sh` and bench/selftest_wrapper.sh
# is driven through this file; it never decides thread/emit policy itself
# (that lives in jsonl_check.py, pinned).
#
# All flags whose long name starts with --wrong- are TEST-ONLY.
# Exit codes (Global constraint 11): 0 ok; 2 refusal/verification failure;
# 3 usage/config error; 4 environment missing; 5 result mismatch/party
# failure; 124 budget/watchdog timeout.
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(dirname "$SCRIPT_DIR")
# shellcheck source=bench/lib.sh
source "$SCRIPT_DIR/lib.sh"

SELF="$SCRIPT_DIR/measure.sh"
NETEM="$SCRIPT_DIR/netem.sh"
JSONL_CHECK="$SCRIPT_DIR/jsonl_check.py"
LOCK_FILE=/tmp/sympsica-bench.lock

require_root() {
  [ "$(id -u)" = "0" ] || { echo "measure.sh must run as root" >&2; exit 4; }
}

cfg() { python3 "$JSONL_CHECK" config --file "$1" --get "$2"; }

run_config_check() {  # $1 = config path; exit 3 on failure (jsonl_check itself returns 2)
  local err
  err=$(python3 "$JSONL_CHECK" config --file "$1" 2>&1 1>/dev/null)
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "$err" >&2
    exit 3
  fi
}

sentinel_check() {  # $1=sentinel_path $2=env $3=network -> exit 4 with message on failure
  python3 - "$1" "$2" "$3" <<'PYEOF'
import json, sys
path, env, network = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path) as f:
    d = json.load(f)
mode = d.get("mode")
if env == "LOCAL" and network == "LAN":
    required = {"structural"}
elif env == "LOCAL":
    required = {"rtt-only", "full"}
else:
    required = {"full"}
if mode not in required:
    sys.stderr.write("netem sentinel mode '%s' for %s (env %s) does not satisfy required %s\n"
                      % (mode, network, env, "|".join(sorted(required))))
    sys.exit(4)
PYEOF
}

init_threads_json() {  # $1 = workdir -- step 9(a): both sides unsampled, tmp+rename
  python3 - "$1/threads.json" <<'PYEOF'
import json, os, sys
path = sys.argv[1]
obj = {"state": "unsampled", "threads_max": None, "rss_kb": None}
data = {"r": dict(obj), "s": dict(obj)}
tmp = path + ".tmp"
with open(tmp, "w") as f:
    json.dump(data, f)
os.replace(tmp, path)
PYEOF
}

# ---------------------------------------------------------------------------
# A2+A5+A6 sampler (§ E mechanics): backgrounded; TERM-trapped; its ONLY
# output is WORKDIR/threads.json (tmp+rename every iteration); it never
# kills anything and never exits on its own.
# ---------------------------------------------------------------------------
run_sampler() {
  local workdir=$1
  declare -A state=([r]=unsampled [s]=unsampled)
  declare -A tmax=()
  declare -A rss=()
  declare -A seen_pid=()

  write_threads_json() {
    python3 - "$workdir/threads.json" \
      "${state[r]}" "${tmax[r]:-}" "${rss[r]:-}" \
      "${state[s]}" "${tmax[s]:-}" "${rss[s]:-}" <<'PYEOF'
import json, os, sys
path = sys.argv[1]


def side(st, tm, rk):
    return {"state": st,
            "threads_max": int(tm) if (st == "observed" and tm != "") else None,
            "rss_kb": int(rk) if rk != "" else None}


data = {"r": side(sys.argv[2], sys.argv[3], sys.argv[4]),
        "s": side(sys.argv[5], sys.argv[6], sys.argv[7])}
tmp = path + ".tmp"
with open(tmp, "w") as f:
    json.dump(data, f)
os.replace(tmp, path)
PYEOF
  }

  finalize() { write_threads_json; }
  trap 'finalize; exit 0' TERM

  write_threads_json  # (a) -- redundant with the parent's own init, harmless

  while :; do
    if [ -f "$workdir/pids" ]; then
      while IFS='=' read -r side pid; do
        [ -n "${side:-}" ] || continue
        [ -n "${seen_pid[$side]:-}" ] || seen_pid[$side]=$pid
      done < "$workdir/pids"
    fi
    local side pid th rk
    for side in r s; do
      pid=${seen_pid[$side]:-}
      [ -n "$pid" ] || continue
      [ -r "/proc/$pid/status" ] || continue
      th=$(sed -n 's/^Threads:[[:space:]]\+\([0-9]\+\)$/\1/p' "/proc/$pid/status" 2>/dev/null)
      rk=$(sed -n 's/^VmHWM:[[:space:]]\+\([0-9]\+\)[[:space:]]kB$/\1/p' "/proc/$pid/status" 2>/dev/null)
      if [ -n "$th" ]; then
        if [ "${state[$side]}" != "observed" ]; then
          state[$side]=observed
          touch "$workdir/gate_$side"
        fi
        if [ -z "${tmax[$side]:-}" ] || [ "$th" -gt "${tmax[$side]}" ]; then
          tmax[$side]=$th
        fi
        [ -n "$rk" ] && rss[$side]=$rk
      fi
    done
    write_threads_json
    sleep 0.25
  done
}

# ---------------------------------------------------------------------------
# § I docker wiring (pinned; exercised by Task 32/33 baselines, not by any
# Task-31 live row -- selftest and HSTx1 are native-only).
# ---------------------------------------------------------------------------
wire_docker_netns() {  # $1 container $2 netns-alias
  local ctr=$1 alias=$2 pid
  pid=$(docker inspect -f '{{.State.Pid}}' "$ctr")
  mkdir -p /var/run/netns
  ln -sfT "/proc/$pid/ns/net" "/var/run/netns/$alias"
}

# ---------------------------------------------------------------------------
# EXIT trap: released once per `measure.sh run` / `selftest-bytes` process.
# ---------------------------------------------------------------------------
CUR_EXEC_MODE=""
on_run_exit() {
  docker rm -f sympsica_pr sympsica_ps >/dev/null 2>&1 || true
  rm -f /var/run/netns/sympsica_r /var/run/netns/sympsica_s 2>/dev/null || true
  bash "$NETEM" netns-down >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# prepare --wrapper W --config C --env LOCAL|AWS [--budget-s 7200]
# ---------------------------------------------------------------------------
cmd_prepare() {
  local wrapper="" config="" env="" budget_s=7200
  while [ $# -gt 0 ]; do
    case "$1" in
      --wrapper) wrapper=$2; shift 2 ;;
      --config) config=$2; shift 2 ;;
      --env) env=$2; shift 2 ;;
      --budget-s) budget_s=$2; shift 2 ;;
      *) echo "measure.sh prepare: unknown argument $1" >&2; exit 3 ;;
    esac
  done
  [ -n "$wrapper" ] && [ -n "$config" ] || { echo "measure.sh prepare: --wrapper and --config are required" >&2; exit 3; }
  case "$env" in LOCAL|AWS) ;; *) echo "measure.sh prepare: --env must be LOCAL or AWS" >&2; exit 3 ;; esac

  run_config_check "$config"
  local key d
  key=$(python3 "$JSONL_CHECK" config --file "$config" --prepare-key)
  d=$(cfg "$config" state_dir)

  if [ -f "$d/.prepared" ]; then
    local existing
    existing=$(cat "$d/.prepared")
    if [ "$existing" = "$key" ]; then
      local last_env
      last_env=$(python3 -c '
import json, sys
lines = [l for l in open(sys.argv[1]) if l.strip()]
print(json.loads(lines[-1])["env"] if lines else "")' "$d/setup.jsonl" 2>/dev/null)
      if [ "$last_env" != "$env" ]; then
        echo "cache prepared for env $last_env" >&2
        exit 3
      fi
      echo "already prepared"
      exit 0
    else
      echo "state_dir prepared under a different config" >&2
      exit 3
    fi
  fi

  mkdir -p "$d"
  local t0 t1 rc
  t0=$(mono_ns)
  timeout --signal=KILL "$budget_s" bash "$wrapper" prepare --config "$config" --state-dir "$d"
  rc=$?
  t1=$(mono_ns)
  if [ "$rc" -eq 124 ]; then
    echo "prepare exceeded $budget_s s -- STOP and escalate (risk register)" >&2
    exit 124
  fi
  [ "$rc" -eq 0 ] || exit 5

  local protocol variant n u wall
  protocol=$(cfg "$config" protocol); variant=$(cfg "$config" variant)
  n=$(cfg "$config" n); u=$(cfg "$config" u)
  wall=$(python3 -c "print(($t1 - $t0) / 1e9)")
  python3 - "$d/setup.jsonl" "$env" "$protocol" "$variant" "$n" "$u" "$wall" "$key" <<'PYEOF'
import datetime, json, sys
path, env, protocol, variant, n, u, wall, key = sys.argv[1:9]
rec = {"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
       "env": env, "protocol": protocol, "variant": variant, "n": int(n), "u": int(u),
       "setup_wall_s": float(wall), "prepare_key": key, "status": "ok"}
with open(path, "a") as f:
    f.write(json.dumps(rec) + "\n")
PYEOF
  printf '%s' "$key" > "$d/.prepared"
}

# ---------------------------------------------------------------------------
# run --wrapper W --config C --out OUT --env LOCAL|AWS [--trials N] [--warmup K]
#     [--wrong-no-restore]
# ---------------------------------------------------------------------------
cmd_run() {
  local wrapper="" config="" out="" env="" trials=1 warmup="" wrong_no_restore=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --wrapper) wrapper=$2; shift 2 ;;
      --config) config=$2; shift 2 ;;
      --out) out=$2; shift 2 ;;
      --env) env=$2; shift 2 ;;
      --trials) trials=$2; shift 2 ;;
      --warmup) warmup=$2; shift 2 ;;
      --wrong-no-restore) wrong_no_restore=1; shift ;;
      *) echo "measure.sh run: unknown argument $1" >&2; exit 3 ;;
    esac
  done
  [ -n "$wrapper" ] && [ -n "$config" ] && [ -n "$out" ] && [ -n "$env" ] \
    || { echo "measure.sh run: --wrapper, --config, --out and --env are required" >&2; exit 3; }

  # step 1
  exec 9>"$LOCK_FILE"
  flock -n 9 || { echo "measure.sh: lock $LOCK_FILE held by another run" >&2; exit 4; }
  trap on_run_exit EXIT

  # step 2
  run_config_check "$config"
  local key
  key=$(python3 "$JSONL_CHECK" config --file "$config" --prepare-key)
  local network exec_mode image state_dir budget_s
  network=$(cfg "$config" network); exec_mode=$(cfg "$config" exec_mode)
  image=$(cfg "$config" image); state_dir=$(cfg "$config" state_dir)
  budget_s=$(cfg "$config" budget_s)
  [ -n "$warmup" ] || warmup=$(cfg "$config" warmup)
  [ -n "$warmup" ] || warmup=0

  # step 3
  mkdir -p "$(dirname "$out")" /tmp/sympsica-bench/logs /tmp/sympsica-bench/runs /tmp/sympsica-bench/calib

  # step 4: sentinel
  local boot_id sentinel
  boot_id=$(cat /proc/sys/kernel/random/boot_id)
  sentinel="/tmp/sympsica-netem-ok-${network}-${boot_id}"
  [ -f "$sentinel" ] || { echo "netem sentinel absent for $network on boot $boot_id: run bench/netem.sh calibrate first" >&2; exit 4; }
  sentinel_check "$sentinel" "$env" "$network" || exit 4

  # step 5: state
  [ -f "$state_dir/.prepared" ] || { echo "run measure.sh prepare first" >&2; exit 4; }
  local prepared_key
  prepared_key=$(cat "$state_dir/.prepared")
  [ "$prepared_key" = "$key" ] || { echo "run measure.sh prepare first" >&2; exit 4; }

  local snapshot="${state_dir}.snapshot"
  if [ ! -d "$snapshot" ]; then
    cp -a "$state_dir" "$snapshot"
  else
    local snap_key
    snap_key=$(cat "$snapshot/.prepared" 2>/dev/null || echo "")
    [ "$snap_key" = "$key" ] || { echo "state_dir prepared under a different config (snapshot key mismatch)" >&2; exit 3; }
  fi

  # step 6
  local runid workroot
  runid="$(date -u +%Y%m%dT%H%M%SZ)-$$"
  workroot="/tmp/sympsica-bench/runs/$runid"
  mkdir -p "$workroot"

  local ok_count=0 timeout_count=0 error_count=0
  local total_iters=$(( warmup + trials ))
  local t
  for (( t = 0; t < total_iters; t++ )); do
    local is_warmup=0
    [ "$t" -lt "$warmup" ] && is_warmup=1
    local trial_idx=$(( t - warmup ))
    local emit_trial=$trial_idx
    [ "$is_warmup" = 1 ] && emit_trial=$t

    local workdir="$workroot/trial$t"
    mkdir -p "$workdir"

    # step 7: restore .work
    local do_restore=1
    [ "$wrong_no_restore" = 1 ] && [ "$t" -ne 0 ] && do_restore=0
    local state_work="${state_dir}.work"
    if [ "$do_restore" = 1 ]; then
      rm -rf "$state_work"
      cp -a "$snapshot" "$state_work"
    fi

    # step 8: netns/docker + per-run quick check
    local ns_r=sympsica_r ns_s=sympsica_s addr_r=10.99.0.1 addr_s=10.99.0.2
    local core_r=2 core_s=3 container_r="" container_s=""
    if [ "$exec_mode" = "docker" ]; then
      CUR_EXEC_MODE=docker
      docker rm -f sympsica_pr sympsica_ps >/dev/null 2>&1 || true
      docker run -d --name sympsica_pr --network none --cpuset-cpus="$core_r" \
        -v "$state_work:/state:rw" -v "$workdir:/runwork:rw" "$image" sleep infinity >/dev/null
      docker run -d --name sympsica_ps --network none --cpuset-cpus="$core_s" \
        -v "$state_work:/state:rw" -v "$workdir:/runwork:rw" "$image" sleep infinity >/dev/null
      container_r=sympsica_pr; container_s=sympsica_ps
      bash "$NETEM" netns-down >/dev/null 2>&1 || true
      wire_docker_netns sympsica_pr sympsica_r
      wire_docker_netns sympsica_ps sympsica_s
      ip link add veth_r type veth peer name veth_s
      ip link set veth_r netns sympsica_r; ip link set veth_s netns sympsica_s
      ip netns exec sympsica_r ip addr add 10.99.0.1/24 dev veth_r
      ip netns exec sympsica_s ip addr add 10.99.0.2/24 dev veth_s
      ip netns exec sympsica_r ip link set lo up; ip netns exec sympsica_r ip link set veth_r up
      ip netns exec sympsica_s ip link set lo up; ip netns exec sympsica_s ip link set veth_s up
    else
      CUR_EXEC_MODE=native
      bash "$NETEM" netns-up
    fi
    bash "$NETEM" apply --profile "$network" >/dev/null

    local verify_rc
    if [ "$env" = "LOCAL" ] && [ "$network" = "LAN" ]; then
      bash "$NETEM" verify --profile "$network" --structural; verify_rc=$?
    else
      bash "$NETEM" verify --profile "$network" --rtt-only --quick; verify_rc=$?
    fi
    if [ "$verify_rc" -ne 0 ]; then
      echo "measure.sh: netem verify failed before run -- no record" >&2
      exit 2
    fi

    # step 9: sampler
    : > "$workdir/pids"
    init_threads_json "$workdir"
    run_sampler "$workdir" &
    local sampler_pid=$!

    # step 10: bytes before
    local tx_r_before tx_s_before
    tx_r_before=$(tx_bytes "$ns_r" veth_r); tx_s_before=$(tx_bytes "$ns_s" veth_s)

    # step 11: launch wrapper
    ( SYMPSICA_MEASURE=1 NS_R="$ns_r" NS_S="$ns_s" ADDR_R="$addr_r" ADDR_S="$addr_s" \
      CORE_R="$core_r" CORE_S="$core_s" STATE_WORK="$state_work" \
      CONTAINER_R="$container_r" CONTAINER_S="$container_s" RUN_WORKDIR="$workdir" \
      timeout --signal=KILL -k 5 "$budget_s" bash "$wrapper" run --config "$config" \
        --out "$workdir/partial.json" --workdir "$workdir" \
        >"$workdir/wrapper.stdout" 2>"$workdir/wrapper.stderr" ) &
    local wpid=$!
    wait "$wpid"
    local rc=$?
    local kill_ns
    kill_ns=$(mono_ns)

    local status
    if [ "$rc" -eq 0 ]; then status=ok
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then status=timeout
    else status=error
    fi

    if [ "$status" != "ok" ]; then
      if [ -f "$workdir/pids" ]; then
        while IFS='=' read -r side pid; do [ -n "${pid:-}" ] && kill -KILL "$pid" 2>/dev/null; done < "$workdir/pids"
      fi
      if [ "$exec_mode" = "docker" ]; then
        docker exec sympsica_pr pkill -KILL -f . >/dev/null 2>&1 || true
        docker exec sympsica_ps pkill -KILL -f . >/dev/null 2>&1 || true
      fi
    fi

    # step 12: bytes after; stop sampler
    local tx_r_after tx_s_after r_out s_out
    tx_r_after=$(tx_bytes "$ns_r" veth_r); tx_s_after=$(tx_bytes "$ns_s" veth_s)
    r_out=$(( tx_r_after - tx_r_before )); s_out=$(( tx_s_after - tx_s_before ))

    kill -TERM "$sampler_pid" 2>/dev/null
    wait "$sampler_pid" 2>/dev/null
    if [ ! -s "$workdir/threads.json" ]; then
      echo "sampler produced no threads.json" >&2
      exit 4
    fi

    # step 13: segments + emit
    local seg_json="$workdir/segments.json"
    local out_target="$out"
    [ "$is_warmup" = 1 ] && out_target="$workroot/warmup.jsonl"

    if [ "$status" = "ok" ]; then
      if ! python3 "$JSONL_CHECK" segments --stderr "$workdir/wrapper.stderr" > "$seg_json" 2>"$workdir/segments.err"; then
        cat "$workdir/segments.err" >&2
        status=error
        python3 "$JSONL_CHECK" segments --stderr "$workdir/wrapper.stderr" --allow-incomplete > "$seg_json"
      fi
    else
      python3 "$JSONL_CHECK" segments --stderr "$workdir/wrapper.stderr" --allow-incomplete > "$seg_json"
    fi

    local emit_args=(--config "$config" --env "$env" --status "$status" --segments "$seg_json"
                      --r-out "$r_out" --s-out "$s_out" --observations "$workdir/threads.json"
                      --partial "$workdir/partial.json" --trial "$emit_trial" --out "$out_target")
    [ "$status" != "ok" ] && emit_args+=(--kill-ns "$kill_ns")
    python3 "$JSONL_CHECK" emit "${emit_args[@]}"

    if [ "$is_warmup" != 1 ]; then
      # the emitted record's status is authoritative (build_record's A5/A6
      # post-hoc policy may have flipped ok -> error after this wrapper's
      # own exit code already looked clean) -- tally THAT, not $status.
      local final_status
      final_status=$(python3 -c '
import json, sys
print(json.loads(open(sys.argv[1]).readlines()[-1])["status"])' "$out_target")
      case "$final_status" in
        ok) ok_count=$((ok_count + 1)) ;;
        timeout) timeout_count=$((timeout_count + 1)) ;;
        *) error_count=$((error_count + 1)) ;;
      esac
    fi
  done

  echo "measure: $trials measured records ($ok_count ok, $timeout_count timeout, $error_count error) -> $out"
}

# ---------------------------------------------------------------------------
# selftest-bytes [--wrong-sum-both] (HSTx1)
# ---------------------------------------------------------------------------
cmd_selftest_bytes() {
  local wrong_sum_both=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --wrong-sum-both) wrong_sum_both=1; shift ;;
      *) echo "measure.sh selftest-bytes: unknown argument $1" >&2; exit 3 ;;
    esac
  done

  trap on_run_exit EXIT
  bash "$NETEM" netns-up   # NO shaping -- bare veth pair

  mkdir -p /tmp/sympsica-bench
  cat > /tmp/sympsica-bench/sink.py <<'PYEOF'
import socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("10.99.0.2", 5010))
s.listen(1)
c, _ = s.accept()
n = 0
while True:
    b = c.recv(1 << 16)
    if not b:
        break
    n += len(b)
print(n)
PYEOF

  ip netns exec sympsica_s python3 /tmp/sympsica-bench/sink.py \
    >/tmp/sympsica-bench/sink.out 2>/tmp/sympsica-bench/sink.err &
  local sink_pid=$!

  wait_listen sympsica_s 5010 5
  local wl_rc=$?
  if [ "$wl_rc" -ne 0 ]; then
    kill -KILL "$sink_pid" 2>/dev/null
    exit "$wl_rc"
  fi

  local tx_r_before rx_r_before tx_s_before rx_s_before
  tx_r_before=$(tx_bytes sympsica_r veth_r); rx_r_before=$(rx_bytes sympsica_r veth_r)
  tx_s_before=$(tx_bytes sympsica_s veth_s); rx_s_before=$(rx_bytes sympsica_s veth_s)

  ip netns exec sympsica_r python3 -c '
import socket
s = socket.create_connection(("10.99.0.2", 5010))
b = b"\0" * 65536
for _ in range(160):
    s.sendall(b)
s.close()'

  wait "$sink_pid"

  local tx_r_after rx_r_after tx_s_after rx_s_after
  tx_r_after=$(tx_bytes sympsica_r veth_r); rx_r_after=$(rx_bytes sympsica_r veth_r)
  tx_s_after=$(tx_bytes sympsica_s veth_s); rx_s_after=$(rx_bytes sympsica_s veth_s)

  local r_tx=$(( tx_r_after - tx_r_before )) r_rx=$(( rx_r_after - rx_r_before ))
  local s_tx=$(( tx_s_after - tx_s_before )) s_rx=$(( rx_s_after - rx_s_before ))

  if [ "$wrong_sum_both" = 1 ]; then
    local total=$(( r_tx + r_rx + s_tx + s_rx ))
    echo "HSTx1 FAILED: sent+received double-counts the one 10 MB blob: $total B outside [10485760,11534336] -- the field convention is per-party OUTGOING bytes (FT11)" >&2
    exit 2
  fi

  local total=$(( r_tx + s_tx ))
  if [ "$total" -lt 10485760 ] || [ "$total" -gt 11534336 ] || [ "$s_tx" -gt 524288 ]; then
    echo "HSTx1 FAILED: external per-party outgoing total $total B outside [10485760,11534336] or s_out $s_tx B > 524288" >&2
    exit 2
  fi
  echo "HSTx1 OK: external per-party outgoing total $total B in [10485760,11534336], s_out $s_tx B"
}

# ---------------------------------------------------------------------------
# selftest-state --demo <name>
# ---------------------------------------------------------------------------
cmd_selftest_state() {
  local demo=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --demo) demo=$2; shift 2 ;;
      *) echo "measure.sh selftest-state: unknown argument $1" >&2; exit 3 ;;
    esac
  done
  [ -n "$demo" ] || { echo "measure.sh selftest-state: --demo is required" >&2; exit 3; }

  local wrapper="$SCRIPT_DIR/selftest_wrapper.sh"
  local config="$SCRIPT_DIR/selftest.json"
  local out="/tmp/sympsica-bench/selftest-${demo}.jsonl"
  mkdir -p /tmp/sympsica-bench
  rm -f "$out"
  local state_dir
  state_dir=$(cfg "$config" state_dir)

  local rc=0
  case "$demo" in
    ok)
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env LOCAL >/dev/null
      bash "$SELF" run --wrapper "$wrapper" --config "$config" --out "$out" --env LOCAL --trials 2 --warmup 1
      rc=$?
      ;;
    no-restore)
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env LOCAL >/dev/null
      bash "$SELF" run --wrapper "$wrapper" --config "$config" --out "$out" --env LOCAL --trials 2 --warmup 1 --wrong-no-restore
      rc=$?
      ;;
    two-threads)
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env LOCAL >/dev/null
      SELFTEST_TWO_THREADS=1 bash "$SELF" run --wrapper "$wrapper" --config "$config" --out "$out" --env LOCAL --trials 2 --warmup 1
      rc=$?
      ;;
    fail-party)
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env LOCAL >/dev/null
      SELFTEST_FAIL_PARTY=1 bash "$SELF" run --wrapper "$wrapper" --config "$config" --out "$out" --env LOCAL --trials 2 --warmup 1
      rc=$?
      ;;
    timeout)
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env LOCAL >/dev/null
      local tmp_cfg=/tmp/sympsica-bench/selftest-timeout-config.json
      python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); d["budget_s"]=5; json.dump(d, open(sys.argv[2],"w"))' "$config" "$tmp_cfg"
      SELFTEST_SLEEP_S=30 bash "$SELF" run --wrapper "$wrapper" --config "$tmp_cfg" --out "$out" --env LOCAL --trials 2 --warmup 1
      rc=$?
      ;;
    no-pids)
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env LOCAL >/dev/null
      SELFTEST_SKIP_PIDS=1 bash "$SELF" run --wrapper "$wrapper" --config "$config" --out "$out" --env LOCAL --trials 2 --warmup 1
      rc=$?
      ;;
    foreign-prepared)
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env LOCAL >/dev/null
      echo deadbeef > "$state_dir/.prepared"
      bash "$SELF" run --wrapper "$wrapper" --config "$config" --out "$out" --env LOCAL --trials 2 --warmup 1
      rc=$?
      rm -f "$state_dir/.prepared"
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env LOCAL >/dev/null
      ;;
    bad-sentinel)
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env LOCAL >/dev/null
      local boot_id sentinel
      boot_id=$(cat /proc/sys/kernel/random/boot_id)
      sentinel="/tmp/sympsica-netem-ok-LAN-$boot_id"
      python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); d["mode"]="full"; json.dump(d, open(sys.argv[1],"w"))' "$sentinel"
      bash "$SELF" run --wrapper "$wrapper" --config "$config" --out "$out" --env LOCAL --trials 2 --warmup 1
      rc=$?
      # the internal `run` sub-process's own EXIT trap already tore the
      # netns pair down; bring it back before we can re-calibrate LAN.
      bash "$NETEM" netns-up >/dev/null 2>&1
      bash "$NETEM" calibrate --profile LAN --date "$(date -u +%F)" --env LOCAL --structural >/dev/null 2>&1
      bash "$NETEM" netns-down >/dev/null 2>&1
      ;;
    prepare-env)
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env LOCAL >/dev/null
      bash "$SELF" prepare --wrapper "$wrapper" --config "$config" --env AWS
      rc=$?
      ;;
    *)
      echo "measure.sh selftest-state: unknown --demo '$demo'" >&2
      exit 3
      ;;
  esac
  return $rc
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
main() {
  if [ $# -eq 0 ]; then
    echo "usage: measure.sh <run|prepare|selftest-bytes|selftest-state> [options]" >&2
    exit 3
  fi
  require_root
  local sub="$1"; shift
  case "$sub" in
    run) cmd_run "$@" ;;
    prepare) cmd_prepare "$@" ;;
    selftest-bytes) cmd_selftest_bytes "$@" ;;
    selftest-state) cmd_selftest_state "$@" ;;
    *) echo "measure.sh: unknown subcommand '$sub'" >&2; exit 3 ;;
  esac
}

main "$@"
