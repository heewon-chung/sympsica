#ifndef SYMPSICA_TEST_UTILS_AUDIT_POOL_TRANSCRIPT_HPP
#define SYMPSICA_TEST_UTILS_AUDIT_POOL_TRANSCRIPT_HPP

// test/utils/audit_pool_transcript.hpp — Task 23 (W6.4/W6.6(ii)):
// full-transcript id-based single-use pool audit (W3.4 semantics,
// R6-AUDIT-TRANSCRIPT), built ENTIRELY on primitives core/pools.hpp's
// CorrelationPool<T> and protocols/setup.hpp's Pools ALREADY expose --
// generated(), consumed_ids(), remaining(), Pools::next_corr_id -- this
// task adds NO pool machinery anywhere; this header is the audit built on
// top of those existing primitives.
//
// Takes RAW values, not a CorrelationPool<T>& or a Pools&, so a
// deliberately-miscounted FIXTURE can drive it directly without touching
// real pool internals (R6-NOTAUTO's FC2: "a test-only manipulation, or a
// deliberately miscounted fixture").
//
// Asserts, in order (SYMPSICA_REQUIRE -- aborts, naming the pool and the
// concrete numbers; test/protocols/kat_pool_audit.cpp's FC1/FC2 demonstrate
// the abort paths):
//   (1) generated == consumed_ids.size() + remaining      [reconciliation]
//   (2) consumed_ids has no duplicates                    [single-use]
//   (3) every id in consumed_ids is < next_corr_id         [no id from nowhere]
//
// Property (4) of R6-AUDIT-TRANSCRIPT ("ids stay globally unique across
// refills") is NOT independently re-verified here -- and CANNOT be, from
// this function's inputs alone. (2) only inspects the CONSUMED subset; a
// duplicate corr_id among two items that were both GENERATED but NEVER
// consumed (unused high-water stock, which W3.4 explicitly makes legal)
// would never appear in consumed_ids at all, so a duplicate confined to
// that generated-but-unconsumed set is structurally invisible to (1)-(3).
// CorrelationPool<T>'s public API has no accessor for the full generated-id
// SET (only generated(), a plain count, and consumed_ids(), the consumed
// subset) -- so no audit built on these primitives, however constructed,
// could re-derive generation-side uniqueness from outside the pool.
//
// Property (4) is instead enforced as a PRECONDITION, at insertion time, by
// CorrelationPool<T>::refill itself (src/core/pools.cpp, the `for (auto&
// item : items)` loop): each item's corr_id is checked against BOTH
// available_ and consumed_set_ -- `!available_.count(id) &&
// !consumed_set_.count(id)` -- and inserted into available_ immediately
// afterward, all within the SAME loop iteration, so this catches a
// duplicate corr_id within one refill() BATCH (the second occurrence sees
// the first already in available_) exactly as it catches one against an
// EARLIER refill() call or an already-consumed id -- SYMPSICA_REQUIRE
// aborts either way, before the duplicate item is ever added. This audit
// therefore RELIES ON that enforcement (if it is running at all, no
// refill() call has ever violated it, or the process would already have
// aborted) rather than re-verifying it after the fact; the three checks
// below cover exactly the three things this function's inputs CAN observe.
//
// High-water items (generated but never consumed, remaining() > 0 at the
// end) are LEGAL by design (W3.4) and never fail any of the three checks --
// kat_pool_audit.cpp's FC4 demonstrates this directly.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "sympsica/utils/common.hpp"
#include "sympsica/utils/field.hpp" // u64

namespace sympsica::test {

inline void audit_pool_transcript(const std::string& pool_name, u64 generated,
                                   const std::vector<u64>& consumed_ids, u64 remaining,
                                   u64 next_corr_id) {
    // (1) reconciliation.
    const u64 accounted = static_cast<u64>(consumed_ids.size()) + remaining;
    if (generated != accounted) {
        const std::string msg = "pool audit [" + pool_name + "]: reconciliation failed: generated=" +
                                 std::to_string(generated) +
                                 " consumed=" + std::to_string(consumed_ids.size()) +
                                 " remaining=" + std::to_string(remaining) +
                                 " (consumed+remaining=" + std::to_string(accounted) + ")";
        SYMPSICA_REQUIRE(false, msg.c_str());
    }

    // (2) no duplicates among CONSUMED ids across the whole transcript,
    // i.e. across every refill (consumed_ids never resets between refills).
    // NOT property (4) -- see file header: this only covers the consumed
    // subset, not generated-but-unconsumed stock.
    const std::unordered_set<u64> seen(consumed_ids.begin(), consumed_ids.end());
    if (seen.size() != consumed_ids.size()) {
        const std::string msg = "pool audit [" + pool_name + "]: duplicate consumed id(s): " +
                                 std::to_string(consumed_ids.size()) +
                                 " consumed entries but only " + std::to_string(seen.size()) +
                                 " distinct ids";
        SYMPSICA_REQUIRE(false, msg.c_str());
    }

    // (3) no id from nowhere: every consumed id must lie inside the range
    // this Pools' corr_id sequence has actually assigned so far.
    for (u64 id : consumed_ids) {
        if (id >= next_corr_id) {
            const std::string msg = "pool audit [" + pool_name + "]: consumed id " + std::to_string(id) +
                                     " was never generated (>= next_corr_id=" +
                                     std::to_string(next_corr_id) + ")";
            SYMPSICA_REQUIRE(false, msg.c_str());
        }
    }
}

} // namespace sympsica::test

#endif // SYMPSICA_TEST_UTILS_AUDIT_POOL_TRANSCRIPT_HPP
