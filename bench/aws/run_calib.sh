#!/usr/bin/env bash
# bench/aws/run_calib.sh -- Task 34 Part B (phase-8-plan.md; controller amendments
# A4/A5/A10/A11/A12). Runs on macOS after R8-AWS-CONSENT. Required env: KEYPAIR_NAME
# SSH_SECURITY_GROUP_ID BUDGET_EMAIL SSH_PRIVATE_KEY_PATH. The user has run
# `aws login` first.
#
# A11 (user directive, 2026-08-22): the calibration body must survive the
# operator's laptop sleeping or shutting down, and must be attachable from a
# different machine. This script therefore has exactly TWO failure-handling
# phases, each owned by its OWN ERR trap, and they do NOT overlap:
#
#   PHASE 1 (this process, LOCAL, SSH-driven): resolve the AMI, launch the
#     instance, wait for SSH, provision (incl. reboot handling -- a reboot
#     cannot be survived by an on-instance script, so it MUST stay in this
#     phase), the four host-gate negatives, netns-up. `on_err` (LOCAL) covers
#     ONLY this phase: any failure here still has a live SSH session and a
#     live local process, so it stops the instance itself and reports.
#   PHASE 2 (a script generated below and started inside a DETACHED tmux
#     session named "sympsica" on the instance, decoupled from this SSH
#     connection and this laptop): the A10 clause-3 loopback repeat, the
#     netem negatives, the four `calibrate` profile runs, the BMS+24 SC
#     prepare/run/accept + its own negative. This script WRITES that body to
#     the instance, starts it via `tmux new-session -d -s sympsica '...'`,
#     verifies the instance has actually acknowledged it, and THEN
#     RETURNS -- it does not wait for phase 2 to finish and does not depend
#     on the SSH connection or this laptop staying up.
#
# A12 (Codex review, 2026-08-22 -- amendment following the DO-NOT-RUN Part-A
# review): "instance keeps billing with nothing scheduled to stop it" has
# more than one strand, and a single local ERR trap or a single on-instance
# sleep-then-shutdown does not cover all of them. This script layers FOUR
# independent mechanisms, each covering a different failure mode the review
# traced, so at least one always fires:
#   1. BOOT-LEVEL FAILSAFE (launch-template.json's UserData): `shutdown -h
#      +480` scheduled as the literal first thing cloud-init runs, before
#      SSH, before this script even connects. Covers: the launch response
#      lost/malformed, this laptop dying immediately after launch, PHASE 2
#      never starting or dying before its own trap arms -- anything that
#      leaves NOTHING else running to stop the instance. `shutdown -h`
#      (poweroff signal) + `--instance-initiated-shutdown-behavior stop` =
#      the instance stops itself, never terminates, no IAM role needed.
#   2. A PERSISTED EC2 CLIENT TOKEN on run-instances. Covers: an ambiguous
#      or lost `run-instances` response before INSTANCE_ID is even parsed
#      and PHASE-1 cleanup is armed -- the token in
#      /tmp/sympsica-bench-client-token lets the exact same instance be
#      found and stopped afterward instead of being un-findable (and
#      client-token is EC2's own idempotency key, so a retry cannot create
#      a duplicate instance either).
#   3. PHASE 1's local ERR trap, now additionally verifying (before it EVER
#      disarms) that the instance has acknowledged BOTH the boot-level
#      failsafe (mechanism 1 actually ran) AND a live tmux session (PHASE 2
#      actually started) -- not just that the `tmux new-session` SSH command
#      itself returned 0.
#   4. PHASE 2's own on-instance trap, covering ERR HUP TERM (not ERR alone
#      -- a dying tmux server delivers a signal, not a command failure) and
#      armed as the FIRST possible action, before any fallible `cd`/
#      `source`/`mkdir`. Its `finish()` REPLACES the boot-level failsafe
#      with an independent `shutdown -h +60` (cancel-then-reschedule, not a
#      blocking sleep) the moment DONE/FAILED is written -- the OS-level
#      timer then does not depend on this process, or tmux, staying alive
#      for the grace window to actually elapse.
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

# A12 mechanism 2: persisted client token (see the module header). run-instances still gets
# --instance-initiated-shutdown-behavior stop (self-stop with a plain `shutdown -h`, no IAM
# role); the EC2 termination command remains forbidden everywhere, in every phase -- a
# self-stop stays reversible (EBS persists; the instance can be started again).
CLIENT_TOKEN="sympsica-bench-$(date -u +%Y%m%dT%H%M%SZ)-$$"
echo "$CLIENT_TOKEN" > /tmp/sympsica-bench-client-token
echo "CLIENT_TOKEN=$CLIENT_TOKEN" | tee -a /tmp/sympsica-bench-aws-launch.log
RUN_RC=0
aws ec2 run-instances --region "$REGION" --client-token "$CLIENT_TOKEN" \
  --instance-initiated-shutdown-behavior stop --cli-input-json file:///tmp/run-instances.json \
  > /tmp/launch.json 2>/tmp/launch.err || RUN_RC=$?
if [ "$RUN_RC" != 0 ] || ! INSTANCE_ID=$(python3 -c "
import json, sys
try:
    print(json.load(open('/tmp/launch.json'))['Instances'][0]['InstanceId'])
except Exception:
    sys.exit(1)" 2>/dev/null); then
  echo "run-instances response missing or malformed (rc=$RUN_RC) -- attempting client-token recovery" >&2
  cat /tmp/launch.err 2>/dev/null >&2 || true
  sleep 5
  aws ec2 describe-instances --region "$REGION" \
    --filters "Name=client-token,Values=$CLIENT_TOKEN" \
    --query 'Reservations[0].Instances[0].InstanceId' --output text > /tmp/recovered-instance-id.txt 2>/dev/null || true
  INSTANCE_ID=$(cat /tmp/recovered-instance-id.txt 2>/dev/null || true)
  [ -n "$INSTANCE_ID" ] && [ "$INSTANCE_ID" != "None" ] || {
    echo "could not recover an instance id via client-token $CLIENT_TOKEN -- check the AWS console for a stray instance with this token before retrying" >&2
    false
  }
  echo "RECOVERED INSTANCE_ID=$INSTANCE_ID via client-token" | tee -a /tmp/sympsica-bench-aws-launch.log
fi
CLEANUP_ARMED=1
echo "INSTANCE_ID=$INSTANCE_ID AMI=$AMI" | tee -a /tmp/sympsica-bench-aws-launch.log
aws ec2 wait instance-running --region "$REGION" --instance-ids "$INSTANCE_ID"
IP=$(aws ec2 describe-instances --region "$REGION" --instance-ids "$INSTANCE_ID" --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
SSH="ssh -i $SSH_PRIVATE_KEY_PATH -o StrictHostKeyChecking=accept-new"
for i in $(seq 1 30); do if $SSH ubuntu@"$IP" true 2>/dev/null; then SSH_READY=1; break; fi; sleep 10; done
[ "$SSH_READY" = 1 ] || { echo "sshd never became ready on $IP" >&2; false; }

# A12 mechanism 1, verified early (fail fast rather than discover at the very end) AND again
# after any reboot below (fix round 2, Codex re-review): confirm the boot-level failsafe
# actually ran on THIS boot. `shutdown -h +480` on a systemd host creates
# /run/systemd/shutdown/scheduled -- verified by real measurement on colima's systemd Ubuntu VM
# (task-34-report.md): schedule/cancel/reschedule all produce and remove that exact file.
#
# CORRECTION (Codex re-review of fix round 1): a plain `#!/bin/bash` UserData script is
# cloud-init's `scripts-user` module, which runs at frequency ONCE-PER-INSTANCE, NOT per boot
# -- it does NOT re-run on a reboot. The report's original comment claiming otherwise was an
# unchecked assumption about cloud-init's default behaviour, exactly the class of defect this
# whole review round exists to catch. Since PHASE 1 DOES reboot the instance below whenever
# `linux-modules-extra` (installed for sch_netem) triggers /var/run/reboot-required, the
# +480 min failsafe set by the FIRST boot's UserData run would otherwise be silently gone by
# the time PHASE 2 needs it. Fix: UserData now ALSO installs
# /var/lib/cloud/scripts/per-boot/00-sympsica-failsafe.sh (cloud-init's `scripts-per-boot`
# module, which DOES run on every boot including the post-provisioning reboot) so any boot,
# planned or not, re-arms the failsafe -- and this verification is explicitly re-run after
# that reboot too, refusing to proceed if it is absent, rather than trusting the self-healing
# mechanism without checking it.
verify_failsafe_scheduled() {
  local ok=0
  for i in $(seq 1 10); do
    if $SSH ubuntu@"$IP" "test -f /run/systemd/shutdown/scheduled" 2>/dev/null; then ok=1; break; fi
    sleep 3
  done
  [ "$ok" = 1 ] || { echo "boot-level failsafe shutdown (user-data / per-boot script) is not scheduled on the instance -- refusing to proceed" >&2; return 1; }
}
verify_failsafe_scheduled || false

ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
# I2 (Codex review, Task 34 fix round): the FIXED controller resource is the budget
# "sympsica-bench-monthly" with ACTUAL>80% + FORECASTED>100% notifications -- creating a
# differently-named/configured budget here would never hit DuplicateRecordException against
# it, it would just silently create a SECOND budget. Use the exact name and both thresholds.
# I1-class fix applied here too: `cmd || VAR=$?` (not `set +e`/`set -e`) is what actually
# suppresses the ERR trap for a deliberately-captured failure -- `set +e` alone does not
# (errtrace/-E keeps the trap live regardless), so the old form never reached the
# DuplicateRecordException branch on a real failure either.
BUDGET_RC=0
BUDGET_OUT=$(aws budgets create-budget --account-id "$ACCOUNT_ID" \
  --budget '{"BudgetName":"sympsica-bench-monthly","BudgetLimit":{"Amount":"400","Unit":"USD"},"TimeUnit":"MONTHLY","BudgetType":"COST"}' \
  --notifications-with-subscribers "[{\"Notification\":{\"NotificationType\":\"ACTUAL\",\"ComparisonOperator\":\"GREATER_THAN\",\"Threshold\":80,\"ThresholdType\":\"PERCENTAGE\"},\"Subscribers\":[{\"SubscriptionType\":\"EMAIL\",\"Address\":\"$BUDGET_EMAIL\"}]},{\"Notification\":{\"NotificationType\":\"FORECASTED\",\"ComparisonOperator\":\"GREATER_THAN\",\"Threshold\":100,\"ThresholdType\":\"PERCENTAGE\"},\"Subscribers\":[{\"SubscriptionType\":\"EMAIL\",\"Address\":\"$BUDGET_EMAIL\"}]}]" 2>&1) || BUDGET_RC=$?
if [ "$BUDGET_RC" != 0 ]; then
  echo "$BUDGET_OUT" | grep -q DuplicateRecordException || { echo "budget creation failed: $BUDGET_OUT" >&2; false; }
  echo "budget sympsica-bench-monthly already exists (DuplicateRecordException) -- OK"
fi
rsync -a -e "$SSH" --exclude results/ --exclude build/ --exclude 'build-*/' "$REPO"/ ubuntu@"$IP":~/symm-upsi-ca/
R() { $SSH ubuntu@"$IP" "cd ~/symm-upsi-ca && source bench/lib.sh && $*"; }   # remote, positive rows (trap-protected)
RN() {  # expected-negative rows: RN <expected_rc> <expected_substring> -- <remote command...>
  local want_rc=$1 want_sub=$2; shift 2; [ "$1" = "--" ] && shift
  local out rc=0
  out=$($SSH ubuntu@"$IP" "cd ~/symm-upsi-ca && source bench/lib.sh && $*" 2>&1) || rc=$?
  printf '%s\n' "$out"
  if [ "$rc" != "$want_rc" ] || ! printf '%s' "$out" | grep -qF -e "$want_sub"; then
    echo "EXPECTED-NEGATIVE MISMATCH: rc=$rc (want $want_rc); substring '$want_sub' $(printf '%s' "$out" | grep -qF -e "$want_sub" && echo found || echo MISSING)" >&2
    return 1   # trips the ERR trap -> stop-instances
  fi
  echo "expected-negative OK (rc=$rc, matched '$want_sub')"
}
R "LOGGED ~/provision.log -- bash bench/aws/provision.sh --out ~/aws-host.json"
if R "test -f /var/run/reboot-required"; then
  # A reboot cannot be survived by an on-instance tmux script (tmux state is
  # process state, gone across a hard reboot) -- this MUST stay in PHASE 1.
  # The boot-level failsafe re-arms across THIS reboot via the per-boot script UserData
  # installed (cloud-init's `scripts-per-boot` module runs on every boot; the UserData
  # script itself is `scripts-user`, once-per-instance only -- see the comment above
  # verify_failsafe_scheduled for the full correction). Re-verified explicitly below,
  # not trusted on the strength of the mechanism alone.
  $SSH ubuntu@"$IP" sudo reboot || true; sleep 60
  aws ec2 wait instance-running --region "$REGION" --instance-ids "$INSTANCE_ID"
  SSH_READY=0; for i in $(seq 1 30); do if $SSH ubuntu@"$IP" true 2>/dev/null; then SSH_READY=1; break; fi; sleep 10; done
  [ "$SSH_READY" = 1 ] || { echo "sshd never came back after reboot" >&2; false; }
  verify_failsafe_scheduled || false
  R "LOGGED ~/provision-check.log -- bash bench/aws/provision.sh --check-only --out ~/aws-host.json"
fi
RN 4 "HARD GATE FAILED: nproc=32" -- "bash bench/aws/provision.sh --check-only --out /tmp/x.json --nproc-override 32"
RN 4 "Thread(s) per core=2" -- "bash bench/aws/provision.sh --check-only --out /tmp/x.json --tpc-override 2"
RN 3 "--out is required" -- "bash bench/aws/provision.sh --check-only"
RN 4 "sch_netem_DOES_NOT_EXIST not loaded" -- "bash bench/aws/provision.sh --check-only --out /tmp/x.json --simulate-no-netem"
R "sudo bash bench/netem.sh netns-up"

# --- PHASE 2 body (A11/A12): generated here, shipped to the instance, started ---
# detached, never waited on by this process. Uses a QUOTED heredoc (no outer
# substitution at all) so every $ below is the ON-INSTANCE script's own --
# it computes its own DATE independently (matches this script's own
# `date -u +%F` convention; the two calls are seconds apart, so they agree in
# every case that matters).
cat > /tmp/on_instance_calib.sh <<'ONINSTANCE'
#!/usr/bin/env bash
# Generated by bench/aws/run_calib.sh (A11 PHASE 2 body, A12 watchdog). Started
# inside `tmux new-session -d -s sympsica`, decoupled from the SSH session/
# laptop that launched it. Owns its OWN trap + self-stop -- the LOCAL script's
# trap does not apply here and does not know this phase exists.
set -Eeuo pipefail

# A12 mechanism 4: RESULTS/finish/on_err/trap are set up FIRST, using nothing
# but bash builtins and `sudo shutdown` -- no dependency on `cd`, `source
# bench/lib.sh`, or `mkdir` succeeding, because the trap must be armed BEFORE
# any of those (deliberately fallible) commands, not after. `finish()`
# replaces the boot-level ~8h failsafe with an independent, OS-scheduled
# ~1h grace shutdown the moment DONE/FAILED is written -- it does NOT block
# (no `sleep`), so the grace window elapses even if this process, or tmux
# itself, dies immediately afterward.
RESULTS=~/results
GRACE_MIN=60
finish() {  # $1 = DONE|FAILED
  local marker=$1
  mkdir -p "$RESULTS" 2>/dev/null || true
  echo "$marker $(date -u +%FT%TZ)" > "$RESULTS/$marker" 2>/dev/null || true
  echo "=== $marker -- replacing the boot-level failsafe with an independent ${GRACE_MIN}-min grace shutdown (attach: tmux attach -t sympsica) ==="
  sudo shutdown -c 2>/dev/null || true
  sudo shutdown -h "+$GRACE_MIN"
  echo "=== ${GRACE_MIN}-min grace window scheduled at the OS level (not a sleeping process) -- exiting now; the timer does not depend on this process or tmux staying alive ==="
}
on_err() {
  local rc=$?
  echo "PHASE 2 FAILURE (rc=$rc) at $(date -u +%FT%TZ)" 2>/dev/null || true
  docker rm -f sympsica_bms_lo_r sympsica_bms_lo_s >/dev/null 2>&1 || true
  finish FAILED
  exit "$rc"
}
trap on_err ERR HUP TERM

cd ~/symm-upsi-ca
source bench/lib.sh
DATE=$(date -u +%F)
mkdir -p "$RESULTS" results/aws
LOG="$RESULTS/progress.log"
exec > >(tee -a "$LOG") 2>&1
echo "=== PHASE 2 (tmux session sympsica) starting $(date -u +%FT%TZ) ==="

RN() {  # RN <expected_rc> <expected_substring> -- <command...> (same contract as PHASE 1's)
  local want_rc=$1 want_sub=$2; shift 2; [ "$1" = "--" ] && shift
  local out rc=0
  out=$(bash -c "source bench/lib.sh; $*" 2>&1) || rc=$?
  printf '%s\n' "$out"
  if [ "$rc" != "$want_rc" ] || ! printf '%s' "$out" | grep -qF -e "$want_sub"; then
    echo "EXPECTED-NEGATIVE MISMATCH: rc=$rc (want $want_rc); substring '$want_sub' $(printf '%s' "$out" | grep -qF -e "$want_sub" && echo found || echo MISSING)"
    return 1
  fi
  echo "expected-negative OK (rc=$rc, matched '$want_sub')"
}

# C3 (Codex review, Task 34 fix round): A10 clause 3 must be repeated on the environment of
# record, before any bms24 row -- inheriting the colima result is not enough. Real single-
# netns loopback transfer of a known byte count, recorded under ~/results/, aborting rather
# than proceeding if this environment's `lo` counts bytes in a materially different way
# (roughly halved/doubled) than colima did.
echo "=== A10 clause 3: AWS loopback byte-counting convention (must be measured here, not inherited) ==="
sudo ip netns del sympsica_clause3_probe 2>/dev/null || true
sudo ip netns add sympsica_clause3_probe
sudo ip netns exec sympsica_clause3_probe ip link set lo up
cat > /tmp/lo_sink.py <<'PYSINK'
import socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 15010))
s.listen(1)
c, _ = s.accept()
n = 0
while True:
    b = c.recv(1 << 16)
    if not b:
        break
    n += len(b)
print(n)
PYSINK
sudo ip netns exec sympsica_clause3_probe python3 /tmp/lo_sink.py > /tmp/lo_sink.out 2>/tmp/lo_sink.err &
SINK_PID=$!
sleep 1
CLAUSE3_TX_BEFORE=$(sudo ip netns exec sympsica_clause3_probe ip -s -j link show dev lo | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["stats64"]["tx"]["bytes"])')
sudo ip netns exec sympsica_clause3_probe python3 -c '
import socket
s = socket.create_connection(("127.0.0.1", 15010))
b = b"\0" * 65536
for _ in range(160):
    s.sendall(b)
s.close()'
wait "$SINK_PID"
CLAUSE3_TX_AFTER=$(sudo ip netns exec sympsica_clause3_probe ip -s -j link show dev lo | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["stats64"]["tx"]["bytes"])')
sudo ip netns del sympsica_clause3_probe
CLAUSE3_N_SENT=10485760
CLAUSE3_TX_DELTA=$((CLAUSE3_TX_AFTER - CLAUSE3_TX_BEFORE))
{
  echo "clause-3 AWS measurement command: known ${CLAUSE3_N_SENT}B single-netns loopback transfer, tx delta of lo"
  echo "clause-3 AWS measurement: N_SENT=$CLAUSE3_N_SENT tx_before=$CLAUSE3_TX_BEFORE tx_after=$CLAUSE3_TX_AFTER tx_delta=$CLAUSE3_TX_DELTA"
} | tee "$RESULTS/clause3-aws.log"
python3 -c "
import sys
n_sent, tx_delta = $CLAUSE3_N_SENT, $CLAUSE3_TX_DELTA
ratio = tx_delta / n_sent
# colima's own measurement (task-34-report.md): ratio ~1.0026 (0.26% TCP/IP framing overhead,
# lo counts once). [0.99, 1.10] tolerates real per-host framing variance while still catching
# a KIND-different behavior (roughly halved ~0.5 or doubled ~2.0) on this environment.
if not (0.99 <= ratio <= 1.10):
    sys.stderr.write('CLAUSE-3 DISAGREEMENT: tx_delta/n_sent ratio %.4f outside [0.99,1.10] -- lo counts bytes differently on AWS than colima; STOP and escalate rather than trust the inherited arithmetic\n' % ratio)
    sys.exit(1)
print('clause-3 OK: ratio %.4f consistent with colima (lo counts the combined total once, small framing overhead)' % ratio)
" | tee -a "$RESULTS/clause3-aws.log"

for P in LAN WAN200 WAN50 WAN5; do
  LOGGED "$RESULTS/calib-$P.log" -- sudo bash bench/netem.sh calibrate --profile "$P" --date "$DATE" --env AWS
done
RN 2 "throughput" -- "LOGGED $RESULTS/neg-rate-double.log -- bash -c 'sudo bash bench/netem.sh apply --profile WAN200 --wrong-rate-double && sudo bash bench/netem.sh verify --profile WAN200'"
LOGGED "$RESULTS/calib-WAN200-restore.log" -- sudo bash bench/netem.sh calibrate --profile WAN200 --date "$DATE" --env AWS
RN 2 "one-sided shaping halves it" -- "LOGGED $RESULTS/neg-one-sided.log -- bash -c 'sudo bash bench/netem.sh apply --profile WAN200 --wrong-one-sided && sudo bash bench/netem.sh verify --profile WAN200 --rtt-only'"
RN 2 "sent+received double-counts" -- "LOGGED $RESULTS/neg-sum-both.log -- sudo bash bench/measure.sh selftest-bytes --wrong-sum-both"

# Task 4 (gate fix round 1, C1, R6-NOTAUTO): the pre-fix combined netem
# delay+rate construction on bms24's `lo` path, exercised through the ACTUAL
# throughput gate (bms_lo_verify_throughput) rather than just the structural
# read-back that let it ship unnoticed the first time. A throwaway netns
# (sympsica_lo_negtest) is enough -- no docker pair needed, since the
# TEST-ONLY flag never launches a party.
RN 5 "forward throughput" -- "LOGGED $RESULTS/neg-wrong-combined-netem.log -- sudo bash -c 'source bench/lib.sh; source baselines/bms24/run.sh; BMS_LO_NS=sympsica_lo_negtest; ip netns del \$BMS_LO_NS 2>/dev/null; ip netns add \$BMS_LO_NS; ip netns exec \$BMS_LO_NS ip link set lo up; ip netns exec \$BMS_LO_NS ip link set lo mtu 1500; bms_lo_apply_netem WAN200 10501 --wrong-combined-netem; bms_lo_verify_throughput WAN200 10501; rc=\$?; ip netns del \$BMS_LO_NS 2>/dev/null; exit \$rc'"

# I1 (Codex review, Task 34 fix round): `set +e` does NOT suppress an ERR trap (errtrace/-E
# keeps it live regardless of errexit), so the old `set +e; cmd; RC=$?; set -e` form let the
# generic on_err fire on ANY prepare failure before this 124 branch could ever run. Being on
# the LEFT of `||` (not the last command in the list) is what actually exempts a command from
# the trap -- `cmd || PREP_RC=$?` captures the real producer status without ever tripping it.
PREP_RC=0
LOGGED "$RESULTS/bms24-calib-prepare.log" -- sudo bash bench/measure.sh prepare --wrapper baselines/bms24/run.sh --config bench/aws/bms24_calib.json --env AWS --budget-s 7200 || PREP_RC=$?
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

# A12 mechanism 3: do NOT disarm local cleanup on the mere success of the `tmux new-session`
# SSH command (that only proves tmux itself accepted the request, not that PHASE 2's script
# is actually alive inside it) -- verify a live tmux session first.
TMUX_OK=0
for i in $(seq 1 15); do
  if $SSH ubuntu@"$IP" "tmux has-session -t sympsica" 2>/dev/null; then TMUX_OK=1; break; fi
  sleep 2
done
[ "$TMUX_OK" = 1 ] || { echo "tmux session 'sympsica' did not come up on the instance" >&2; false; }
CLEANUP_ARMED=0
cat <<REPORT
LAUNCHED: instance $INSTANCE_ID ($IP) is running the calibration body inside a detached tmux
session ("sympsica"), confirmed alive; this script has returned and does not depend on this
SSH session, this shell, or this laptop staying up. Boot-level failsafe confirmed scheduled
(mechanism 1); client token $CLIENT_TOKEN persisted at /tmp/sympsica-bench-client-token
(mechanism 2) in case this instance ever needs to be found independently of this log.

Attach to watch live:   ssh -i $SSH_PRIVATE_KEY_PATH -o StrictHostKeyChecking=accept-new ubuntu@$IP -t tmux attach -t sympsica
Tail the combined log:  ssh -i $SSH_PRIVATE_KEY_PATH ubuntu@$IP tail -f ~/results/progress.log
Fetch results/logs:     rsync -a -e "ssh -i $SSH_PRIVATE_KEY_PATH" ubuntu@$IP:~/symm-upsi-ca/results/aws/ "$REPO"/results/aws/
                        rsync -a -e "ssh -i $SSH_PRIVATE_KEY_PATH" ubuntu@$IP:~/symm-upsi-ca/bench/calib/ "$REPO"/bench/calib/
                        rsync -a -e "ssh -i $SSH_PRIVATE_KEY_PATH" ubuntu@$IP:~/results/ /tmp/sympsica-aws-results-$DATE/

On completion or failure the instance writes ~/results/DONE or ~/results/FAILED, replaces the
boot-level failsafe with an independent 60-minute grace shutdown (so you can attach/fetch from
anywhere), then self-stops (never terminates -- instance-initiated-shutdown-behavior=stop was
set at launch, no IAM role needed). If nothing on the instance ever reaches that point (tmux
itself dies, a kernel-level failure, etc.), the original boot-level failsafe still fires on its
own schedule. If you miss the grace window the instance simply stops with results intact on
EBS; "aws ec2 start-instances --region $REGION --instance-ids $INSTANCE_ID" resumes it for
retrieval.

NOTE: the security group allows SSH only from the one IP address it was provisioned with.
Attaching or fetching from a different network requires that IP to be added first (the
controller can do this -- a one-line change).
REPORT
echo "INSTANCE_ID=$INSTANCE_ID"
