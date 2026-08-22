#!/usr/bin/env bash
# bench/aws/provision.sh -- W8.0, materialized from .handoff/aws-guide.md SS3 (the
# guide is the authority). Run ON the instance as ubuntu:
#   bash bench/aws/provision.sh --out ~/aws-host.json        (logs -> stderr)
# Deliberate delta (controller-approved): aws-guide's
#   pip3 install pytest pyyaml numpy pandas matplotlib
# is Phase-9/10 analysis tooling and is NOT run here (Phase-8 gates are stdlib-
# only); python3-pip itself IS installed per the guide's apt list.
set -euo pipefail
OUT=""; CHECK_ONLY=0; NPROC_OVERRIDE=""; TPC_OVERRIDE=""; NETEM_MOD=sch_netem
while [ $# -gt 0 ]; do case "$1" in
  --out) OUT=$2; shift 2;; --check-only) CHECK_ONLY=1; shift;;
  --nproc-override) NPROC_OVERRIDE=$2; shift 2;; --tpc-override) TPC_OVERRIDE=$2; shift 2;;
  --simulate-no-netem) NETEM_MOD=sch_netem_DOES_NOT_EXIST; shift;;   # TEST-ONLY (R6 negative)
  *) echo "usage: provision.sh --out FILE [--check-only] [--nproc-override N] [--tpc-override N] [--simulate-no-netem]" >&2; exit 3;; esac; done
[ -n "$OUT" ] || { echo "--out is required" >&2; exit 3; }
if [ "$CHECK_ONLY" = 0 ]; then
  { sudo apt-get update
    sudo apt-get install -y build-essential cmake ninja-build git python3-pip \
      docker.io iproute2 iperf3 jq tmux "linux-modules-extra-$(uname -r)"
    sudo usermod -aG docker ubuntu; } 1>&2
fi
sudo modprobe sch_netem 1>&2 || true
lsmod | grep -q "^$NETEM_MOD " || { echo "HARD GATE FAILED: $NETEM_MOD not loaded" >&2; exit 4; }
NPROC=${NPROC_OVERRIDE:-$(nproc)}
TPC=${TPC_OVERRIDE:-$(lscpu | sed -n 's/^Thread(s) per core:[[:space:]]*//p')}
[ "$NPROC" = 16 ] || { echo "HARD GATE FAILED: nproc=$NPROC != 16 (wrong instance type or SMT on)" >&2; exit 4; }
[ "$TPC" = 1 ]    || { echo "HARD GATE FAILED: Thread(s) per core=$TPC != 1 (SMT on)" >&2; exit 4; }
TOKEN=$(curl -sX PUT http://169.254.169.254/latest/api/token -H 'X-aws-ec2-metadata-token-ttl-seconds: 300')
imds() { curl -s -H "X-aws-ec2-metadata-token: $TOKEN" "http://169.254.169.254/latest/meta-data/$1"; }
python3 - "$NPROC" "$TPC" "$(lscpu | sed -n 's/^Model name:[[:space:]]*//p')" "$(uname -r)" \
  "$(cat /proc/sys/kernel/random/boot_id)" "$(imds instance-type)" "$(imds instance-id)" \
  "$(imds ami-id)" "$(imds placement/region)" "$(imds placement/availability-zone)" "$OUT" <<'PYEOF'
import json, sys, datetime
n, tpc, model, kernel, bootid, itype, iid, ami, region, az, out = sys.argv[1:12]
doc = {"date": datetime.date.today().isoformat(), "env": "AWS",
       "nproc": int(n), "threads_per_core": int(tpc), "cpu_model": model, "kernel": kernel,
       "governor": "n/a (EC2 Nitro)", "boot_id": bootid, "instance_type": itype,
       "instance_id": iid, "ami_id": ami, "region": region, "availability_zone": az}
json.dump(doc, open(out, "w"), indent=1); open(out, "a").write("\n")
print("wrote " + out)
PYEOF
echo "NOTE: log out and back in (or run: newgrp docker) before using docker" >&2
