#include "sympsica/gates/symdiff.hpp"

#include <array>
#include <vector>

#include "sympsica/gates/ztest.hpp"
#include "sympsica/utils/common.hpp"

namespace sympsica {

namespace {

// Layer-1/2 index schedule -- COPIED VERBATIM from src/gates/minors.cpp's
// kXi/kYi (task-13-brief.md W4.2's m[0..19] schedule). DEVIATION, disclosed
// (task-14-report.md): gates/minors.hpp's MinorCircuit::eval is documented
// as "the SINGLE SOURCE OF TRUTH for the schedule ... never reimplement
// the schedule elsewhere", but its per-call API computes ONE bucket's
// D1..D4 in 2 rounds -- calling it once per bucket would cost 2*B rounds,
// not the 2 total rounds W4.4's 8-round batching requires. task-14-brief.md
// requirement 1's file list does not include gates/minors.hpp, so rather
// than change MinorCircuit's API this file re-hosts the same schedule,
// batched across buckets, and cross-checks it directly against
// MinorCircuit::eval per-bucket in
// GatesSymdiff.BatchedMinorsMatchMinorCircuitPerBucket (test/gates/
// kat_symdiff.cpp) so the two can never silently diverge.
constexpr std::size_t kLayer1 = 20;
constexpr std::size_t kLayer2 = 9;
constexpr std::array<std::size_t, kLayer1> kXi{0, 1, 0, 1, 0, 1, 2, 1, 2, 2,
                                                 3, 2, 3, 2, 3, 4, 3, 4, 4, 5};
constexpr std::array<std::size_t, kLayer1> kYi{2, 1, 3, 2, 4, 3, 2, 4, 3, 4,
                                                 3, 5, 4, 6, 5, 4, 6, 5, 6, 5};

Share sub(Share a, Share b) { return Share{a.v.sub(b.v)}; }
Share add(Share a, Share b) { return Share{a.v.add(b.v)}; }

} // namespace

std::vector<Share> SymDiffEvaluator::eval_buckets(std::span<const u32> betas,
                                                    std::span<const std::array<Share, 7>> syndromes,
                                                    BeaverEngine& engine, TriplePool& pool,
                                                    ZtGatePool& zt_pool, Channel& ch) {
    SYMPSICA_REQUIRE(betas.size() == syndromes.size(),
                     "SymDiffEvaluator::eval_buckets: betas/syndromes size mismatch");
    const std::size_t B = syndromes.size();
    const Role role = engine.role();

    // --- ROUND 1: minors layer 1, batched across ALL B buckets ---------
    std::vector<Share> x1(kLayer1 * B), y1(kLayer1 * B);
    for (std::size_t b = 0; b < B; ++b) {
        for (std::size_t i = 0; i < kLayer1; ++i) {
            x1[b * kLayer1 + i] = syndromes[b][kXi[i]];
            y1[b * kLayer1 + i] = syndromes[b][kYi[i]];
        }
    }
    std::vector<Share> m = engine.mul(x1, y1, pool, ch);
    SYMPSICA_REQUIRE(m.size() == kLayer1 * B, "eval_buckets: layer-1 mul returned wrong count");

    // local per-bucket combos -> D1, D2 and layer-2 inputs (no round)
    std::vector<Share> D1(B), D2(B);
    std::vector<Share> x2(kLayer2 * B), y2(kLayer2 * B);
    for (std::size_t b = 0; b < B; ++b) {
        auto mb = [&](std::size_t i) { return m[b * kLayer1 + i]; };
        Share T12 = sub(mb(0), mb(1));
        Share T13 = sub(mb(2), mb(3));
        Share T14 = sub(mb(4), mb(5));
        Share T23 = sub(mb(5), mb(6));
        Share T24 = sub(mb(7), mb(8));
        Share T34 = sub(mb(9), mb(10));
        Share B12 = sub(mb(9), mb(10));
        Share B13 = sub(mb(11), mb(12));
        Share B14 = sub(mb(13), mb(14));
        Share B23 = sub(mb(14), mb(15));
        Share B24 = sub(mb(16), mb(17));
        Share B34 = sub(mb(18), mb(19));
        Share A3 = T34, B3 = T24, C3 = T23;

        D1[b] = syndromes[b][0];
        D2[b] = T12;

        x2[b * kLayer2 + 0] = syndromes[b][0];
        y2[b * kLayer2 + 0] = A3;
        x2[b * kLayer2 + 1] = syndromes[b][1];
        y2[b * kLayer2 + 1] = B3;
        x2[b * kLayer2 + 2] = syndromes[b][2];
        y2[b * kLayer2 + 2] = C3;
        x2[b * kLayer2 + 3] = T12;
        y2[b * kLayer2 + 3] = B34;
        x2[b * kLayer2 + 4] = T13;
        y2[b * kLayer2 + 4] = B24;
        x2[b * kLayer2 + 5] = T14;
        y2[b * kLayer2 + 5] = B23;
        x2[b * kLayer2 + 6] = T23;
        y2[b * kLayer2 + 6] = B14;
        x2[b * kLayer2 + 7] = T24;
        y2[b * kLayer2 + 7] = B13;
        x2[b * kLayer2 + 8] = T34;
        y2[b * kLayer2 + 8] = B12;
    }

    // --- ROUND 2: minors layer 2, batched -------------------------------
    std::vector<Share> n = engine.mul(x2, y2, pool, ch);
    SYMPSICA_REQUIRE(n.size() == kLayer2 * B, "eval_buckets: layer-2 mul returned wrong count");

    std::vector<Share> D3(B), D4(B);
    for (std::size_t b = 0; b < B; ++b) {
        auto nb = [&](std::size_t i) { return n[b * kLayer2 + i]; };
        D3[b] = add(sub(nb(0), nb(1)), nb(2));
        D4[b] = add(sub(add(add(sub(nb(3), nb(4)), nb(5)), nb(6)), nb(7)), nb(8));
    }

    // --- ROUND 3: gate opening, batched across ALL buckets' 4 gates ----
    std::vector<ZtGate> gates(4 * B);
    for (std::size_t g = 0; g < 4 * B; ++g) gates[g] = zt_pool.take();

    std::vector<Share> Ds(4 * B);
    for (std::size_t b = 0; b < B; ++b) {
        Ds[b * 4 + 0] = D1[b];
        Ds[b * 4 + 1] = D2[b];
        Ds[b * 4 + 2] = D3[b];
        Ds[b * 4 + 3] = D4[b];
    }
    std::vector<Fp> z = ZeroTest::open_masked(Ds, gates, ch);

    // local: digit split + DPF eval for every gate (no round)
    std::vector<std::array<Share, 4>> rhos(4 * B);
    for (std::size_t g = 0; g < 4 * B; ++g)
        rhos[g] = ZeroTest::eval_local_rhos(z[g], gates[g], role);

    // --- ROUNDS 4, 5: ZT recombination, batched -------------------------
    std::vector<Share> b_tau = ZeroTest::recombine(rhos, engine, pool, ch);
    SYMPSICA_REQUIRE(b_tau.size() == 4 * B, "eval_buckets: recombine returned wrong count");

    std::vector<Share> b1(B), b2(B), b3(B), s4(B);
    for (std::size_t b = 0; b < B; ++b) {
        b1[b] = b_tau[b * 4 + 0];
        b2[b] = b_tau[b * 4 + 1];
        b3[b] = b_tau[b * 4 + 2];
        s4[b] = b_tau[b * 4 + 3];
    }

    // --- ROUNDS 6, 7, 8: suffix products, batched across buckets -------
    std::vector<Share> s3 = engine.mul(b3, s4, pool, ch);
    std::vector<Share> s2 = engine.mul(b2, s3, pool, ch);
    std::vector<Share> s1 = engine.mul(b1, s2, pool, ch);

    // local: t_beta share = 4*nu - (s1+s2+s3+s4); only the Receiver's own
    // local share includes the public 4 (nu = 1 Receiver, 0 Sender) --
    // see this header's doc comment for the reconstruction derivation.
    std::vector<Share> t(B);
    const Fp four_nu = (role == Role::Receiver) ? Fp(4) : Fp(0);
    for (std::size_t b = 0; b < B; ++b) {
        Fp sum = s1[b].v.add(s2[b].v).add(s3[b].v).add(s4[b].v);
        t[b] = Share{four_nu.sub(sum)};
    }
    return t;
}

} // namespace sympsica
