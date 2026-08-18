#ifndef SYMPSICA_GATES_SYMDIFF_HPP
#define SYMPSICA_GATES_SYMDIFF_HPP

#include <array>
#include <span>
#include <vector>

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/gates/beaver.hpp"
#include "sympsica/utils/net.hpp"

namespace sympsica {

// SymDiffEvaluator — the 8-round layer-batched symmetric-difference
// evaluator (task-14-brief.md, W4.4): given a batch of buckets' syndrome
// shares (d1..d7 per bucket), recovers each bucket's rank t (Remark 1,
// t = max{tau in 1..4 : D_tau != 0}, else 0) AS A FRESH SHARE, entirely
// under MPC, in EXACTLY 8 communication rounds no matter how many buckets
// are in the batch (task-14-brief.md's binding "TOTAL = exactly 8 rounds
// for any batch size >= 1").
//
// Round map (plan W4.4, verbatim numbering; see eval_buckets's body for
// the corresponding code section):
//   1: minors layer 1 (20 products/bucket), batched across every bucket.
//   2: minors layer 2 (9 products/bucket), batched across every bucket.
//   3: gate opening (ZeroTest::open_masked), batched across every
//      bucket's 4 gates (tau = 1..4, one gate per minor D_tau).
//   4: ZT recombination layer B, batched across every bucket's 4 gates.
//   5: ZT recombination layer C, batched across every bucket's 4 gates.
//   6: s_3 = b_3 * s_4, batched across every bucket.
//   7: s_2 = b_2 * s_3, batched across every bucket.
//   8: s_1 = b_1 * s_2, batched across every bucket.
// Local (no round): every per-bucket linear combo (minors' T/B/A3/B3/C3
// terms, D3/D4 from layer 2's outputs) and the final affine step
// t_beta_share = 4*nu - (s_1+s_2+s_3+s_4), nu = 1 for Receiver's own
// local share only (0 for Sender) -- summing both parties' shares gives
// 4 - sum(s_tau), which recovers t exactly (task-14-report.md has the
// derivation: s_tau reconstructs to [D_tau == D_(tau+1) == ... == D_4 ==
// 0], so sum(s_tau) over tau=1..4 equals 4 - t).
//
// Per-bucket cost (task-14-brief.md W4.5): 44 triples (29 minors + 12 ZT
// recombination + 3 suffix), 4 fresh ZtGates, 184 field elements /
// 1472 bytes of wire (summed over BOTH parties' outgoing traffic).
//
// DESIGN-DOC DELTA (R-EVAL-SHAPE, task-14-brief.md, binding ruling): the
// design doc names PartyState& as the syndrome source; Phase 4 has no
// Query wiring to produce that, so this signature instead takes
// `span<const array<Share,7>>` (one pre-shared syndrome vector per
// bucket) directly -- the MPC-correct interface. Phase 5's Query is
// expected to build these shares and call this signature.
//
// `betas` carries J-ordering/padding semantics (R-EVAL-SHAPE): it is NOT
// otherwise consumed by the arithmetic here (a beta paired with an
// all-zero-SHARE syndrome row is the padding case, and behaves
// identically to a genuine empty-bucket t=0 row -- see
// task-14-report.md/SD-1). PRECONDITION: betas.size() == syndromes.size().
class SymDiffEvaluator {
public:
    static std::vector<Share> eval_buckets(std::span<const u32> betas,
                                            std::span<const std::array<Share, 7>> syndromes,
                                            BeaverEngine& engine, TriplePool& pool,
                                            ZtGatePool& zt_pool, Channel& ch);
};

} // namespace sympsica

#endif // SYMPSICA_GATES_SYMDIFF_HPP
