#ifndef SYMPSICA_CORE_POOLS_HPP
#define SYMPSICA_CORE_POOLS_HPP

#include <cstddef>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "libOTe/Dpf/RegularDpf.h"

#include "sympsica/core/share.hpp"
#include "sympsica/utils/field.hpp"

namespace sympsica {

namespace oc = osuCrypto;

// Production correlation structs (REVISED per review finding 6; R2, design
// §pools.hpp). Both carry an immutable `corr_id`, assigned at generation
// time (Phase 5's Setup, out of scope here) -- see CorrelationPool below.
struct Triple {
    u64 corr_id;
    Share a, b, c;
};

struct ZtGate {
    u64 corr_id;
    Share mask_m;      // additive Fp share, from B2A
    oc::RegularDpfKey key; // ONE multi-point key, numPoints=4, domain 2^16
};

// ---------------------------------------------------------------------------
// R3 (Phase-3 entry obligation (a)): TriplePool and ZtGatePool, below, NEVER
// top up implicitly -- take()/take_by_id() on an id that is not currently
// available (exhausted pool, or an id that was never refilled) aborts via
// SYMPSICA_REQUIRE. Every consumer of TriplePool and ZtGatePool MUST size
// its pool with rejection slack (the extra_rejections pattern used by
// test/integration's offline pipelines, e.g.
// ztgate_pipeline.hpp::PipelineOpts::max_resample_rounds) rather than
// relying on an implicit refill; Phase-5 Setup owns the actual
// refill/continuation design (this pool only enforces the invariant).
//
// Entry obligation (e) landing spot: the offline correlation generation
// that will eventually fill these pools (Phase 5 Setup) runs its silent OT
// extension at SemiHonest security -- libOTe's own default (see
// CMakeLists.txt's ENABLE_SILENTOT / ENABLE_SILENT_VOLE knobs; no malicious
// hardening is applied on top of it in this PoC).
// ---------------------------------------------------------------------------

namespace detail {

// CorrelationPool<T> — identity-based single-use pool shared by TriplePool
// and ZtGatePool (W3.4, REVISED per review finding 6: identity-based
// single-use enforcement, NOT counters). Every T carries an immutable
// T::corr_id; take() atomically transitions an id available -> consumed and
// returns the correlation; a SECOND transition of the same id aborts
// (SYMPSICA_REQUIRE) -- TV-F10 exercises exactly this via take_by_id().
// Unused high-water items (generated but never taken) are legal: this
// pool's own bookkeeping always satisfies generated() == consumed_ids()
// .size() + remaining() by construction (every state transition is
// available->consumed, tracked in both directions).
//
// T must expose a public `u64 corr_id` member.
template <typename T>
class CorrelationPool {
public:
    // Next available correlation, FIFO by refill() order. Aborts if the
    // pool is empty (R3: no implicit top-up).
    T take();

    // Test hook (TV-F10): consumes the correlation with this specific
    // corr_id, regardless of FIFO position. Aborts if `corr_id` is unknown
    // OR already consumed (the double-consume case TV-F10 exercises).
    T take_by_id(u64 corr_id);

    std::size_t remaining() const { return order_.size(); }

    // Adds `items` to the pool's available set. Validates that every
    // corr_id is unique across the pool's whole lifetime (never previously
    // refilled, available or consumed) -- corr_ids are ASSIGNED at
    // generation time upstream (Phase 5 Setup), not by this pool; refill()
    // only validates them.
    void refill(std::vector<T>&& items);

    // Consumed-id transcript, in consumption order (FT5 audits).
    const std::vector<u64>& consumed_ids() const { return consumed_log_; }

    // Total items ever added via refill().
    u64 generated() const { return generated_; }

    // debug_last_consumed() — task-24-brief.md W6.6(iv)/R6-DPFKEY/SC7 test
    // hook: the pool's OWN residual copy of the most recently
    // take()n/take_by_id()n item, AFTER take()/take_by_id() have run their
    // zeroization step on it (see pools.cpp). For ZtGatePool this is what a
    // test inspects to confirm the DPF key was actually scrubbed from the
    // pool's own storage, not merely handed to the caller unmodified --
    // T = Triple never zeroizes anything (Triple carries no key material),
    // so this is just whatever the ordinary take() left behind there. The
    // accessor exists uniformly on the shared template (rather than only on
    // ZtGatePool) to avoid a template specialization just for one debug
    // hook; it costs one extra T-sized member on TriplePool too, which is
    // cheap (Triple has no key).
    const T& debug_last_consumed() const { return last_consumed_; }

private:
    std::list<u64> order_; // FIFO order of available corr_ids
    std::unordered_map<u64, std::pair<T, typename std::list<u64>::iterator>> available_;
    std::unordered_set<u64> consumed_set_;
    std::vector<u64> consumed_log_;
    u64 generated_ = 0;
    T last_consumed_{}; // see debug_last_consumed() above
};

} // namespace detail

class TriplePool : public detail::CorrelationPool<Triple> {};
class ZtGatePool : public detail::CorrelationPool<ZtGate> {};

} // namespace sympsica

#endif // SYMPSICA_CORE_POOLS_HPP
