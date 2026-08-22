#!/usr/bin/env bash
# bench/aws/run_calib.sh -- Task 34 Part B (phase-8-plan.md; controller amendments
# A4/A5/A10/A11). Runs on macOS after R8-AWS-CONSENT. Required env: KEYPAIR_NAME
# SSH_SECURITY_GROUP_ID BUDGET_EMAIL SSH_PRIVATE_KEY_PATH. The user has run
# `aws login` first.
#
# A11 (user directive, 2026-08-22): the calibration body must survive the
# operator's laptop sleeping or shutting down, and must be attachable from a
# different machine. This script therefore has exactly TWO failure-handling
# phases, each owned by its OWN ERR trap, and they do NOT overlap:
#
#   PHASE 1 (this process, LOCAL, SSH-driven): resolve the AMI, launch the
#     instance (--instance-initiated-shutdown-behavior stop lets it stop
#     itself later with a plain `sudo shutdown -h now`, no IAM role needed),
#     wait for SSH, provision (incl. reboot handling -- a reboot cannot be
#     survived by an on-instance script, so it MUST stay in this phase), the
#     four host-gate negatives, netns-up. `on_err` (LOCAL) covers ONLY this
#     phase: any failure here still has a live SSH session and a live local
#     process, so it stops the instance itself and reports -- exactly as
#     before A11.
#   PHASE 2 (a script generated below and started inside a DETACHED tmux
#     session named "sympsica" on the instance, decoupled from this SSH
#     connection and this laptop): the netem negatives, the four `calibrate`
#     profile runs, the BMS+24 SC prepare/run/accept + its own negative. This
#     script WRITES that body to the instance, starts it via
#     `tmux new-session -d -s sympsica '...'`, and THEN RETURNS -- it does
#     NOT wait for phase 2 and does not depend on the SSH connection or this
#     laptop staying up. Phase 2 owns ITS OWN ERR trap (no IAM role, no local
#     dependency): on success OR failure it writes a DONE/FAILED marker plus
#     a tee'd log under ~/results/, waits a 60-minute grace window (so the
#     operator can attach/fetch from anywhere), then self-stops via
#     `sudo shutdown -h now`. NEVER terminate, in either phase.
set -Eeuo pipefail
REPO=/Users/heewonchung/Documents/04-Dev/01-research/active/symm-upsi-ca
REGION=us-east-1; DATE=$(date -u +%F)
cd "$REPO"; source bench/lib.sh
: "${KEYPAIR_NAME:?}" "${SSH_SECURITY_GROUP_ID:?}" "${BUDGET_EMAIL:?}" "${SSH_PRIVATE_KEY_PATH:?}"
test -r "$SSH_PRIVATE_KEY_PATH" || { echo "SSH_PRIVATE_KEY_PATH not readable" >&2; exit 3; }
aws sts get-caller-identity >/dev/null || { echo "no live AWS session: run 'aws login' first" >&2; exit 4; }
INSTANCE_ID=""; IP=""; SSH_READY=0; CLEANUP_ARMED=0

# --- PHASE 1 ERR trap (LOCAL: launch/provision/host-gates/netns-up only) ---
on_err() {
  local rc=$?
  if [ "$CLEANUP_ARMED" = 1 ] && [ -n "$INSTANCE_ID" ]; then
    echo "FAILURE (rc=$rc) during launch/provision: stopping $INSTANCE_ID (never terminating)" >&2
    aws ec2 stop-instances --region "$REGION" --instance-ids "$INSTANCE_ID" >/dev/null || true
    if aws ec2 wait instance-stopped --region "$REGION" --instance-ids "$INSTANCE_ID"; then
      echo "REPORT: instance $INSTANCE_ID STOPPED after failure; results may be partial" >&2
    else
      echo "REPORT: stop requested for $INSTANCE_ID but final state UNVERIFIED -- check the console; instance id retained" >&2
    fi
  fi
  exit "$rc"
}
trap on_err ERR

AMI=$(aws ssm get-parameter --region "$REGION" --name /aws/service/canonical/ubuntu/server/22.04/stable/current/amd64/hvm/ebs-gp2/ami-id --query Parameter.Value --output text)
python3 -c "import json,sys; d=json.load(open('bench/aws/launch-template.json')); d['ImageId']=sys.argv[1]; d['KeyName']=sys.argv[2]; d['SecurityGroupIds']=[sys.argv[3]]; json.dump(d,open('/tmp/run-instances.json','w'),indent=1)" "$AMI" "$KEYPAIR_NAME" "$SSH_SECURITY_GROUP_ID"
# A11: --instance-initiated-shutdown-behavior stop -- lets the on-instance
# PHASE 2 script self-stop with `sudo shutdown -h now`, no IAM role needed.
# terminate-instances remains forbidden everywhere; this keeps a self-stop
# reversible (EBS persists; the instance can be started again).
aws ec2 run-instances --region "$REGION" --instance-initiated-shutdown-behavior stop --cli-input-json file:///tmp/run-instances.json > /tmp/launch.json
INSTANCE_ID=$(python3 -c "import json; print(json.load(open('/tmp/launch.json'))['Instances'][0]['InstanceId'])")
CLEANUP_ARMED=1
echo "INSTANCE_ID=$INSTANCE_ID AMI=$AMI" | tee /tmp/sympsica-bench-aws-launch.log
aws ec2 wait instance-running --region "$REGION" --instance-ids "$INSTANCE_ID"
IP=$(aws ec2 describe-instances --region "$REGION" --instance-ids "$INSTANCE_ID" --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
SSH="ssh -i $SSH_PRIVATE_KEY_PATH -o StrictHostKeyChecking=accept-new"
for i in $(seq 1 30); do if $SSH ubuntu@"$IP" true 2>/dev/null; then SSH_READY=1; break; fi; sleep 10; done
[ "$SSH_READY" = 1 ] || { echo "sshd never became ready on $IP" >&2; false; }
ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
set +e
BUDGET_OUT=$(aws budgets create-budget --account-id "$ACCOUNT_ID" \
  --budget '{"BudgetName":"sympsica-bench","BudgetLimit":{"Amount":"400","Unit":"USD"},"TimeUnit":"MONTHLY","BudgetType":"COST"}' \
  --notifications-with-subscribers "[{\"Notification\":{\"NotificationType\":\"ACTUAL\",\"ComparisonOperator\":\"GREATER_THAN\",\"Threshold\":100,\"ThresholdType\":\"PERCENTAGE\"},\"Subscribers\":[{\"SubscriptionType\":\"EMAIL\",\"Address\":\"$BUDGET_EMAIL\"}]}]" 2>&1); BUDGET_RC=$?
set -e
if [ "$BUDGET_RC" != 0 ]; then
  echo "$BUDGET_OUT" | grep -q DuplicateRecordException || { echo "budget creation failed: $BUDGET_OUT" >&2; false; }
  echo "budget already exists (DuplicateRecordException) -- OK"
fi
rsync -a -e "$SSH" --exclude results/ --exclude build/ --exclude 'build-*/' "$REPO"/ ubuntu@"$IP":~/symm-upsi-ca/
R() { $SSH ubuntu@"$IP" "cd ~/symm-upsi-ca && source bench/lib.sh && $*"; }   # remote, positive rows (trap-protected)
RN() {  # expected-negative rows: RN <expected_rc> <expected_substring> -- <remote command...>
  local want_rc=$1 want_sub=$2; shift 2; [ "$1" = "--" ] && shift
  local out rc=0
  out=$($SSH ubuntu@"$IP" "cd ~/symm-upsi-ca && source bench/lib.sh && $*" 2>&1) || rc=$?
  printf '%s\n' "$out"
  if [ "$rc" != "$want_rc" ] || ! printf '%s' "$out" | grep -qF "$want_sub"; then
    echo "EXPECTED-NEGATIVE MISMATCH: rc=$rc (want $want_rc); substring '$want_sub' $(printf '%s' "$out" | grep -qF "$want_sub" && echo found || echo MISSING)" >&2
    return 1   # trips the ERR trap -> stop-instances
  fi
  echo "expected-negative OK (rc=$rc, matched '$want_sub')"
}
R "LOGGED ~/provision.log -- bash bench/aws/provision.sh --out ~/aws-host.json"
if R "test -f /var/run/reboot-required"; then
  # A reboot cannot be survived by an on-instance tmux script (tmux state is
  # process state, gone across a hard reboot) -- this MUST stay in PHASE 1.
  $SSH ubuntu@"$IP" sudo reboot || true; sleep 60
  aws ec2 wait instance-running --region "$REGION" --instance-ids "$INSTANCE_ID"
  SSH_READY=0; for i in $(seq 1 30); do if $SSH ubuntu@"$IP" true 2>/dev/null; then SSH_READY=1; break; fi; sleep 10; done
  [ "$SSH_READY" = 1 ] || { echo "sshd never came back after reboot" >&2; false; }
  R "LOGGED ~/provision-check.log -- bash bench/aws/provision.sh --check-only --out ~/aws-host.json"
fi
RN 4 "HARD GATE FAILED: nproc=32" -- "bash bench/aws/provision.sh --check-only --out /tmp/x.json --nproc-override 32"
RN 4 "Thread(s) per core=2" -- "bash bench/aws/provision.sh --check-only --out /tmp/x.json --tpc-override 2"
RN 3 "--out is required" -- "bash bench/aws/provision.sh --check-only"
RN 4 "sch_netem_DOES_NOT_EXIST not loaded" -- "bash bench/aws/provision.sh --check-only --out /tmp/x.json --simulate-no-netem"
R "sudo bash bench/netem.sh netns-up"

# --- PHASE 2 body (A11): generated here, shipped to the instance, started ---
# detached, never waited on by this process. Uses a QUOTED heredoc (no outer
# substitution at all) so every $ below is the ON-INSTANCE script's own --
# it computes its own DATE independently (matches this script's own
# `date -u +%F` convention; the two calls are seconds apart, so they agree in
# every case that matters).
cat > /tmp/on_instance_calib.sh <<'ONINSTANCE'
#!/usr/bin/env bash
# Generated by bench/aws/run_calib.sh (A11 PHASE 2 body). Started inside
# `tmux new-session -d -s sympsica`, decoupled from the SSH session/laptop
# that launched it. Owns its OWN ERR trap + self-stop -- the LOCAL script's
# trap does not apply here and does not know this phase exists.
set -Eeuo pipefail
cd ~/symm-upsi-ca
source bench/lib.sh
DATE=$(date -u +%F)
RESULTS=~/results
mkdir -p "$RESULTS" results/aws
LOG="$RESULTS/progress.log"
exec > >(tee -a "$LOG") 2>&1
echo "=== PHASE 2 (tmux session sympsica) starting $(date -u +%FT%TZ) ==="

GRACE_S=3600
finish() {  # $1 = DONE|FAILED
  local marker=$1
  echo "$marker $(date -u +%FT%TZ)" > "$RESULTS/$marker"
  echo "=== $marker -- 60-minute grace window before self-stop (attach: tmux attach -t sympsica) ==="
  sleep "$GRACE_S"
  echo "=== grace window elapsed -- self-stopping (instance-initiated-shutdown-behavior=stop; never terminate) ==="
  sudo shutdown -h now
}
on_err() {
  local rc=$?
  echo "PHASE 2 FAILURE (rc=$rc) at $(date -u +%FT%TZ)"
  docker rm -f sympsica_bms_lo_r sympsica_bms_lo_s >/dev/null 2>&1 || true
  finish FAILED
  exit "$rc"
}
trap on_err ERR

RN() {  # RN <expected_rc> <expected_substring> -- <command...> (same contract as PHASE 1's)
  local want_rc=$1 want_sub=$2; shift 2; [ "$1" = "--" ] && shift
  local out rc=0
  out=$(bash -c "$*" 2>&1) || rc=$?
  printf '%s\n' "$out"
  if [ "$rc" != "$want_rc" ] || ! printf '%s' "$out" | grep -qF "$want_sub"; then
    echo "EXPECTED-NEGATIVE MISMATCH: rc=$rc (want $want_rc); substring '$want_sub' $(printf '%s' "$out" | grep -qF "$want_sub" && echo found || echo MISSING)"
    return 1
  fi
  echo "expected-negative OK (rc=$rc, matched '$want_sub')"
}

for P in LAN WAN200 WAN50 WAN5; do
  LOGGED "$RESULTS/calib-$P.log" -- sudo bash bench/netem.sh calibrate --profile "$P" --date "$DATE" --env AWS
done
RN 2 "throughput" -- "LOGGED $RESULTS/neg-rate-double.log -- bash -c 'sudo bash bench/netem.sh apply --profile WAN200 --wrong-rate-double && sudo bash bench/netem.sh verify --profile WAN200'"
LOGGED "$RESULTS/calib-WAN200-restore.log" -- sudo bash bench/netem.sh calibrate --profile WAN200 --date "$DATE" --env AWS
RN 2 "one-sided shaping halves it" -- "LOGGED $RESULTS/neg-one-sided.log -- bash -c 'sudo bash bench/netem.sh apply --profile WAN200 --wrong-one-sided && sudo bash bench/netem.sh verify --profile WAN200 --rtt-only'"
RN 2 "sent+received double-counts" -- "LOGGED $RESULTS/neg-sum-both.log -- sudo bash bench/measure.sh selftest-bytes --wrong-sum-both"

set +e
LOGGED "$RESULTS/bms24-calib-prepare.log" -- sudo bash bench/measure.sh prepare --wrapper baselines/bms24/run.sh --config bench/aws/bms24_calib.json --env AWS --budget-s 7200
PREP_RC=$?
set -e
if [ "$PREP_RC" = 124 ]; then
  echo "BMS+24 setup exceeded the 7200 s watchdog (risk register) -- self-stopping; cache/partial setup persist on EBS"
  finish FAILED
  exit 124
fi
[ "$PREP_RC" = 0 ] || { echo "bms24 prepare failed rc=$PREP_RC"; exit "$PREP_RC"; }

LOGGED "$RESULTS/bms24-calib-run.log" -- sudo bash bench/measure.sh run --wrapper baselines/bms24/run.sh --config bench/aws/bms24_calib.json --out results/aws/bms24_calib.jsonl --env AWS --warmup 1
LOGGED "$RESULTS/bms24-calib-accept.log" -- python3 bench/jsonl_check.py accept --file results/aws/bms24_calib.jsonl --count 1 --status ok --bytes-within 41130000,50270000 --online-within 11.6,1160 --flag counters=na --flag bytes=combined-loopback
RN 2 "ACC: bytes.total" -- "python3 bench/jsonl_check.py accept --file results/aws/bms24_calib.jsonl --bytes-within 0,1000"

cp ~/aws-host.json "$RESULTS/aws-host-$DATE.json"
echo "=== PHASE 2 complete: results in ~/symm-upsi-ca/results/aws/, ~/symm-upsi-ca/bench/calib/, ~/results/ ==="
finish DONE
ONINSTANCE
chmod +x /tmp/on_instance_calib.sh
rsync -a -e "$SSH" /tmp/on_instance_calib.sh ubuntu@"$IP":~/on_instance_calib.sh
$SSH ubuntu@"$IP" "tmux new-session -d -s sympsica 'bash ~/on_instance_calib.sh'"
CLEANUP_ARMED=0
cat <<REPORT
LAUNCHED: instance $INSTANCE_ID ($IP) is running the calibration body inside a detached tmux
session ("sympsica"); this script has returned and does not depend on this SSH session, this
shell, or this laptop staying up.

Attach to watch live:   ssh -i $SSH_PRIVATE_KEY_PATH -o StrictHostKeyChecking=accept-new ubuntu@$IP -t tmux attach -t sympsica
Tail the combined log:  ssh -i $SSH_PRIVATE_KEY_PATH ubuntu@$IP tail -f ~/results/progress.log
Fetch results/logs:     rsync -a -e "ssh -i $SSH_PRIVATE_KEY_PATH" ubuntu@$IP:~/symm-upsi-ca/results/aws/ "$REPO"/results/aws/
                        rsync -a -e "ssh -i $SSH_PRIVATE_KEY_PATH" ubuntu@$IP:~/symm-upsi-ca/bench/calib/ "$REPO"/bench/calib/
                        rsync -a -e "ssh -i $SSH_PRIVATE_KEY_PATH" ubuntu@$IP:~/results/ /tmp/sympsica-aws-results-$DATE/

On completion or failure the instance writes ~/results/DONE or ~/results/FAILED, waits a 60-minute
grace window (so you can attach/fetch from anywhere), then self-stops via "sudo shutdown -h now"
(never terminates -- instance-initiated-shutdown-behavior=stop was set at launch, no IAM role
needed). If you miss the grace window the instance simply stops with results intact on EBS;
"aws ec2 start-instances --region $REGION --instance-ids $INSTANCE_ID" resumes it for retrieval.

NOTE: the security group allows SSH only from the one IP address it was provisioned with.
Attaching or fetching from a different network requires that IP to be added first (the
controller can do this -- a one-line change).
REPORT
echo "INSTANCE_ID=$INSTANCE_ID"
