#include "sympsica/gates/minors.hpp"

#include <array>
#include <vector>

#include "sympsica/utils/common.hpp"

namespace sympsica {

namespace {

// Local-only Share linear combination (no communication): both parties
// apply the same operation to their own shares, so the sum reconstructs
// the difference/sum of the underlying secrets.
Share sub(Share a, Share b) { return Share{a.v.sub(b.v)}; }
Share add(Share a, Share b) { return Share{a.v.add(b.v)}; }

} // namespace

std::array<Share, 4> MinorCircuit::eval(std::span<const Share> d, BeaverEngine& engine,
                                         TriplePool& pool, Channel& ch) {
    SYMPSICA_REQUIRE(d.size() == 7, "MinorCircuit::eval: d must have exactly 7 entries (d1..d7)");

    // --- LAYER 1: 20 products, ONE batched round -----------------------
    constexpr std::size_t kLayer1 = 20;
    constexpr std::size_t kLayer2 = 9;
    static_assert(kLayer1 == 20, "MinorCircuit layer-1 schedule size pin");
    static_assert(kLayer2 == 9, "MinorCircuit layer-2 schedule size pin");
    static_assert(kLayer1 + kLayer2 == 29, "MinorCircuit mu_impl pin (29 secure mults)");

    // d-index (0-based) for each layer-1 operand, in m[0..19] order
    // (verbatim W4.2 schedule; see class doc comment).
    constexpr std::array<std::size_t, kLayer1> kXi{0, 1, 0, 1, 0, 1, 2, 1, 2, 2,
                                                     3, 2, 3, 2, 3, 4, 3, 4, 4, 5};
    constexpr std::array<std::size_t, kLayer1> kYi{2, 1, 3, 2, 4, 3, 2, 4, 3, 4,
                                                     3, 5, 4, 6, 5, 4, 6, 5, 6, 5};

    std::vector<Share> x1(kLayer1), y1(kLayer1);
    for (std::size_t i = 0; i < kLayer1; ++i) {
        x1[i] = d[kXi[i]];
        y1[i] = d[kYi[i]];
    }
    std::vector<Share> m = engine.mul(x1, y1, pool, ch);
    SYMPSICA_REQUIRE(m.size() == kLayer1, "MinorCircuit::eval: layer-1 mul returned wrong count");

    // --- local linear combos (no communication) -------------------------
    Share T12 = sub(m[0], m[1]);
    Share T13 = sub(m[2], m[3]);
    Share T14 = sub(m[4], m[5]);
    Share T23 = sub(m[5], m[6]);
    Share T24 = sub(m[7], m[8]);
    Share T34 = sub(m[9], m[10]);
    Share B12 = sub(m[9], m[10]);
    Share B13 = sub(m[11], m[12]);
    Share B14 = sub(m[13], m[14]);
    Share B23 = sub(m[14], m[15]);
    Share B24 = sub(m[16], m[17]);
    Share B34 = sub(m[18], m[19]);
    Share A3 = T34, B3 = T24, C3 = T23;

    Share D1 = d[0];
    Share D2 = T12;

    // --- LAYER 2: 9 products, ONE batched round --------------------------
    std::vector<Share> x2{d[0], d[1], d[2], T12, T13, T14, T23, T24, T34};
    std::vector<Share> y2{A3, B3, C3, B34, B24, B23, B14, B13, B12};
    std::vector<Share> n = engine.mul(x2, y2, pool, ch);
    SYMPSICA_REQUIRE(n.size() == kLayer2, "MinorCircuit::eval: layer-2 mul returned wrong count");

    Share D3 = add(sub(n[0], n[1]), n[2]);
    Share D4 = add(sub(add(add(sub(n[3], n[4]), n[5]), n[6]), n[7]), n[8]);

    return {D1, D2, D3, D4};
}

u64 MinorCircuit::t_of(const std::array<Fp, 4>& D) {
    u64 t = 0;
    for (u64 tau = 1; tau <= 4; ++tau) {
        if (!D[tau - 1].is_zero()) t = tau;
    }
    return t;
}

} // namespace sympsica
