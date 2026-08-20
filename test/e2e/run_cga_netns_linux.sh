#!/bin/sh
# test/e2e/run_cga_netns_linux.sh — task-24-brief.md W6.5 CG-A(iii)/
# R6-NETNS: the EXTERNAL Linux-network-namespace leg of the zero-
# communication Update claim.
#
# NOT wired into ctest and NOT runnable on macOS (R6-NETNS: "macOS has no
# network namespaces"). This is a manual, Linux-only script, run against a
# Linux x86_64 build of the `party` binary (this project's own build has
# been verified buildable there via colima+gcc:13, see PINS.md's "CMake
# compatibility notes" / task-9-report.md) -- e.g. inside the colima
# `x86` VM profile this task used. Requires root (network namespace
# creation) and `iproute2` (`ip netns`, present on Ubuntu 24.04 by default).
#
# What it does:
#   1. Creates a fresh network namespace, brings its loopback interface up.
#   2. Reads /sys/class/net/lo/statistics/tx_bytes BEFORE.
#   3. Runs `party --role r --update-only --schedule <1000-edit schedule>
#      --state <dir> --seed 42` INSIDE that namespace (ip netns exec) --
#      the exact same schedule test/e2e/run_cga_gate.py's macOS-side SC1/
#      SC2 leg uses (generated via `run_cga_gate.py --write-schedule-only`,
#      one source of truth, no hand-duplicated schedule).
#   4. Reads tx_bytes AFTER, from a SEPARATE `ip netns exec` invocation (a
#      genuinely external vantage point, not the measured process itself).
#      Asserts the delta is EXACTLY 0 (SC3).
#   5. FC2 [external counter non-vacuity]: in the SAME namespace, pings
#      loopback (a real send of known, nonzero bytes) and shows the SAME
#      measurement methodology reports a NONZERO delta -- proving the
#      methodology can detect traffic, not just that this one run happened
#      to see none.
#
# Usage: run_cga_netns_linux.sh <party-bin> <python3-bin> <this-repo-root> <workdir>
set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <party-bin> <python3-bin> <repo-root> <workdir>" >&2
    exit 2
fi
PARTY_BIN="$1"
PY="$2"
REPO_ROOT="$3"
WORKDIR="$4"

if [ "$(id -u)" -ne 0 ]; then
    echo "run_cga_netns_linux.sh: must run as root (network namespace creation)" >&2
    exit 2
fi

mkdir -p "$WORKDIR"
SCHEDULE="$WORKDIR/cga_schedule.json"
STATE_DIR="$WORKDIR/state_r"
NS="sympsica_cga_$$"

"$PY" "$REPO_ROOT/test/e2e/run_cga_gate.py" --write-schedule-only "$SCHEDULE"

cleanup() {
    ip netns delete "$NS" >/dev/null 2>&1 || true
}
trap cleanup EXIT

ip netns add "$NS"
ip netns exec "$NS" ip link set lo up

before=$(ip netns exec "$NS" cat /sys/class/net/lo/statistics/tx_bytes)
echo "[run_cga_netns_linux] netns=$NS interface=lo tx_bytes BEFORE=$before"

echo "[run_cga_netns_linux] command: ip netns exec $NS $PARTY_BIN --role r --update-only --schedule $SCHEDULE --state $STATE_DIR --seed 42"
set +e
ip netns exec "$NS" "$PARTY_BIN" --role r --update-only --schedule "$SCHEDULE" --state "$STATE_DIR" --seed 42
party_rc=$?
set -e
echo "[run_cga_netns_linux] party exit code: $party_rc"

after=$(ip netns exec "$NS" cat /sys/class/net/lo/statistics/tx_bytes)
delta=$((after - before))
echo "[run_cga_netns_linux] netns=$NS interface=lo tx_bytes AFTER=$after DELTA=$delta"

# FC2 [external counter non-vacuity]: same netns, same measurement
# methodology, a real send of known bytes over loopback.
fc2_before=$(ip netns exec "$NS" cat /sys/class/net/lo/statistics/tx_bytes)
ip netns exec "$NS" ping -c 3 -q 127.0.0.1 >/dev/null
fc2_after=$(ip netns exec "$NS" cat /sys/class/net/lo/statistics/tx_bytes)
fc2_delta=$((fc2_after - fc2_before))
echo "[run_cga_netns_linux] FC2 (ping 3x over lo): tx_bytes BEFORE=$fc2_before AFTER=$fc2_after DELTA=$fc2_delta"

status=0
if [ "$party_rc" -ne 0 ]; then
    echo "[run_cga_netns_linux] SC1 FAIL: party exited $party_rc (expected 0)" >&2
    status=1
fi
if [ "$delta" -ne 0 ]; then
    echo "[run_cga_netns_linux] SC3 FAIL: tx_bytes delta = $delta (expected exactly 0)" >&2
    status=1
fi
if [ "$fc2_delta" -le 0 ]; then
    echo "[run_cga_netns_linux] FC2 FAIL: ping over loopback produced tx_bytes delta = $fc2_delta (expected > 0 -- the measurement methodology cannot detect real traffic)" >&2
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "[run_cga_netns_linux] OK: SC1 (exit 0), SC3 (external tx_bytes delta == 0 exactly), FC2 (methodology detects real traffic: delta=$fc2_delta > 0)"
fi
exit "$status"
