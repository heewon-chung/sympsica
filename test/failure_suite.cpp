// test/failure_suite.cpp — task-20-brief.md W6.1: the TV-F negative
// registry. Consolidates every TV-F1..TV-F17 row (authority:
// .handoff/sympsica-test-vectors.md lines 91-117, "Failure conditions &
// misinterpretation traps") into ONE auditable place: for each row, WHERE
// the wrong construction lives, the exact reproduction command, and (for
// COVERED rows) the observed failure signature this task actually ran and
// captured. task-20-report.md carries the full command + real-output-tail
// transcript for every COVERED row below.
//
// This file does NOT re-run any negative itself (requirement 4: the
// registry test must be FAST, < 5s) -- it asserts RECORDED evidence about
// work done elsewhere in this repo (existing KATs under test/, the five
// -D negative build dirs at the repo root, run manually as part of
// task-20). "By construction this would fail" is explicitly NOT accepted
// evidence here (R6-OBSERVED) -- that is precisely the FC4 defect the
// Phase-5 gate review caught (task-19-report.md, R-FC4-REAL). Every
// COVERED row was OBSERVED failing by a real ctest/python run in task-20.
//
// Controller ruling R6-FSUITE-17 (task-20-brief.md): the plan's SC
// "failure suite 17/17 behaving as negatives" was not achievable at Phase
// 6 -- four rows (TV-F14..F17) had no subject code then; their subjects
// were owned by later phases (.handoff/sympsica-plan.md's Phase->file
// ownership table). This registry ships all 17 rows in exactly one of two
// states: COVERED (17 rows, TV-F1..TV-F17 as of task-32);
// DEFERRED-TO-PHASE-n rows: none remain, never silently absent.
// Each DEFERRED row names its owning phase
// AND its missing subject file, and SC3's guard below asserts that file
// is currently ABSENT -- a SELF-EXPIRING deferral: the moment a later
// phase creates that file, this suite FAILS, forcing the row to be wired
// into a real COVERED negative instead of staying deferred forever.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <fstream>
#include <set>
#include <string>

namespace {

enum class RowState { kCovered, kDeferred };

struct Row {
    const char* id;                     // "TV-F1" .. "TV-F17"
    RowState state;
    const char* subject;                // subject file/mechanism (non-empty, SC1)
    const char* justification;          // non-empty, SC1
    const char* command;                // COVERED: exact reproduction command run in task-20
    const char* observed;               // COVERED: observed failure signature (real output)
    const char* deferred_owning_phase;  // DEFERRED only: e.g. "Phase 7"; "" for COVERED
    const char* deferred_subject_path;  // DEFERRED only: path relative to repo root; "" for COVERED
};

// clang-format off
constexpr std::array<Row, 17> kRegistry{{
{"TV-F1", RowState::kCovered,
 "include/sympsica/utils/field.hpp (read_fp) -- test/utils/kat_field.cpp",
 "feed FLD/ENC fixtures big-endian -> KAT mismatch (controller ruling, task-11-report.md: the "
 "PRIMARY path is a silent MISMATCH to a DIFFERENT canonical value, not an abort -- read_fp's "
 "canonicity check only rejects >= P, and a BE/LE-confused value is very often still < P; a "
 "SUPPLEMENTARY test documents the non-canonical/abort case too)",
 "ctest --test-dir build -R "
 "'Field\\.TVF1_BigEndianMismatchDetectedNotAborted|FieldDeathTest\\.TVF1_BigEndianNonCanonicalAborts'",
 "both PASSED (task-20, this run): (a) byte-reversing 0x0100000000000002's wire encoding decodes "
 "as the DIFFERENT canonical value 0x0200000000000001 (EXPECT_NE(decoded, golden) fires, no "
 "abort); (b) byte-reversing value 255's wire encoding lands >= P and EXPECT_DEATH matches "
 "\"read_fp: value not canonical\"",
 "", ""},

{"TV-F2", RowState::kCovered,
 "test/gates/oracle_dpf/kat_oracle_libote.cpp (pure-arithmetic simulation, no 2nd DPF lib needed)",
 "DPF additive shares over Z_2^64, each independently reduced mod p -> wraps in the u64 sum shift "
 "the mod-p reconstruction by +8 (2^64 == 8 mod p), NOT 0, so near-wrap payloads mismatch ZT-4's "
 "oracle",
 "ctest --test-dir build -R 'OracleDpf\\.TVF2_Z2p64IndependentReductionBreaksNearWrapOracle'",
 "PASSED (task-20, this run): for payloads {p-1, p-2} over 10 seeds each, "
 "ASSERT_GT(mismatches, 0u) held -- naive mod-2^64-then-mod-p reduction fails to reconstruct the "
 "true payload whenever the u64 sum wraps (overwhelmingly likely for a near-wrap payload)",
 "", ""},

{"TV-F3", RowState::kCovered,
 "include/sympsica/gates/ztest.hpp (ZeroTest::check_canonical_entry) -- test/gates/kat_ztest.cpp",
 "non-canonical z or r (raw value >= p, constructed directly via struct init -- read_fp's own "
 "canonicity check means this cannot arrive any other way over the wire) aborts at ZeroTest's own "
 "entry guard rather than propagating a garbage boundary result",
 "ctest --test-dir build -R 'GatesZtest\\.TVF3_NonCanonicalInputAbortsAtGateEntry'",
 "PASSED (task-20, this run): both EXPECT_DEATH(ZeroTest::check_canonical_entry(...), \"\") calls "
 "(non-canonical D share, non-canonical gate mask share) fired the abort",
 "", ""},

{"TV-F4", RowState::kCovered,
 "include/sympsica/utils/encoding.hpp (Encoder::sigma) -- test/utils/kat_encoding.cpp",
 "sigma as g*x (linear) or x^g (power) instead of the real g^x -- ENC-2's concrete toy-field row "
 "(g=2, p=101, x=6, real sigma=64) distinguishes both misreadings by construction. Task-20 NEW "
 "WORK (R6-FSUITE): ENC-2 itself only PINNED the correct value and documented the misreadings in "
 "a comment; TVF4_LinearAndPowerMisreadingsGiveWrongValue makes them EXECUTABLE",
 "ctest --test-dir build -R 'Encoding\\.TVF4_LinearAndPowerMisreadingsGiveWrongValue'",
 "PASSED (task-20, this run): g*x = 12 != 64, x^g = 6^2 mod 101 = 36 != 64 -- both misreadings "
 "distinguishable from the real sigma(6)=64",
 "", ""},

{"TV-F5", RowState::kCovered,
 "CMake option SYMPSICA_NO_FILTER (src/protocols/update.cpp) -- build-nofilter/, TEST-ONLY flag, "
 "default OFF",
 "building with the Protocol-3 duplicate/redundant-edit filter disabled drives PowerSumTable "
 "coefficients out of {0,+-1}: a duplicate insert corrupts my_ids/my_size/table/J instead of being "
 "a no-op (UPD-5's own positive-leg assertions, unmodified, become the negative under this build)",
 "cmake --build build-nofilter -j4 --target sympsica_tests_protocols && "
 "ctest --test-dir build-nofilter -R 'update\\.Update\\.UPD5_DuplicateInsertNoEffectWithFilter'",
 "FAILED as expected (task-20, this run): st.my_ids == {66,66} (want {66}), st.my_size == 2 (want "
 "1); st2.my_ids == {77,77,77} (want {77}), st2.my_size == 3 (want 1), st2.J and "
 "st2.table.rows() both diverged from their before-state -- UPD5's own assertions catch the "
 "corruption",
 "", ""},

{"TV-F6", RowState::kCovered,
 "include/sympsica/protocols/update.hpp (Update::apply's J-marking) -- test/protocols/kat_update.cpp",
 "marking J for a filtered (skipped) edit instead of leaving J untouched would fail UPD-4's "
 "invariant; hand-constructed as negative-as-positive-assert (does not call the real buggy leg -- "
 "reproduces the described bug by hand and asserts it is distinguishable)",
 "ctest --test-dir build -R 'update\\.Update\\.TVF6_MarkingJOnASkippedEditWouldFailUPD4'",
 "PASSED (task-20, this run): EXPECT_EQ(st.J, j_before) holds for the real (correct) "
 "implementation; EXPECT_NE(buggy_J, st.J) confirms the hand-constructed \"mark J anyway\" bug "
 "IS distinguishable from the correct result",
 "", ""},

{"TV-F7", RowState::kCovered,
 "include/sympsica/gates/minors.hpp (MinorCircuit::t_of) -- test/gates/kat_minors.cpp",
 "the rejected \"first zero minor\" decision rule (t = index of the first zero D_i) would wrongly "
 "report t=1 on MIN-3's interior-accidental-zero fixture rows (D_1=0, true t in {2,3,4} via the "
 "max rule). Task-20 NEW WORK (R6-FSUITE): MIN-3 itself only PINNED the correct (max-rule) t and "
 "documented the rejected rule in a comment; TVF7_FirstZeroRuleMisreadsInteriorZeroCases makes it "
 "EXECUTABLE against the same fixture rows. Fix round 1 (controller Finding 2): the first version "
 "compared against the fixture's own t_expected column and never called MinorCircuit::t_of, so a "
 "regression of t_of TO this rejected rule would have left it green; fixed to call the REAL t_of "
 "directly and demonstrated failing under that exact regression (see task-20-report.md)",
 "ctest --test-dir build -R 'GatesMinors\\.TVF7_FirstZeroRuleMisreadsInteriorZeroCases'",
 "PASSED (task-20, this run): every interior-zero (D_1=0) row in test/fixtures/min0.fixture's "
 "min3 set had first_zero_t=1 != MinorCircuit::t_of(D) (the REAL rule, called directly) -- the "
 "rejected rule misreports on every one",
 "", ""},

{"TV-F8", RowState::kCovered,
 "CMake option SYMPSICA_WRONG_SIGN (src/protocols/query.cpp) -- build-wrongsign/, TEST-ONLY flag, "
 "default OFF",
 "flipping the Sender's syndrome sign convention to +row instead of -row loses R-SYND's asymmetry "
 "and desyncs the reconstructed count on a genuinely asymmetric set pair (E2E scale, via "
 "run_e2e_gate.py --expect-mismatch --require-asymmetric, seed-0 schedule)",
 "cmake --build build-wrongsign -j4 --target party && python3 test/e2e/run_e2e_gate.py "
 "--party-bin build-wrongsign/party --reference ref/reference.py "
 "--schedule-r test/e2e/fixtures/e2e_seed0_r.json --schedule-s test/e2e/fixtures/e2e_seed0_s.json "
 "--workdir /tmp/sympsica_task20_fc1_wrongsign --timeout-s 900 --expect-mismatch "
 "--require-asymmetric",
 "PASS (exit 0), task-20 this run: asymmetry OK (|A\\B|=599, |B\\A|=600); both parties exited 0 "
 "after 158.1s; all 3 query days mismatched reference.py (expected [401,425,420], got huge "
 "wraparound-looking values on d1/d2 and 210 on d3) -- \"FC OK: wrong construction observed "
 "FAILING at 3 query day(s)\"",
 "", ""},

{"TV-F9", RowState::kCovered,
 "CMake option SYMPSICA_WRONG_CONVERT (src/protocols/query.cpp) -- build-wrongconvert/, TEST-ONLY "
 "flag, default OFF",
 "applying the receiver-form count-identity affine conversion unconditionally on BOTH parties "
 "(conversion must be receiver-side only; the sender holds a -2^{-1}t share) desyncs the "
 "reconstructed count by roughly the affine offset (E2E scale, via run_e2e_gate.py "
 "--expect-mismatch, seed-0 schedule)",
 "cmake --build build-wrongconvert -j4 --target party && python3 test/e2e/run_e2e_gate.py "
 "--party-bin build-wrongconvert/party --reference ref/reference.py "
 "--schedule-r test/e2e/fixtures/e2e_seed0_r.json --schedule-s test/e2e/fixtures/e2e_seed0_s.json "
 "--workdir /tmp/sympsica_task20_fc2_wrongconvert --timeout-s 900 --expect-mismatch",
 "PASS (exit 0), task-20 this run: both parties exited 0 after 160.5s; all 3 query days "
 "mismatched reference.py (expected [401,425,420], got large near-2^60 values on every day) -- "
 "\"FC OK: wrong construction observed FAILING at 3 query day(s)\"",
 "", ""},

{"TV-F10", RowState::kCovered,
 "include/sympsica/core/pools.hpp (take_by_id atomic available->consumed transition) -- "
 "test/core/kat_state_pools.cpp, test/protocols/kat_setup.cpp",
 "deliberately taking the SAME corr_id twice via take_by_id (one named triple flavor, one named "
 "gate flavor, plus the production Setup-produced-pool leg) must abort -- counter-equality alone "
 "is NOT a single-use proof (unused high-water items are legal); identity-based enforcement is "
 "required",
 "ctest --test-dir build -R "
 "'StatePoolsDeathTest\\.TVF10_|SetupDeathTest\\.TVF10_ProductionLeg_DoubleTakeByIdOnSetupCorrIdAborts'",
 "all PASSED (task-20, this run): core.StatePoolsDeathTest.TVF10_ZtGate_DoubleTakeByIdAborts, "
 "core.StatePoolsDeathTest.TVF10_TakeThenTakeByIdSameIdAborts, and "
 "update.SetupDeathTest.TVF10_ProductionLeg_DoubleTakeByIdOnSetupCorrIdAborts each EXPECT_DEATH "
 "fired on the second take_by_id of the same corr_id",
 "", ""},

{"TV-F11", RowState::kCovered,
 "src/protocols/query.cpp (Query::run's atomic accumulate/write-back/reset) -- "
 "test/protocols/kat_crash_invariant.cpp",
 "committing the accumulate step before all |J| evaluations are done, then killing mid-query, "
 "must leave detectably TORN state (not a graceful atomic commit) -- Protocol-4's remark requires "
 "atomic accumulate/write-back/reset as one unit",
 "ctest --test-dir build -R "
 "'CrashInvariant\\.FC3_TVF11_WrongOrderMidQueryCommitProducesDetectablyTornState'",
 "PASSED (task-20, this run): the wrong-order partial commit's on-disk state is NOT byte-identical "
 "to the pre-commit golden -- the PRE leg discriminates. ATTRIBUTION CORRECTION (Phase-6 gate "
 "review, Minor 2): the POST leg's byte-inequality against the fully-committed golden is CONFOUNDED "
 "by fresh correlation randomness, which can make those bytes unequal on its own, so the POST "
 "byte-inequality is NOT citable as torn-state discrimination. The citable POST evidence is Task "
 "28's semantic checker apps/crash_post_check.cpp, read with its own claim boundary (gate Important "
 "2: cross-party key-set + aggregate consistency, not per-bucket values). This is an attribution "
 "defect only -- the real atomic save/rename sequence is not implicated",
 "", ""},

{"TV-F12", RowState::kCovered,
 "CMake option SYMPSICA_WRONG_BOUNDARY (src/protocols/query.cpp SwitchRule::decide) -- "
 "build-wrongboundary/, TEST-ONLY flag, default OFF (controller ruling R-FC4-REAL, "
 "task-19-report.md: replaces an earlier tautological standalone-lambda FC4 draft)",
 "relaxing SwitchRule::decide's myJ > Params::U_MAX boundary to myJ >= Params::U_MAX flips the "
 "verdict to FullAnnounced strictly AT the boundary instead of strictly above it -- the "
 "announcement bit is DECLARED leakage that must fire above u_max, never at it",
 "cmake --build build-wrongboundary -j4 --target sympsica_tests_protocols_heavy && "
 "ctest --test-dir build-wrongboundary -R "
 "'TVF12Schedule\\.SC4_And_FC4_ScheduleLevelBoundaryAt1024And1025'",
 "Subprocess aborted (task-20, this run, 36.14s): \"SYMPSICA_REQUIRE failed: read_u64_vec: length "
 "prefix exceeds buffer (src/utils/serdes.cpp:55)\" -- the relaxed boundary desyncs the two "
 "parties' path verdicts at q2, corrupting the wire protocol (matches task-19-report.md's own "
 "discovery: the desync is a wire-level corruption, not a graceful EXPECT_EQ mismatch)",
 "", ""},

{"TV-F13", RowState::kCovered,
 "include/sympsica/gates/symdiff.hpp (SymDiffEvaluator round-batching) -- test/gates/kat_symdiff.cpp",
 "evaluating buckets SEQUENTIALLY (one eval_buckets(B=1) call per bucket) does not give SD-3's "
 "batched 8-rounds-total: 8 rounds is per LAYER-BATCHED query, not per bucket, so 2 sequential "
 "single-bucket calls cost 16 rounds",
 "ctest --test-dir build -R "
 "'GatesSymdiff\\.TVF13_SequentialPerBucketEvaluationDoesNotGiveEightRoundsTotal'",
 "PASSED (task-20, this run): rounds0==rounds1==16 (8*B, B=2) for two sequential single-bucket "
 "calls; EXPECT_NE(rounds0, 8u) confirms the SD-3 \"exactly 8 rounds total\" claim fails against "
 "this usage",
 "", ""},

{"TV-F14", RowState::kCovered,
 "data/parse_ofac.py (--wrong-abbrev: abbreviation-aware entry termination, TEST-ONLY flag, "
 "refuses --out-dir) -- data/test_ofac.py",
 "an abbreviation-aware parse (pinned 13-entry list a.k.a./f.k.a./n.k.a./No./Ltd./Co./Inc./St./"
 "Jr./U.S./S.A./LLC./Corp.) refuses to terminate entries at abbreviation-final lines, merging "
 "adjacent entries: total=8574, median=31 vs the pinned convention's total=11499, median=42, so "
 "OFC-1's exact-stats assertion FAILS pointing at data/PROVENANCE.md. NOTE (controller pre-flight "
 "ruling, Phase 7): the original deferred row predicted median 28; that number depended on an "
 "unspecified abbreviation list and is NOT reproducible -- the pinned list above is now the "
 "recorded wrong construction and 31 is its observed median",
 "python3 data/test_ofac.py --wrong-abbrev TestOFC1",
 "FAILED (task-29, this run): AssertionError: Tuples differ: (114, 8574, 31, 142, 762) != "
 "(114, 11499, 42, 181, 961) -- OFC-1: (days,total,median,p90,max) diverge from the pinned parse "
 "convention -- see data/PROVENANCE.md for the pinned convention and percentile rule; unittest "
 "summary: Ran 1 test in 0.032s, FAILED (failures=1)",
 "", ""},

{"TV-F15", RowState::kCovered,
 "bench/measure.sh (selftest-bytes --wrong-sum-both: TEST-ONLY flag computing bytes as "
 "sent+received on both interfaces) -- bench/jsonl_check.py validate_record",
 "the harness self-test pushes a known 10485760-byte blob across the netns veth pair; the "
 "correct per-party OUTGOING convention (FT11) reads ~10.5 MB (payload + ~4.6% framing, "
 "window [10485760, 11534336] B); summing sent+received double-counts the blob to ~21 MB, "
 "outside the window, and selftest-bytes ABORTS with exit 2. Real run in task-31-report.md",
 "colima ssh -p x86 -- bash -lc 'cd /Users/heewonchung/Documents/04-Dev/01-research/active/"
 "symm-upsi-ca && sudo bash bench/measure.sh selftest-bytes --wrong-sum-both'",
 "REAL run (colima x86 VM, 2026-08-21, 31-B.6(b)): "
 "HSTx1 FAILED: sent+received double-counts the one 10 MB blob: 21036746 B outside "
 "[10485760,11534336] -- the field convention is per-party OUTGOING bytes (FT11) -- "
 "producer exit=2",
 "", ""},

{"TV-F16", RowState::kCovered,
 "bench/netem.sh (--wrong-one-sided: TEST-ONLY flag shaping only one veth end; verify measures "
 "ping RTT against the per-profile ROUND-TRIP target +/-10%)",
 "the WAN profiles are stated as ROUND-TRIP 80 ms and applied as compensated ~40 ms per-"
 "interface egress on BOTH veth ends; applying the per-direction delay on one side only (the "
 "loopback-style misreading, FT12) halves the measured RTT (~43 ms: controller pre-measured "
 "43.086 ms avg vs 84.037 ms both-ends on the colima VM, 2026-08-21), and netem.sh verify "
 "ABORTS with exit 2 naming the target and the both-ends rule. Real run in task-31-report.md",
 "colima ssh -p x86 -- bash -lc 'cd /Users/heewonchung/Documents/04-Dev/01-research/active/"
 "symm-upsi-ca && sudo bash bench/netem.sh netns-up && sudo bash bench/netem.sh apply "
 "--profile WAN200 --wrong-one-sided && sudo bash bench/netem.sh verify --profile WAN200 "
 "--rtt-only'",
 "REAL run (colima x86 VM, 2026-08-21, 31-A.2(c)): "
 "WRONG-CONSTRUCTION: one-sided delay applied (TEST-ONLY) -- netem verify FAILED: profile "
 "WAN200 measured RTT 42.374 ms outside 72.0..88.0 ms (target 80.0 ms; delay must sit on BOTH "
 "veth ends -- one-sided shaping halves it) -- producer exit=2",
 "", ""},

{"TV-F17", RowState::kCovered,
 "baselines/bms24/run.sh validate (add-only variant refuses del_size>0 without the explicit "
 "\"delete_mode\":\"reinsert-accumulate\" opt-in) -- test/bench/fixtures/bms24_addonly_with_dels.json",
 "the BMS+24 addition binaries have no delete concept at all, so deletions can only be dropped "
 "by OUR wrapper: feeding them to the add-only wrapper REFUSES with exit 2 and a message naming "
 "the required opt-in (never a silent drop); the Scenario-U add-only interpretation is an "
 "explicit, footnoted config (plan W8.1). validate runs on any host. Real run in task-32-report.md",
 "bash baselines/bms24/run.sh validate --config test/bench/fixtures/bms24_addonly_with_dels.json; "
 "echo exit=$?",
 "REAL run (macOS, 2026-08-21, task-32): "
 "bms24-addonly: config contains deletions (del_size=64); REFUSING -- Scenario-U add-only runs "
 "must set \"delete_mode\":\"reinsert-accumulate\" explicitly (TV-F17; plan W8.1) -- exit=2",
 "", ""},
}};
// clang-format on

// Additional negative build (R6-FSUITE): SYMPSICA_STALE_SIZES is NOT one of
// the plan's 17 TV-F rows -- it is an EXTRA negative from task-19's own
// R-FCFLAGS work (round-0's exchanged counterpart size discarded, pinned to
// 0). Recorded separately and clearly marked, per R6-FSUITE, so it never
// perturbs the pinned 13/4 counts above.
struct AdditionalRow {
    const char* label;
    const char* subject;
    const char* justification;
    const char* command;
    const char* observed;
};

constexpr AdditionalRow kAdditionalRows[] = {
    {"EXTRA (not a TV-F row): SYMPSICA_STALE_SIZES",
     "CMake option SYMPSICA_STALE_SIZES (src/protocols/query.cpp) -- build-stalesizes/, TEST-ONLY "
     "flag, default OFF",
     "discarding round-0's freshly exchanged counterpart size (pinning it to 0) desyncs the wire "
     "protocol's buffer sizing, tripping coproto's own cancellation rather than reaching JSONL "
     "output at all",
     "cmake --build build-stalesizes -j4 --target party && python3 test/e2e/run_e2e_gate.py "
     "--party-bin build-stalesizes/party --reference ref/reference.py "
     "--schedule-r test/e2e/fixtures/e2e_seed0_r.json --schedule-s test/e2e/fixtures/e2e_seed0_s.json "
     "--workdir /tmp/sympsica_task20_extra_stalesizes --timeout-s 900 --expect-mismatch "
     "--expect-crash-stderr-substring \"coproto::code::cancel\"",
     "PASS (exit 0), task-20 this run: both parties exited -6 (SIGABRT) after 52.2s; "
     "--expect-crash-stderr-substring positively matched \"coproto::code::cancel\" in a party's "
     "stderr (M2 fix, task-20-brief.md) -- \"FC OK: wrong construction crashed at least one party "
     "... with the EXPECTED signature\""},
};

} // namespace

// SC1: exactly 17 rows, ids TV-F1..TV-F17, no gaps, no duplicates.
TEST(FSuite, Registry_Exactly17RowsIdsTVF1ThroughTVF17NoGapsNoDuplicates) {
    ASSERT_EQ(kRegistry.size(), 17u);
    std::set<std::string> seen_ids;
    for (std::size_t i = 0; i < kRegistry.size(); ++i) {
        const std::string expected_id = "TV-F" + std::to_string(i + 1);
        EXPECT_EQ(std::string(kRegistry[i].id), expected_id)
            << "row " << i << ": ids must appear in order TV-F1..TV-F17 with no gaps";
        seen_ids.insert(kRegistry[i].id);
    }
    EXPECT_EQ(seen_ids.size(), 17u) << "duplicate id detected";
}

// SC1: every row is COVERED or DEFERRED, and carries a non-empty subject +
// justification (a row silently absent, or with empty fields, is a FAILURE).
TEST(FSuite, Registry_EveryRowHasNonEmptySubjectAndJustification) {
    for (const auto& row : kRegistry) {
        SCOPED_TRACE(row.id);
        EXPECT_TRUE(row.state == RowState::kCovered || row.state == RowState::kDeferred);
        ASSERT_NE(row.subject, nullptr);
        EXPECT_GT(std::string(row.subject).size(), 0u) << "empty subject";
        ASSERT_NE(row.justification, nullptr);
        EXPECT_GT(std::string(row.justification).size(), 0u) << "empty justification";
    }
}

// SC1: pin the exact counts so a future silent regression (e.g. a COVERED
// row quietly downgraded to DEFERRED, or vice versa without new evidence)
// fails this suite instead of passing unnoticed.
TEST(FSuite, Registry_PinnedCounts13Covered4Deferred) {
    std::size_t covered = 0, deferred = 0;
    for (const auto& row : kRegistry) {
        if (row.state == RowState::kCovered) {
            ++covered;
        } else {
            ++deferred;
        }
    }
    EXPECT_EQ(covered, 17u) << "Phase-8 target (task-32): all 17 rows COVERED";
    EXPECT_EQ(deferred, 0u) << "Phase-8: no deferred rows remain";
}

// SC2: every COVERED row must carry a non-empty reproduction command and
// observed-failure signature (R6-OBSERVED: recorded evidence, not a
// by-construction claim).
TEST(FSuite, Registry_CoveredRowsCarryCommandAndObservedSignature) {
    for (const auto& row : kRegistry) {
        if (row.state != RowState::kCovered) continue;
        SCOPED_TRACE(row.id);
        EXPECT_GT(std::string(row.command).size(), 0u) << "COVERED row missing its repro command";
        EXPECT_GT(std::string(row.observed).size(), 0u) << "COVERED row missing its observed signature";
        EXPECT_EQ(std::string(row.deferred_owning_phase).size(), 0u)
            << "COVERED row must not carry deferral fields";
        EXPECT_EQ(std::string(row.deferred_subject_path).size(), 0u)
            << "COVERED row must not carry deferral fields";
    }
}

// SC3: every DEFERRED row names its owning phase and its missing subject
// file, and (the self-expiring guard) that file is currently ABSENT from
// the tree. The moment a later phase creates it, this assertion FAILS,
// forcing the row to be wired into a real COVERED negative.
TEST(FSuite, Registry_DeferredRowsNameOwningPhaseAndSubjectFileIsAbsent) {
    for (const auto& row : kRegistry) {
        if (row.state != RowState::kDeferred) continue;
        SCOPED_TRACE(row.id);
        ASSERT_GT(std::string(row.deferred_owning_phase).size(), 0u)
            << "SC3: a DEFERRED row must name its owning phase";
        ASSERT_GT(std::string(row.deferred_subject_path).size(), 0u)
            << "SC3: a DEFERRED row must name its missing subject file";
        EXPECT_EQ(std::string(row.command).size(), 0u)
            << "DEFERRED row must not carry a COVERED-only command";
        EXPECT_EQ(std::string(row.observed).size(), 0u)
            << "DEFERRED row must not carry a COVERED-only observed signature";

        const std::string abs_path =
            std::string(SYMPSICA_SOURCE_DIR) + "/" + row.deferred_subject_path;
        std::ifstream f(abs_path);
        EXPECT_FALSE(f.is_open())
            << "SC3 self-expiry: " << row.id << "'s deferred subject '" << row.deferred_subject_path
            << "' now EXISTS in the tree -- this row must be wired into a real COVERED negative "
               "instead of staying deferred (see task-20-report.md and "
               ".handoff/sympsica-plan.md's Phase->file ownership table)";
    }
}

// R6-FSUITE: the extra (non-TV-F) stale-sizes negative is recorded, clearly
// marked as additional, and was itself observed failing.
TEST(FSuite, AdditionalRows_StaleSizesRecordedAndObserved) {
    ASSERT_EQ(sizeof(kAdditionalRows) / sizeof(kAdditionalRows[0]), 1u)
        << "exactly one additional (non-TV-F) row is expected at Phase 6: SYMPSICA_STALE_SIZES";
    const auto& row = kAdditionalRows[0];
    EXPECT_NE(std::string(row.label).find("not a TV-F row"), std::string::npos)
        << "the additional row must be clearly marked as NOT one of the 17";
    EXPECT_GT(std::string(row.command).size(), 0u);
    EXPECT_GT(std::string(row.observed).size(), 0u);
}
