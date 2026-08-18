#include "sympsica/core/share.hpp"

#include <array>

#include "sympsica/utils/serdes.hpp"

namespace sympsica {

Share affine(Fp a, Share x, Fp b) {
    return Share{a.mul(x.v).add(b)};
}

Fp open(Channel& ch, Share mine) {
    std::array<u8, 8> out_buf{};
    write_fp(out_buf, mine.v);
    ch.send(out_buf);

    std::array<u8, 8> in_buf{};
    ch.recv(in_buf);
    Fp theirs = read_fp(in_buf);

    return mine.v.add(theirs);
}

} // namespace sympsica
