#ifndef SYMPSICA_PROTOCOLS_SETUP_HPP
#define SYMPSICA_PROTOCOLS_SETUP_HPP

#include <cstddef>
#include <memory>

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"

namespace sympsica {

// PkOpCounter — process-wide, monotonic, thread-safe counter of "public-key"
// (base-OT) protocol EXECUTIONS this project's own code has explicitly run
// (task-16-brief.md R-PKOP, claim-B gate W6.5). Incremented ONLY at
// protocols/detail/setup_ot.hpp's run_base_ots() call site -- never by
// counting anything inside vendored libOTe (whose SimplestOT internals are
// read-only; see that header's own comment). Setup::run calls run_base_ots
// exactly once per Pools lifetime; refill_offline never calls it, so the
// counter is asserted CONSTANT across every refill/query after Setup (SC5).
// Shared by both parties' threads when a test runs them in one process (the
// project's own two-party test pattern), so it observes BOTH sides' base-OT
// executions collectively -- SC5/FC3 only ever need relative change, never
// a per-party absolute value.
struct PkOpCounter {
    static u64 value();
    static void increment();
};

// PoolSizes / Pools — R-SIG (controller ruling, binding): supersedes the
// design doc's `pair<Params, Pools>` return; Params stays
// Params::instantiate()-owned, untouched by Setup.
struct PoolSizes {
    std::size_t triples = 0;
    std::size_t gates = 0;
};

namespace detail {
struct SetupOtState; // opaque here; fully defined in protocols/detail/setup_ot.hpp
} // namespace detail

struct Pools {
    TriplePool triples;
    ZtGatePool gates;

    // R-CORRID: running counter continued by refill_offline, so corr_ids
    // stay globally unique across the WHOLE Pools lifetime including every
    // refill (CorrelationPool::refill's cross-refill validation is the
    // enforcement backstop, not this counter -- see pools.hpp).
    u64 next_corr_id = 0;

    // R-CONT (R-SIG's "+ continuation state"): the persistent silent-OT
    // extension machinery (base OTs already run, R-PKOP) that
    // refill_offline reuses so base OTs never run a second time on this
    // Pools. Pimpl'd (net.hpp's Channel precedent) so libOTe's Silent-OT
    // headers stay out of this public header. Null only before Setup::run
    // has produced this Pools.
    std::unique_ptr<detail::SetupOtState> ot_state;

    Pools();
    ~Pools();
    Pools(Pools&&) noexcept;
    Pools& operator=(Pools&&) noexcept;
    Pools(const Pools&) = delete;
    Pools& operator=(const Pools&) = delete;
};

// Setup — Protocol W5.1 (REVISED per review finding 1): the Phase-2 W2.1
// (ztgate) / W2.2 (vole/Beaver) pipelines, productionized. run() performs
// the ONE-TIME base-OT setup (Chou-Orlandi/SimplestOT, kappa=128, R-PKOP,
// via protocols/detail/setup_ot.hpp) and then fills `sizes.triples` Triples
// and `sizes.gates` ZtGates NATIVELY into a fresh Pools -- corr_id assigned
// at generation, core::Triple/core::ZtGate produced directly (R-UNIFY).
// refill_offline() tops each pool up to `targets`, generating
// max(0, target - remaining()) new items per pool, from silent OT extension
// ONLY (zero base OTs, R-PKOP). Both generate in batches of at most 512
// items (R-BATCH, the W2.4 GO decision's batch size).
//
// R-METER: this traffic flows over Channel::socket() directly (same as the
// Phase-2 pipelines it productionizes), NOT through Channel::send()/recv(),
// so Channel::bytes_sent()/sends_count() do NOT observe it -- offline byte
// counts must be read from coproto::Socket's own bytesSent()/bytesReceived()
// counters (see setup.cpp's measurement call sites for the "offline vs
// online" scope comment R-METER requires).
class Setup {
public:
    static Pools run(Role role, Channel& ch, const Params& params, PoolSizes sizes);

    static void refill_offline(Pools& pools, Role role, Channel& ch, const Params& params,
                                PoolSizes targets);
};

} // namespace sympsica

#endif // SYMPSICA_PROTOCOLS_SETUP_HPP
