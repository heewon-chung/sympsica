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

// d-index (0-based) for each layer-1 operand, in m[0..19] order (verbatim
// W4.2 schedule; see class doc comment in minors.hpp).
constexpr std::size_t kLayer1 = 20;
constexpr std::size_t kLayer2 = 9;
static_assert(kLayer1 == 20, "MinorCircuit layer-1 schedule size pin");
static_assert(kLayer2 == 9, "MinorCircuit layer-2 schedule size pin");
static_assert(kLayer1 + kLayer2 == 29, "MinorCircuit mu_impl pin (29 secure mults/bucket)");

constexpr std::array<std::size_t, kLayer1> kXi{0, 1, 0, 1, 0, 1, 2, 1, 2, 2,
                                                 3, 2, 3, 2, 3, 4, 3, 4, 4, 5};
constexpr std::array<std::size_t, kLayer1> kYi{2, 1, 3, 2, 4, 3, 2, 4, 3, 4,
                                                 3, 5, 4, 6, 5, 4, 6, 5, 6, 5};

} // namespace

std::vector<std::array<Share, 4>> MinorCircuit::eval_batch(
    std::span<const std::array<Share, 7>> d_batch, BeaverEngine& engine, TriplePool& pool,
    Channel& ch) {
    const std::size_t B = d_batch.size();

    // --- LAYER 1: 20*B products, ONE batched round, ALL buckets ---------
    std::vector<Share> x1(kLayer1 * B), y1(kLayer1 * B);
    for (std::size_t b = 0; b < B; ++b) {
        for (std::size_t i = 0; i < kLayer1; ++i) {
            x1[b * kLayer1 + i] = d_batch[b][kXi[i]];
            y1[b * kLayer1 + i] = d_batch[b][kYi[i]];
        }
    }
    std::vector<Share> m = engine.mul(x1, y1, pool, ch);
    SYMPSICA_REQUIRE(m.size() == kLayer1 * B,
                     "MinorCircuit::eval_batch: layer-1 mul returned wrong count");

    // --- local linear combos per bucket (no communication) --------------
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

        D1[b] = d_batch[b][0];
        D2[b] = T12;

        x2[b * kLayer2 + 0] = d_batch[b][0];
        y2[b * kLayer2 + 0] = A3;
        x2[b * kLayer2 + 1] = d_batch[b][1];
        y2[b * kLayer2 + 1] = B3;
        x2[b * kLayer2 + 2] = d_batch[b][2];
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

    // --- LAYER 2: 9*B products, ONE batched round, ALL buckets ----------
    std::vector<Share> n = engine.mul(x2, y2, pool, ch);
    SYMPSICA_REQUIRE(n.size() == kLayer2 * B,
                     "MinorCircuit::eval_batch: layer-2 mul returned wrong count");

    std::vector<std::array<Share, 4>> out(B);
    for (std::size_t b = 0; b < B; ++b) {
        auto nb = [&](std::size_t i) { return n[b * kLayer2 + i]; };
        Share D3 = add(sub(nb(0), nb(1)), nb(2));
        Share D4 = add(sub(add(add(sub(nb(3), nb(4)), nb(5)), nb(6)), nb(7)), nb(8));
        out[b] = {D1[b], D2[b], D3, D4};
    }
    return out;
}

std::array<Share, 4> MinorCircuit::eval(std::span<const Share> d, BeaverEngine& engine,
                                         TriplePool& pool, Channel& ch) {
    SYMPSICA_REQUIRE(d.size() == 7, "MinorCircuit::eval: d must have exactly 7 entries (d1..d7)");

    std::array<std::array<Share, 7>, 1> batch;
    for (std::size_t k = 0; k < 7; ++k) batch[0][k] = d[k];

    auto out = eval_batch(batch, engine, pool, ch);
    return out[0];
}

u64 MinorCircuit::t_of(const std::array<Fp, 4>& D) {
    u64 t = 0;
    for (u64 tau = 1; tau <= 4; ++tau) {
        if (!D[tau - 1].is_zero()) t = tau;
    }
    return t;
}

} // namespace sympsica
