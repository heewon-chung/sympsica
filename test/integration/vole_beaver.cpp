#include "vole_beaver.hpp"

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

    if (role == Role::Receiver) {
        // R's own per-triple secrets: independent, freely sampled. This is
        // the half of the batch that NoisyVole's single-delta-per-call shape
        // leaves genuinely independent (see the header comment and the
        // report's "OLE call granularity" section).
        for (oc::u64 i = 0; i < n; ++i) {
            out.a[i] = Fp::from_u64(prng.get<oc::u64>());
            out.b[i] = Fp::from_u64(prng.get<oc::u64>());
        }

        // cross term 1: u1[i] + v1[i] == a_R[i] * b_S   (R supplies x = a_R)
        auto ot1 = pool.take(kOlePerCallOts);
        std::vector<Fp> u1;
        co_await ole_receive(out.a, u1, prng, ot1, sock);

        // cross term 2: u2[i] + v2[i] == b_R[i] * a_S   (R supplies x = b_R)
        auto ot2 = pool.take(kOlePerCallOts);
        std::vector<Fp> u2;
        co_await ole_receive(out.b, u2, prng, ot2, sock);

        for (oc::u64 i = 0; i < n; ++i)
            out.c[i] = out.a[i].mul(out.b[i]).add(u1[i]).add(u2[i]);
    } else {
        Fp b_S, a_S;
        std::vector<Fp> v1, v2;

        auto ot1 = pool.take(kOlePerCallOts);
        co_await ole_send(b_S, v1, n, prng, ot1, sock, /*negate=*/!corrupt_ct1_sign);

        auto ot2 = pool.take(kOlePerCallOts);
        co_await ole_send(a_S, v2, n, prng, ot2, sock, /*negate=*/true);

        // S's per-triple secrets are the SAME scalar for the whole batch —
        // the direct consequence of NoisyVole's single delta per call (see
        // header comment). Broadcast so out.a/out.b still carry one entry
        // per triple for callers that reconstruct a[i]/b[i] per triple.
        Fp ab_S = a_S.mul(b_S);
        for (oc::u64 i = 0; i < n; ++i) {
            out.a[i] = a_S;
            out.b[i] = b_S;
            out.c[i] = ab_S.add(v1[i]).add(v2[i]);
        }
    }
}

} // namespace sympsica::vole
