#include "sympsica/gates/symdiff.hpp"

#include <array>
#include <vector>

#include "sympsica/gates/minors.hpp"
#include "sympsica/gates/ztest.hpp"
#include "sympsica/utils/common.hpp"

namespace sympsica {

std::vector<Share> SymDiffEvaluator::eval_buckets(std::span<const u32> betas,
                                                    std::span<const std::array<Share, 7>> syndromes,
                                                    BeaverEngine& engine, TriplePool& pool,
                                                    ZtGatePool& zt_pool, Channel& ch) {
    SYMPSICA_REQUIRE(betas.size() == syndromes.size(),
                     "SymDiffEvaluator::eval_buckets: betas/syndromes size mismatch");
    const std::size_t B = syndromes.size();
    const Role role = engine.role();

    // --- ROUNDS 1, 2: minors, batched across ALL B buckets --------------
    // Controller ruling (task-14-report.md addendum): the m-index schedule
    // has exactly ONE implementation, MinorCircuit::eval_batch
    // (gates/minors.hpp/.cpp) -- it is NOT re-hosted here. eval_batch runs
    // the schedule ONCE, batched across every bucket in `syndromes`, in
    // exactly 2 rounds (round 1 = layer 1, round 2 = layer 2) regardless
    // of B, which is exactly what this evaluator's fixed 8-round budget
    // requires.
    std::vector<std::array<Share, 4>> D = MinorCircuit::eval_batch(syndromes, engine, pool, ch);
    SYMPSICA_REQUIRE(D.size() == B, "eval_buckets: MinorCircuit::eval_batch returned wrong count");

    // --- ROUND 3: gate opening, batched across ALL buckets' 4 gates ----
    std::vector<ZtGate> gates(4 * B);
    for (std::size_t g = 0; g < 4 * B; ++g) gates[g] = zt_pool.take();

    std::vector<Share> Ds(4 * B);
    for (std::size_t b = 0; b < B; ++b) {
        Ds[b * 4 + 0] = D[b][0];
        Ds[b * 4 + 1] = D[b][1];
        Ds[b * 4 + 2] = D[b][2];
        Ds[b * 4 + 3] = D[b][3];
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
