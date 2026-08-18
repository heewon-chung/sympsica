#include "sympsica/protocols/setup.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

// macoro/sync_wait.h uses std::source_location and basic_traceable but does
// not include macoro/trace.h itself -- Setup drives a real coproto::asio
// Channel (via ch.socket()), not coproto::LocalAsyncSocket, so it hits the
// same transitive-include gap test/integration/w24_pool_gate.cpp's header
// comment already documents; supplied directly here for the same reason.
#include "macoro/trace.h"
#include "macoro/sync_wait.h"

#include "sympsica/protocols/detail/setup_ot.hpp"
#include "sympsica/protocols/detail/vole_beaver.hpp"
#include "sympsica/protocols/detail/ztgate_pipeline.hpp"
#include "sympsica/utils/common.hpp"

namespace sympsica {

namespace zt = ztgate;
namespace zvole = vole;

// sympsica::Role (core/share.hpp) and ztgate::Role (protocols/detail/
// ztgate_pipeline.hpp) are two INDEPENDENTLY declared enums -- generate_gates
// and generate_triples below `static_cast<zt::Role>(role)` a `sympsica::Role`
// straight across, which is only sound because both enumerate {Receiver,
// Sender} in the same order with the same underlying values. Pinned here so
// a future reordering of either enum fails to compile instead of silently
// swapping which party plays which role at Setup's pipeline call sites.
static_assert(static_cast<std::underlying_type_t<Role>>(Role::Receiver) ==
                      static_cast<zt::oc::u64>(zt::Role::Receiver) &&
                  static_cast<std::underlying_type_t<Role>>(Role::Sender) ==
                      static_cast<zt::oc::u64>(zt::Role::Sender),
              "sympsica::Role and ztgate::Role must agree on their enumerator values -- "
              "generate_triples/generate_gates static_cast<zt::Role>(role) relies on this");

namespace {

// --- PkOpCounter's storage ---------------------------------------------------
std::atomic<u64> g_pk_op_counter{0};

// --- R-BATCH: the W2.4 GO decision's batch size, applied to both pool kinds
// (the plan text names gates explicitly; triples get the same cap for the
// same pool-fill-size-sanity reason, documented in task-16-report.md).
constexpr std::size_t kBatchSize = 512;

// --- per-item OT costs, from vole_beaver's/ztgate_pipeline's own pinned
// costs (task-6/task-5 rulings, restated in this task's brief's "Global
// constants": 122 COTs/triple, kOtsPerGate/gate).
u64 triple_ot_pool_size(std::size_t batch) {
    return zvole::kOlePerCallOts * 2 * static_cast<u64>(batch);
}

// Cheap resample-slack insurance (mirrors w24_pool_gate.cpp's pool_size()
// convention): a sampled 61-bit mask is rejected with probability 2^-61 per
// gate, essentially never triggered across a real batch, but FC4's forced-
// boundary-mask test can legitimately trigger a resample -- so every gate
// batch (Setup::run's own fills too) carries a small flat slack budget of
// extra AND-gate OTs.
constexpr u64 kExtraRejectionSlack = 4;

u64 gate_ot_pool_size(std::size_t batch) {
    return zt::kOtsPerGate * static_cast<u64>(batch) + zt::kAndGates * kExtraRejectionSlack;
}

u64 next_corr_id(Pools& pools) { return pools.next_corr_id++; }

// Generates exactly `count` new Triples into `pools.triples`, in batches of
// at most kBatchSize, assigning corr_ids from `pools.next_corr_id` (R-CORRID)
// and consuming `st`'s persistent silent-OT machinery (R-PKOP: zero base OTs
// here -- `st` already has its base OTs set by the time this is called,
// either by Setup::run's own run_base_ots() call or by an earlier call on
// the same Pools).
void generate_triples(Pools& pools, Role role, detail::SetupOtState& st, coproto::Socket& sock,
                       std::size_t count) {
    std::size_t done = 0;
    while (done < count) {
        std::size_t batch = std::min(kBatchSize, count - done);

        zt::OtPool ot_pool;
        macoro::sync_wait(
            detail::fill_ot_pool(role, triple_ot_pool_size(batch), st, sock, ot_pool));

        zvole::BeaverBatch out;
        macoro::sync_wait(zvole::beaver_triples(static_cast<zt::Role>(role), batch, st.proto_prng,
                                                 ot_pool, sock, out));

        std::vector<Triple> triples;
        triples.reserve(batch);
        for (std::size_t i = 0; i < batch; ++i) {
            triples.push_back(
                Triple{next_corr_id(pools), Share{out.a[i]}, Share{out.b[i]}, Share{out.c[i]}});
        }
        pools.triples.refill(std::move(triples));

        done += batch;
    }
}

// Generates exactly `count` new ZtGates into `pools.gates`, in batches of at
// most kBatchSize, via the SAME production pipeline
// (ztgate::generate_ztgates) FC4 exercises directly with a non-empty
// PipelineOpts::forced_mask_halves -- Setup's own calls here always leave
// `opts.forced_mask_halves` at its default-empty value (R-HOST: the
// test-only knob lives on the production PipelineOpts struct, but Setup
// itself never sets it).
void generate_gates(Pools& pools, Role role, detail::SetupOtState& st, coproto::Socket& sock,
                     std::size_t count) {
    std::size_t done = 0;
    while (done < count) {
        std::size_t batch = std::min(kBatchSize, count - done);

        zt::OtPool ot_pool;
        macoro::sync_wait(detail::fill_ot_pool(role, gate_ot_pool_size(batch), st, sock, ot_pool));

        zt::PipelineOpts opts;
        opts.count = batch;
        std::vector<zt::ZtGateOut> raw;
        zt::PipelineStats stats;
        macoro::sync_wait(zt::generate_ztgates(static_cast<zt::Role>(role), sock, opts,
                                                st.proto_prng, ot_pool, raw, stats));

        std::vector<ZtGate> gates;
        gates.reserve(batch);
        for (auto& g : raw) {
            // R-UNIFY: core::ZtGate produced NATIVELY here (corr_id assigned
            // at generation) -- mask_half/digit_shares are the pipeline's
            // own private-to-the-party working values and are intentionally
            // dropped; test/gates/ztgate_convert.hpp's converter is a
            // SEPARATE, test-only copy of this same field-for-field mapping
            // used by tests that call generate_ztgates directly (R-UNIFY:
            // that converter stays test-side, untouched by this task).
            gates.push_back(ZtGate{next_corr_id(pools), Share{g.mask_share}, std::move(g.key)});
        }
        pools.gates.refill(std::move(gates));

        done += batch;
    }
}

} // namespace

// --- PkOpCounter --------------------------------------------------------------

u64 PkOpCounter::value() { return g_pk_op_counter.load(std::memory_order_relaxed); }
void PkOpCounter::increment() { g_pk_op_counter.fetch_add(1, std::memory_order_relaxed); }

// --- Pools' special members (out-of-line: detail::SetupOtState is only
// complete in setup_ot.hpp, included above -- required for unique_ptr's
// destructor/move to instantiate; net.hpp's Channel pimpl uses the same
// out-of-line-special-members pattern for the same reason). -----------------

Pools::Pools() = default;
Pools::~Pools() = default;
Pools::Pools(Pools&&) noexcept = default;
Pools& Pools::operator=(Pools&&) noexcept = default;

// --- Setup ---------------------------------------------------------------

Pools Setup::run(Role role, Channel& ch, const Params& params, PoolSizes sizes) {
    // Setup's own use of `params` beyond the fixed R-SIG signature: cross-
    // checks ztgate_pipeline's pinned digit-layout constants against
    // Params' single authority, catching constant drift between the two
    // sources (task-5-report.md/task-14-report.md's documented duplication
    // risk) before generating a single gate.
    SYMPSICA_REQUIRE(zt::kNumDigits == Params::C && zt::kDigitBits == Params::W &&
                          zt::kTopDigitBits == Params::TOP_DIGIT_BITS,
                      "Setup::run: ztgate pipeline digit-layout constants drifted from Params");

    Pools pools;
    pools.ot_state = std::make_unique<detail::SetupOtState>();

    // R-PKOP: base OTs run EXACTLY ONCE per Pools lifetime, right here.
    macoro::sync_wait(detail::run_base_ots(role, *pools.ot_state, ch.socket()));

    generate_triples(pools, role, *pools.ot_state, ch.socket(), sizes.triples);
    generate_gates(pools, role, *pools.ot_state, ch.socket(), sizes.gates);

    return pools;
}

void Setup::refill_offline(Pools& pools, Role role, Channel& ch, const Params& params,
                            PoolSizes targets) {
    (void)params; // fixed by R-SIG; refill needs no additional Params check beyond run()'s own.
    SYMPSICA_REQUIRE(pools.ot_state != nullptr,
                      "Setup::refill_offline: pools were not produced by Setup::run");

    std::size_t triples_have = pools.triples.remaining();
    std::size_t gates_have = pools.gates.remaining();
    std::size_t triples_needed = triples_have >= targets.triples ? 0 : targets.triples - triples_have;
    std::size_t gates_needed = gates_have >= targets.gates ? 0 : targets.gates - gates_have;

    // R-PKOP: refill_offline NEVER calls run_base_ots -- every fill below
    // reuses pools.ot_state's already-established base OTs via
    // detail::fill_ot_pool (silent extension only).
    generate_triples(pools, role, *pools.ot_state, ch.socket(), triples_needed);
    generate_gates(pools, role, *pools.ot_state, ch.socket(), gates_needed);
}

} // namespace sympsica
