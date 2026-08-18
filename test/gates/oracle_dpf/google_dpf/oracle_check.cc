// test/gates/oracle_dpf/google_dpf/oracle_check.cc — Task 7 (task-7-brief.md,
// W2.3 Test C, part (b)): google/distributed_point_functions vs the SAME
// plaintext point-function oracle used by part (a)
// (test/gates/oracle_dpf/kat_oracle_libote.cpp), including the near-wrap
// payloads {p-1, p-2}. Output group: IntModN<uint64_t, p>, domain 2^16 (16
// bits per digit tree, matching part (a)'s DPF domain). Tag:
// [CROSS-IMPL-SEMANTIC], not [OFFICIAL] — same rule as part (a): endian/
// digit behavior is pinned by our own local fixtures (ZT-3), never by
// cross-library key compatibility (libOTe's RegularDpfKey and Google's
// DpfKey proto are unrelated key formats; this only checks that both
// realize the same point-function SEMANTICS against the same oracle).
//
// This is a plain main()-based driver (not gtest): it lives in its own
// small bzlmod workspace (this directory's MODULE.bazel), separate from the
// CMake project that builds the rest of sympsica and from googletest's own
// bzlmod version pin, to keep the two build systems fully decoupled. `bazel
// test //:oracle_check` treats a zero exit code as pass, nonzero as fail --
// exactly what this driver returns.
//
// Golden rows embedded below mirror test/fixtures/zt4_oracle_dpf.fixture
// VERBATIM (regenerate: `python3 ref/reference.py emit-zt4 --seeds
// 0,1,2,3,4,5,6,7,8,9 --out test/fixtures/zt4_oracle_dpf.fixture` from the
// repo root). They are duplicated here as C++ constants, rather than read
// from the fixture file at runtime, because this workspace is a separate
// Bazel project and wiring Bazel-runfiles-based data access for one small
// fixture was judged not worth the complexity for a [CROSS-IMPL-SEMANTIC]
// check that is explicitly not part of the OFFICIAL gate (task-7-report.md,
// Concerns/Deviations names this and the keep-in-sync obligation). If
// test/fixtures/zt4_oracle_dpf.fixture is ever regenerated with different
// seeds, these tables MUST be regenerated to match.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "absl/numeric/int128.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "dpf/distributed_point_function.h"
#include "dpf/int_mod_n.h"
#include "dpf/status_macros.h"

namespace {

using distributed_point_functions::DistributedPointFunction;
using distributed_point_functions::DpfParameters;
using distributed_point_functions::IntModN;
using distributed_point_functions::ToValueType;

// p = 2^61 - 1 (task-2 brief, "Global constants" — the same p sympsica's
// Fp uses throughout: include/sympsica/utils/field.hpp).
constexpr unsigned __int128 kP = 2305843009213693951ULL;
using Fp = IntModN<uint64_t, kP>;

constexpr int kDomainBits = 16;
constexpr uint64_t kDomain = uint64_t(1) << kDomainBits;
constexpr uint64_t kNumDigits = 4;
constexpr uint64_t kMainPayload = 1;  // matches the Task-5 pipeline's step-5 payload

struct OracleRow {
    uint64_t seed;
    uint64_t r;
    uint64_t digits[kNumDigits];
};

// oracle_row <seed> <r> <d0> <d1> <d2> <d3> from
// test/fixtures/zt4_oracle_dpf.fixture (10 rows, seeds 0..9).
constexpr OracleRow kOracleRows[] = {
    {0, 153307352162749871ull, {52655, 31517, 43065, 544}},
    {1, 1227844342346046657ull, {23745, 35074, 11756, 4362}},
    {2, 1682153688901572302ull, {22222, 7319, 13790, 5976}},
    {3, 2092789425003139053ull, {36845, 56065, 5348, 7435}},
    {4, 1041426021413522122ull, {35530, 57907, 58226, 3699}},
    {5, 217082132513276762ull, {50010, 41865, 15116, 771}},
    {6, 2118000079115640832ull, {57344, 44527, 42457, 7524}},
    {7, 273560573251292631ull, {3543, 22834, 57828, 971}},
    {8, 2186024489510581814ull, {13878, 61333, 20912, 7766}},
    {9, 1058155691525562468ull, {24676, 48752, 21246, 3759}},
};

// `nearwrap` rows: {p-1, p-2}.
constexpr uint64_t kNearWrapPayloads[2] = {
    static_cast<uint64_t>(kP - 1),
    static_cast<uint64_t>(kP - 2),
};

int g_checks = 0;
int g_failures = 0;

void Fail(const std::string& msg)
{
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
}

// The oracle check itself, full-domain: f(x) = payload iff x == point, else
// 0 (ref/reference.py's dpf_oracle(), the SAME formula part (a)'s
// assert_matches_oracle() checks). GenerateKeys() + one CreateEvaluationContext
// per party + EvaluateUntil<Fp>(0, {}, ctx) — the non-incremental,
// single-level DPF's full-domain evaluation (empty `prefixes` at hierarchy
// level 0 evaluates every one of the kDomain leaves; see
// distributed_point_function.h's CreateEvaluationContext/EvaluateUntil doc
// comment).
absl::Status CheckOraclePoint(uint64_t point, uint64_t payload, const std::string& scope)
{
    if (point >= kDomain) return absl::InvalidArgumentError(scope + ": point out of domain");
    if (payload >= kP) return absl::InvalidArgumentError(scope + ": payload not canonical");

    DpfParameters parameters;
    parameters.set_log_domain_size(kDomainBits);
    *(parameters.mutable_value_type()) = ToValueType<Fp>();

    DPF_ASSIGN_OR_RETURN(auto dpf, DistributedPointFunction::Create(parameters));
    DPF_ASSIGN_OR_RETURN(auto keys, dpf->GenerateKeys(absl::uint128(point), Fp(payload)));

    DPF_ASSIGN_OR_RETURN(auto ctx_a, dpf->CreateEvaluationContext(keys.first));
    DPF_ASSIGN_OR_RETURN(auto ctx_b, dpf->CreateEvaluationContext(keys.second));
    DPF_ASSIGN_OR_RETURN(std::vector<Fp> out_a, (dpf->EvaluateUntil<Fp>(0, {}, ctx_a)));
    DPF_ASSIGN_OR_RETURN(std::vector<Fp> out_b, (dpf->EvaluateUntil<Fp>(0, {}, ctx_b)));

    if (out_a.size() != kDomain || out_b.size() != kDomain) {
        Fail(scope + ": unexpected output size (" + std::to_string(out_a.size()) + "/" +
             std::to_string(out_b.size()) + ", want " + std::to_string(kDomain) + ")");
        ++g_checks;
        return absl::OkStatus();
    }

    uint64_t bad = 0;
    for (uint64_t x = 0; x < kDomain; ++x) {
        const Fp recon = out_a[x] + out_b[x];
        const uint64_t want = (x == point) ? payload : 0;
        bad += (recon.value() != want);
    }
    if (bad != 0) {
        Fail(scope + ": reconstructed value diverged from the plaintext oracle at " +
             std::to_string(bad) + " / " + std::to_string(kDomain) + " leaves");
    }
    ++g_checks;
    return absl::OkStatus();
}

}  // namespace

int main()
{
    // Main payload (1): every fixture row's 4 digit points -- same public
    // points/payload as part (a)'s ZT4_LibOTeKeysMatchPlaintextOracleMainPayload.
    for (const auto& row : kOracleRows) {
        for (uint64_t j = 0; j < kNumDigits; ++j) {
            const std::string scope =
                "seed " + std::to_string(row.seed) + " digit " + std::to_string(j) + " (main payload)";
            auto status = CheckOraclePoint(row.digits[j], kMainPayload, scope);
            if (!status.ok()) Fail(scope + ": " + std::string(status.message()));
        }
    }

    // Near-wrap payloads {p-1, p-2}: reuse the SAME public points as part
    // (a)'s near-wrap test -- oracle rows 0 and 1's digit points, one row
    // per payload (task-7-brief.md: "at the same public points/payloads").
    for (uint64_t pi = 0; pi < 2; ++pi) {
        const auto& row = kOracleRows[pi];
        const uint64_t payload = kNearWrapPayloads[pi];
        for (uint64_t j = 0; j < kNumDigits; ++j) {
            const std::string scope = "near-wrap payload " + std::to_string(payload) + " digit " +
                                      std::to_string(j) + " (seed " + std::to_string(row.seed) + "'s points)";
            auto status = CheckOraclePoint(row.digits[j], payload, scope);
            if (!status.ok()) Fail(scope + ": " + std::string(status.message()));
        }
    }

    std::cout << g_checks << " oracle point checks run (full domain, " << kDomain
              << " leaves each), " << g_failures << " failures.\n";
    if (g_failures > 0) {
        std::cerr << "[CROSS-IMPL-SEMANTIC] ZT-4 part (b): google/distributed_point_functions "
                     "DIVERGED from the plaintext oracle.\n";
        return 1;
    }
    std::cout << "[CROSS-IMPL-SEMANTIC] ZT-4 part (b): google/distributed_point_functions matches "
                 "the plaintext oracle at every checked point (main payload=1 over "
              << (sizeof(kOracleRows) / sizeof(kOracleRows[0])) << " seeds x " << kNumDigits
              << " digits; near-wrap payloads {p-1, p-2}).\n";
    return 0;
}
