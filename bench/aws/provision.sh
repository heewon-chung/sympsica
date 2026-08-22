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
# I4 (Codex review, Task 34 fix round): curl -s returns success on HTTP 4xx/5xx (no --fail),
# so a transient IMDS/auth hiccup could silently write a manifest full of empty strings.
# -f (fail on HTTP error) + -S (still show the error) + bounded timeouts, then require every
# value nonempty and assert the two facts that matter most (instance type, region) before
# ever writing the JSON.
TOKEN=$(curl -fsS --connect-timeout 5 --max-time 10 -X PUT http://169.254.169.254/latest/api/token -H 'X-aws-ec2-metadata-token-ttl-seconds: 300') \
  || { echo "provision.sh: IMDSv2 token request failed" >&2; exit 4; }
[ -n "$TOKEN" ] || { echo "provision.sh: IMDSv2 token request returned empty" >&2; exit 4; }
imds() { curl -fsS --connect-timeout 5 --max-time 10 -H "X-aws-ec2-metadata-token: $TOKEN" "http://169.254.169.254/latest/meta-data/$1"; }
require_nonempty() { [ -n "$1" ] || { echo "provision.sh: IMDS metadata '$2' is empty" >&2; exit 4; }; }
ITYPE=$(imds instance-type) || { echo "provision.sh: IMDS instance-type request failed" >&2; exit 4; }; require_nonempty "$ITYPE" instance-type
IID=$(imds instance-id) || { echo "provision.sh: IMDS instance-id request failed" >&2; exit 4; }; require_nonempty "$IID" instance-id
AMI=$(imds ami-id) || { echo "provision.sh: IMDS ami-id request failed" >&2; exit 4; }; require_nonempty "$AMI" ami-id
REGION=$(imds placement/region) || { echo "provision.sh: IMDS region request failed" >&2; exit 4; }; require_nonempty "$REGION" region
AZ=$(imds placement/availability-zone) || { echo "provision.sh: IMDS availability-zone request failed" >&2; exit 4; }; require_nonempty "$AZ" availability-zone
[ "$ITYPE" = "c6i.8xlarge" ] || { echo "HARD GATE FAILED: instance-type '$ITYPE' != c6i.8xlarge" >&2; exit 4; }
[ "$REGION" = "us-east-1" ]  || { echo "HARD GATE FAILED: region '$REGION' != us-east-1" >&2; exit 4; }
python3 - "$NPROC" "$TPC" "$(lscpu | sed -n 's/^Model name:[[:space:]]*//p')" "$(uname -r)" \
  "$(cat /proc/sys/kernel/random/boot_id)" "$ITYPE" "$IID" \
  "$AMI" "$REGION" "$AZ" "$OUT" <<'PYEOF'
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
