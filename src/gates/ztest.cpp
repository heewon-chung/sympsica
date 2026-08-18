#include "sympsica/gates/ztest.hpp"

#include "libOTe/Dpf/RegularDpf.h"

#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/common.hpp"
#include "sympsica/utils/serdes.hpp"

namespace sympsica {

namespace {

namespace oc = osuCrypto;

// R is party 0 and S is party 1 throughout (same convention as
// ztgate_pipeline.hpp::party_idx and RegularDpf's own partyIdx
// parameter) -- production's own copy, since src/ cannot depend on
// test/integration/.
u64 dpf_party_idx(Role role) { return role == Role::Receiver ? 0 : 1; }

} // namespace

void ZeroTest::check_canonical_entry(Share D, const ZtGate& gate) {
    SYMPSICA_REQUIRE(D.v.v < Fp::P,
                     "ZeroTest: non-canonical D share at entry (FT3 guard, R-TVF3)");
    SYMPSICA_REQUIRE(gate.mask_m.v.v < Fp::P,
                     "ZeroTest: non-canonical gate mask share at entry (FT3 guard, R-TVF3)");
}

std::array<u64, ZeroTest::kNumDigits> ZeroTest::digit_split(Fp z) {
    SYMPSICA_REQUIRE(z.v < Fp::P, "ZeroTest::digit_split: non-canonical z");
    u64 v = z.v;
    std::array<u64, kNumDigits> d{};
    for (u64 j = 0; j + 1 < kNumDigits; ++j)
        d[j] = (v >> (kDigitBits * j)) & ((u64(1) << kDigitBits) - 1);
    d[kNumDigits - 1] = (v >> (kDigitBits * (kNumDigits - 1))) & ((u64(1) << kTopDigitBits) - 1);
    return d;
}

std::vector<Fp> ZeroTest::open_masked(std::span<const Share> D, std::span<const ZtGate> gates,
                                       Channel& ch) {
    SYMPSICA_REQUIRE(D.size() == gates.size(), "ZeroTest::open_masked: D/gates size mismatch");
    const std::size_t n = D.size();

    std::vector<Fp> mine(n);
    for (std::size_t i = 0; i < n; ++i) {
        check_canonical_entry(D[i], gates[i]);
        mine[i] = D[i].v.add(gates[i].mask_m.v);
    }

    std::vector<u8> out_buf(n * 8);
    std::span<u8> out_span(out_buf);
    for (std::size_t i = 0; i < n; ++i) write_fp(out_span.subspan(i * 8, 8), mine[i]);
    ch.send(out_buf);

    std::vector<u8> in_buf(n * 8);
    ch.recv(in_buf);
    std::span<const u8> in_span(in_buf);

    std::vector<Fp> z(n);
    for (std::size_t i = 0; i < n; ++i) {
        Fp theirs = read_fp(in_span.subspan(i * 8, 8));
        z[i] = mine[i].add(theirs);
    }
    return z;
}

std::array<Share, ZeroTest::kNumDigits> ZeroTest::eval_local_rhos(Fp z, const ZtGate& gate,
                                                                   Role role) {
    const auto digits = digit_split(z);
    CoeffCtxFp61 ctx;

    std::array<Share, kNumDigits> rhos{};
    std::array<bool, kNumDigits> found{};

    // Full-domain expand of gate.key (see this class's header comment for
    // why -- libOTe exposes no single-point eval), keeping only the leaf
    // at each tree's target digit. RegularDpf::expand's key parameter is
    // non-const in the vendored API even though it never writes through
    // it (implExpand only READS inputKey's correction words/bits/
    // mLeafVals on this code path -- verified by inspection of
    // vendor/libOTe/libOTe/Dpf/RegularDpf.h's implExpand body) -- the
    // const_cast below is the same "read-only despite the signature"
    // pattern test/integration/w24_boundary_masks.cpp's expand_both()
    // relies on implicitly (there via a non-const reference parameter).
    auto sink = [&](auto k, auto i, auto&& v, auto /*tag*/) {
        const auto tree = static_cast<u64>(k);
        const auto leaf = static_cast<u64>(i);
        if (tree < kNumDigits && leaf == digits[tree]) {
            SYMPSICA_REQUIRE(v.v < Fp::P,
                             "ZeroTest::eval_local_rhos: non-canonical DPF leaf share");
            rhos[tree] = Share{v};
            found[tree] = true;
        }
    };
    oc::RegularDpf<Fp, CoeffCtxFp61>::expand(dpf_party_idx(role), kDomain,
                                              const_cast<oc::RegularDpfKey&>(gate.key), sink, ctx);

    for (u64 k = 0; k < kNumDigits; ++k)
        SYMPSICA_REQUIRE(found[k], "ZeroTest::eval_local_rhos: target digit leaf never visited");
    return rhos;
}

std::vector<Share> ZeroTest::recombine(std::span<const std::array<Share, kNumDigits>> rhos,
                                        BeaverEngine& engine, TriplePool& pool, Channel& ch) {
    const std::size_t g = rhos.size();

    // Round B: {rho0*rho1, rho2*rho3} for every gate, ONE batched call.
    std::vector<Share> xB(2 * g), yB(2 * g);
    for (std::size_t i = 0; i < g; ++i) {
        xB[2 * i + 0] = rhos[i][0];
        yB[2 * i + 0] = rhos[i][1];
        xB[2 * i + 1] = rhos[i][2];
        yB[2 * i + 1] = rhos[i][3];
    }
    std::vector<Share> p = engine.mul(xB, yB, pool, ch);
    SYMPSICA_REQUIRE(p.size() == 2 * g, "ZeroTest::recombine: layer-B mul returned wrong count");

    // Round C: the final product, every gate, ONE batched call.
    std::vector<Share> xC(g), yC(g);
    for (std::size_t i = 0; i < g; ++i) {
        xC[i] = p[2 * i + 0];
        yC[i] = p[2 * i + 1];
    }
    std::vector<Share> b = engine.mul(xC, yC, pool, ch);
    SYMPSICA_REQUIRE(b.size() == g, "ZeroTest::recombine: layer-C mul returned wrong count");
    return b;
}

Share ZeroTest::eval(Share D, const ZtGate& gate, BeaverEngine& engine, TriplePool& pool,
                      Channel& ch) {
    std::array<Share, 1> Ds{D};
    std::array<ZtGate, 1> gates{gate};
    std::vector<Fp> z = open_masked(Ds, gates, ch);                    // round A

    std::array<std::array<Share, kNumDigits>, 1> rhos{
        eval_local_rhos(z[0], gate, engine.role())};                   // local
    std::vector<Share> b = recombine(rhos, engine, pool, ch);          // rounds B, C
    return b[0];
}

} // namespace sympsica
