#include "sympsica/protocols/detail/vole_beaver.hpp"

#include <array>

#include "libOTe/Tools/CoeffCtx.h"
#include "libOTe/Vole/Noisy/NoisyVoleReceiver.h"
#include "libOTe/Vole/Noisy/NoisyVoleSender.h"

#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/common.hpp"

namespace sympsica::vole {

namespace {

// Read `choices`' kOlePerCallOts bits into an integer and reduce to Fp. See
// the header comment: this IS the Sender's delta for the call this OtSlice
// was taken for — NoisyVoleSender::send internally recomputes
// ctx.binaryDecomposition(delta) and must land on exactly these bits, so
// delta cannot be chosen independently of them.
Fp delta_from_choices(const oc::BitVector& choices) {
    SYMPSICA_REQUIRE(choices.size() == kOlePerCallOts,
                     "delta_from_choices: OtSlice is not a 61-OT slice");
    oc::u64 raw = 0;
    for (oc::u64 i = 0; i < kOlePerCallOts; ++i)
        raw |= (oc::u64)(bool)choices[i] << i;
    // raw ranges over [0, 2^61); the single excluded pattern (all ones) is
    // exactly Fp::P — the same "the one rejected value" shape as
    // ztgate_pipeline's mask rejection (2^-61 probability), but here there is
    // no resample loop to fall back into (delta is fixed by the OTs already
    // consumed), so this is an abort rather than a silent wraparound.
    SYMPSICA_REQUIRE(raw != Fp::P,
                     "delta_from_choices: OT choice pattern hit the one non-canonical "
                     "61-bit value (probability 2^-61 per call); rerun with a different seed");
    return Fp(raw);
}

} // namespace

macoro::task<> ole_receive(oc::span<const Fp> x, std::vector<Fp>& u, oc::PRNG& prng,
                            const OtSlice& ot, coproto::Socket& sock) {
    SYMPSICA_REQUIRE(ot.send.size() == kOlePerCallOts,
                     "ole_receive: OtSlice is not a 61-OT slice");
    sympsica::CoeffCtxFp61 ctx;
    std::vector<std::array<oc::block, 2>> otMsg(ot.send.begin(), ot.send.end());
    std::vector<Fp> c(x.begin(), x.end());
    u.assign(x.size(), Fp(0));

    co_await oc::NoisyVoleReceiver<Fp, Fp, sympsica::CoeffCtxFp61>::receive(c, u, prng, otMsg,
                                                                             sock, ctx);

    for (const auto& s : u)
        SYMPSICA_REQUIRE(s.v < Fp::P, "ole_receive: non-canonical share");
}

macoro::task<> ole_send(Fp& y, std::vector<Fp>& v, oc::u64 n, oc::PRNG& prng, const OtSlice& ot,
                         coproto::Socket& sock, bool negate) {
    SYMPSICA_REQUIRE(ot.recv.size() == kOlePerCallOts,
                     "ole_send: OtSlice is not a 61-OT slice");
    sympsica::CoeffCtxFp61 ctx;
    y = delta_from_choices(ot.choices);

    std::vector<oc::block> otMsg(ot.recv.begin(), ot.recv.end());
    std::vector<Fp> b(n, Fp(0));

    co_await oc::NoisyVoleSender<Fp, Fp, sympsica::CoeffCtxFp61>::send(y, b, prng, otMsg, sock,
                                                                        ctx);

    v.resize(n);
    for (oc::u64 j = 0; j < n; ++j) {
        SYMPSICA_REQUIRE(b[j].v < Fp::P, "ole_send: non-canonical share");
        v[j] = negate ? b[j].neg() : b[j];
    }
}

macoro::task<> beaver_triples(Role role, oc::u64 n, oc::PRNG& prng, ztgate::OtPool& pool,
                               coproto::Socket& sock, BeaverBatch& out,
                               bool corrupt_ct1_sign) {
    out.a.assign(n, Fp(0));
    out.b.assign(n, Fp(0));
    out.c.assign(n, Fp(0));
    if (n == 0)
        co_return;

    // Controller ruling (task-6 fix round 1): each triple gets its own pair
    // of length-1 NoisyVole calls (2 * kOlePerCallOts OTs), so every entry
    // of a/b/c below is independent per triple on BOTH parties' sides — no
    // batch-wide sharing of any secret. See vole_beaver.hpp's header comment
    // for why this precludes the earlier single-batched-call design.
    if (role == Role::Receiver) {
        for (oc::u64 i = 0; i < n; ++i) {
            // R's own per-triple secrets: independent, freely sampled.
            Fp a_R = Fp::from_u64(prng.get<oc::u64>());
            Fp b_R = Fp::from_u64(prng.get<oc::u64>());
            out.a[i] = a_R;
            out.b[i] = b_R;

            std::vector<Fp> x_a{a_R};
            std::vector<Fp> x_b{b_R};

            // cross term 1: u1 + v1 == a_R[i] * b_S[i]   (R supplies x = a_R[i])
            auto ot1 = pool.take(kOlePerCallOts);
            std::vector<Fp> u1;
            co_await ole_receive(x_a, u1, prng, ot1, sock);

            // cross term 2: u2 + v2 == b_R[i] * a_S[i]   (R supplies x = b_R[i])
            auto ot2 = pool.take(kOlePerCallOts);
            std::vector<Fp> u2;
            co_await ole_receive(x_b, u2, prng, ot2, sock);

            out.c[i] = a_R.mul(b_R).add(u1[0]).add(u2[0]);
        }
    } else {
        for (oc::u64 i = 0; i < n; ++i) {
            Fp b_S, a_S;
            std::vector<Fp> v1, v2;

            auto ot1 = pool.take(kOlePerCallOts);
            co_await ole_send(b_S, v1, 1, prng, ot1, sock, /*negate=*/!corrupt_ct1_sign);

            auto ot2 = pool.take(kOlePerCallOts);
            co_await ole_send(a_S, v2, 1, prng, ot2, sock, /*negate=*/true);

            out.a[i] = a_S;
            out.b[i] = b_S;
            out.c[i] = a_S.mul(b_S).add(v1[0]).add(v2[0]);
        }
    }
}

} // namespace sympsica::vole
