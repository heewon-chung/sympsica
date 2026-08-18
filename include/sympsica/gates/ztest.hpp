#ifndef SYMPSICA_GATES_ZTEST_HPP
#define SYMPSICA_GATES_ZTEST_HPP

#include <array>
#include <span>
#include <vector>

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/gates/beaver.hpp"
#include "sympsica/utils/net.hpp"

namespace sympsica {

// ZeroTest — the masked-opening zero test over DPF digit comparison
// (task-14-brief.md, W4.3): given a Share D and a fresh ZtGate (an
// additive Share of a 61-bit mask r plus a single multi-point RegularDpf
// key over that mask's 4 canonical digits, core/pools.hpp), decides
// [D == 0] without revealing D, returning a fresh Share of the 0/1
// result.
//
// NOTE (design-doc delta, task-5-report.md's ZtGate revision): the plan's
// W4.3 sketch shows `keys[4]` (one key per digit); core/pools.hpp's ZtGate
// (authoritative, per task-14-brief.md's binding note) instead carries ONE
// multi-point RegularDpfKey covering all 4 digit trees. DPF_Eval(key_i,
// z_i) below means "evaluate tree i of that one multi-point key at digit
// i's query point" -- see eval_local_rhos.
//
// DPF-eval decision: libOTe's RegularDpf (vendor/libOTe/libOTe/Dpf/
// RegularDpf.h) exposes only two evaluation entry points -- the
// interactive `keyGen` (used once, offline, by the Phase-2 pipeline to
// PRODUCE a gate) and the non-interactive, FULL-DOMAIN static `expand`.
// There is no single-point/point-only eval anywhere in the vendored API
// (confirmed by reading RegularDpf.h in full while implementing this
// class -- also the reading task-5/task-8 already reached, see
// ztgate_pipeline.hpp's and w24_boundary_masks.cpp's own notes on this).
// eval_local_rhos therefore performs ONE full-domain expand of the gate's
// key per call (4 trees x 2^16 leaves) and keeps only the 4 leaves whose
// index matches the opened value's own digit split -- the cheapest
// CORRECT option the API surface offers, used identically on both the
// test and production paths (task-14-brief.md's explicit allowance).
//
// Communication shape (batched, so this composes with gates/symdiff.hpp's
// 8-total-round requirement): every method below that touches the
// network operates on a SPAN of independent (D, gate) pairs / (rho)
// tuples and performs exactly ONE round no matter how many entries the
// span holds -- open_masked is round A (batched across every bucket's
// four gates), recombine is rounds B and C (two Beaver-mul layers,
// likewise fully batched). ZeroTest::eval composes all three for exactly
// ONE gate (3 rounds total) -- the convenience form ZT-1/ZT-2 use
// directly; gates/symdiff.cpp instead calls open_masked/eval_local_rhos/
// recombine itself so rounds A/B/C are shared across an entire batch of
// buckets.
class ZeroTest {
public:
    static constexpr u64 kDigitBits = 16;
    static constexpr u64 kTopDigitBits = 13;  // top digit: 61 - 3*16 = 13 bits
    static constexpr u64 kNumDigits = 4;
    static constexpr u64 kDomain = u64(1) << kDigitBits;

    // FT3 canonicality guard (task-14-brief.md R-TVF3), checked BEFORE any
    // Fp arithmetic combines D and the gate's mask: Fp::add (field.hpp)
    // assumes both operands are already canonical and, given a raw
    // Fp{Fp::P} operand, its single-fold reduction silently collapses the
    // sum down to the OTHER operand instead of rejecting it -- a silent
    // WRONG answer, not a crash, if this guard were skipped. Exposed as
    // its own static so TV-F3 can exercise it directly (no live Channel /
    // no EXPECT_DEATH-after-fork hazard -- see task-14-report.md).
    static void check_canonical_entry(Share D, const ZtGate& gate);

    // Carry-less little-endian 16-bit digit split of a canonical z, same
    // convention as ztgate_pipeline.hpp's digit_split (production cannot
    // depend on test/integration/, so this is production's own copy --
    // task-14-report.md documents the duplication). digits[0..2] are 16
    // bits wide, digits[3] is the narrow 13-bit top digit.
    static std::array<u64, kNumDigits> digit_split(Fp z);

    // Round A: batched masked opening of z_i = D_i + gates[i].mask_m for
    // every i in [0, D.size()); D.size() must equal gates.size(). ONE
    // round (one send + one recv of D.size() field elements/party) no
    // matter how many entries are batched. NOT core/share.hpp's reveal
    // helper (R-GREP reserves that call site for the final output opening
    // and tests): z is a fresh masked opening (r is single-use, sampled
    // independently per gate by the offline pipeline), so this
    // implements its own wire packing directly via serdes, mirroring
    // gates/beaver.hpp's BeaverEngine::mul.
    static std::vector<Fp> open_masked(std::span<const Share> D, std::span<const ZtGate> gates,
                                        Channel& ch);

    // Local only (no communication): for one already-opened z and its
    // gate, evaluates the four DPF trees at z's four digits, returning
    // this party's Share of [z_digit_i == r_digit_i] for i in 0..3 (the
    // "rho" values, plan W4.3).
    static std::array<Share, kNumDigits> eval_local_rhos(Fp z, const ZtGate& gate, Role role);

    // Rounds B, C: batched over rhos.size() independent gates. For gate
    // g, b_g = (rho[g][0]*rho[g][1]) * (rho[g][2]*rho[g][3]) via 3 Beaver
    // mults, with layer B ({rho0*rho1, rho2*rho3}, EVERY gate) batched
    // into ONE mul() call (round B) and layer C (the final product,
    // EVERY gate) batched into a second ONE mul() call (round C) -- so
    // this costs exactly 2 rounds and 3*rhos.size() triples regardless of
    // rhos.size().
    static std::vector<Share> recombine(std::span<const std::array<Share, kNumDigits>> rhos,
                                         BeaverEngine& engine, TriplePool& pool, Channel& ch);

    // Single-gate convenience: rounds A, B, C (open_masked + eval_local_rhos
    // + recombine) for exactly one (D, gate) pair -- 3 rounds, 3 triples
    // total. Used directly by ZT-1/ZT-2; gates/symdiff.cpp instead
    // composes the three primitives above itself so A/B/C are shared
    // across a whole batch of buckets (see gates/symdiff.hpp).
    static Share eval(Share D, const ZtGate& gate, BeaverEngine& engine, TriplePool& pool,
                       Channel& ch);
};

} // namespace sympsica

#endif // SYMPSICA_GATES_ZTEST_HPP
