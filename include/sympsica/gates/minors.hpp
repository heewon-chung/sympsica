#ifndef SYMPSICA_GATES_MINORS_HPP
#define SYMPSICA_GATES_MINORS_HPP

#include <array>
#include <span>
#include <vector>

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/gates/beaver.hpp"
#include "sympsica/utils/net.hpp"

namespace sympsica {

// MinorCircuit — the hand-optimized cofactor circuit computing D_1..D_4
// (the leading principal minors of the 4x4 Hankel matrix H[i][j] = d_{i+j-1}
// built from the depth-7 syndrome vector d1..d7) from Shares (task-13
// brief, W4.2). This is the SINGLE SOURCE OF TRUTH for the schedule
// (design doc, gates/minors.hpp): the main protocol path, any microbench,
// and every KAT call exactly this -- never reimplement the schedule
// elsewhere.
//
// EXACT schedule (plan text W4.2, verbatim; cross-checked against
// ref/reference.py's Ref.minors(), the independently brute-force-verified
// golden -- see task-13-report.md for the cross-check evidence):
//
//   LAYER 1 (ONE BeaverEngine::mul batch, 20 products PER BUCKET):
//     m[0]=d1*d3  m[1]=d2*d2  m[2]=d1*d4  m[3]=d2*d3  m[4]=d1*d5  m[5]=d2*d4
//     m[6]=d3*d3  m[7]=d2*d5  m[8]=d3*d4  m[9]=d3*d5  m[10]=d4*d4
//     m[11]=d3*d6 m[12]=d4*d5 m[13]=d3*d7 m[14]=d4*d6 m[15]=d5*d5
//     m[16]=d4*d7 m[17]=d5*d6 m[18]=d5*d7 m[19]=d6*d6
//   Local linear combos (no communication):
//     D1 = d1; D2 = m0 - m1
//     T12=m0-m1  T13=m2-m3  T14=m4-m5  T23=m5-m6  T24=m7-m8  T34=m9-m10
//     B12=m9-m10 B13=m11-m12 B14=m13-m14 B23=m14-m15 B24=m16-m17 B34=m18-m19
//     A3=T34 (=m9-m10)  B3=T24 (=m7-m8)  C3=T23 (=m5-m6)
//   LAYER 2 (ONE BeaverEngine::mul batch, 9 products PER BUCKET):
//     n[0]=d1*A3  n[1]=d2*B3  n[2]=d3*C3
//     n[3]=T12*B34 n[4]=T13*B24 n[5]=T14*B23 n[6]=T23*B14 n[7]=T24*B13
//     n[8]=T34*B12
//   Outputs: D3 = n0 - n1 + n2;  D4 = n3 - n4 + n5 + n6 - n7 + n8.
//
// mu_impl = 29 secure multiplications PER BUCKET (20 + 9), <= paper's bound
// mu(4) <= 40; consumes exactly 29 triples PER BUCKET and 2 communication
// rounds TOTAL, no matter how many buckets are batched (controller ruling,
// task-14-report.md addendum): eval_batch below is the schedule's ONE
// implementation -- it runs the SAME m/T/B/n index arithmetic ONCE, batched
// across every bucket in its input span (20*B products in layer 1, 9*B in
// layer 2, still exactly 2 rounds). eval() is a thin wrapper calling
// eval_batch with a batch of one; gates/symdiff.cpp (Phase 4b) calls
// eval_batch directly so its own minors rounds stay fixed at 2 regardless
// of batch size.
class MinorCircuit {
public:
    // Batched entry point (controller ruling, task-14-report.md addendum;
    // design doc's single-source-of-truth requirement): computes {D1,D2,
    // D3,D4} for EVERY bucket in d_batch simultaneously, in exactly 2
    // communication rounds (ONE BeaverEngine::mul call per layer, spanning
    // ALL buckets) and 29*d_batch.size() triples, regardless of
    // d_batch.size(). Output[i] corresponds to d_batch[i].
    static std::vector<std::array<Share, 4>> eval_batch(
        std::span<const std::array<Share, 7>> d_batch, BeaverEngine& engine, TriplePool& pool,
        Channel& ch);

    // d must have exactly 7 entries (d1..d7, C++ index 0..6). Returns
    // {D1,D2,D3,D4} as fresh Shares. Thin wrapper over eval_batch (a batch
    // of one) -- kept for the many single-bucket call sites (MIN-1..5's
    // KATs, the real-VOLE composition test) that predate the batched API.
    static std::array<Share, 4> eval(std::span<const Share> d, BeaverEngine& engine,
                                      TriplePool& pool, Channel& ch);

    // t_of — Remark 1's rank-recovery decision rule (TV-F7, binding):
    // t = max{tau in 1..4 : D[tau-1] != 0}, else 0 -- NEVER the first-zero
    // index (MIN-3 pins exactly this: an interior D_tau can vanish even
    // when the true t is larger). Operates on OPENED (plaintext) minors --
    // the real protocol's secure decision is gates/ztest.hpp's job (Phase
    // 4b, out of scope here); this plaintext helper is for tests and
    // reference-cross-checks. The design doc does not name a home for this
    // rule, so it lives here per task-13-brief.md requirement 2's
    // fallback ("if the design places recovery elsewhere, put the helper
    // in minors.hpp as a static").
    static u64 t_of(const std::array<Fp, 4>& D);
};

} // namespace sympsica

#endif // SYMPSICA_GATES_MINORS_HPP
