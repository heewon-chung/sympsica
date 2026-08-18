#include "sympsica/gates/beaver.hpp"

#include "sympsica/utils/common.hpp"
#include "sympsica/utils/serdes.hpp"

namespace sympsica {

std::vector<Share> BeaverEngine::mul(std::span<const Share> x, std::span<const Share> y,
                                      TriplePool& pool, Channel& ch) {
    SYMPSICA_REQUIRE(x.size() == y.size(), "BeaverEngine::mul: x/y size mismatch");
    const std::size_t n = x.size();

    std::vector<Triple> triples;
    triples.reserve(n);
    for (std::size_t i = 0; i < n; ++i) triples.push_back(pool.take());

    // Local shares of d_i = x_i - a_i, e_i = y_i - b_i (no communication).
    std::vector<Fp> d_mine(n), e_mine(n);
    for (std::size_t i = 0; i < n; ++i) {
        d_mine[i] = x[i].v.sub(triples[i].a.v);
        e_mine[i] = y[i].v.sub(triples[i].b.v);
    }

    // ONE round: pack both vectors into a single outgoing buffer (d's
    // followed by e's), send, then receive the peer's matching buffer.
    std::vector<u8> out_buf(2 * n * 8);
    std::span<u8> out_span(out_buf);
    for (std::size_t i = 0; i < n; ++i) write_fp(out_span.subspan(i * 8, 8), d_mine[i]);
    for (std::size_t i = 0; i < n; ++i) write_fp(out_span.subspan((n + i) * 8, 8), e_mine[i]);
    ch.send(out_buf);

    std::vector<u8> in_buf(2 * n * 8);
    ch.recv(in_buf);
    std::span<const u8> in_span(in_buf);

    std::vector<Share> z(n);
    for (std::size_t i = 0; i < n; ++i) {
        Fp d_theirs = read_fp(in_span.subspan(i * 8, 8));
        Fp e_theirs = read_fp(in_span.subspan((n + i) * 8, 8));
        Fp d = d_mine[i].add(d_theirs);
        Fp e = e_mine[i].add(e_theirs);

        Fp z_i = triples[i].c.v.add(d.mul(triples[i].b.v)).add(e.mul(triples[i].a.v));
        if (role_ == Role::Receiver) z_i = z_i.add(d.mul(e));
        z[i] = Share{z_i};
    }
    return z;
}

} // namespace sympsica
