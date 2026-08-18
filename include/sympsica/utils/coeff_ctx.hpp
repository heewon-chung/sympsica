#ifndef SYMPSICA_UTILS_COEFF_CTX_HPP
#define SYMPSICA_UTILS_COEFF_CTX_HPP

// CoeffCtxFp61 — adapter satisfying libOTe's CoeffCtx concept (RegularDpf +
// NoisyVole instantiation) for sympsica's Fp (p = 2^61-1). Modeled line-by-line
// on libOTe's CoeffCtxFp (vendor/libOTe/libOTe/Tools/Field/Fp.h), with the
// arithmetic swapped from Fp<mod,T,TT>'s operator+/-/* to our Fp::add/sub/mul
// (task-2 brief, W1.2 — see task-2-report.md for the full interface-delta
// note: our Fp exposes named methods, not operators, so unlike the vendored
// CoeffCtxFp, this adapter must override plus/minus/mul explicitly rather
// than inherit CoeffCtxInteger's generic `ret = lhs + rhs` versions).
//
// Deliberately split out of field.hpp: this header hard-includes
// libOTe/Tools/CoeffCtx.h, which transitively pulls in
// libOTe/Vole/Noisy/NoisyVoleSender.h and a slice of cryptoTools (BitVector,
// PRNG, Timer, Network/Channel) — real compile weight that every consumer of
// Fp (encoding.hpp, params.hpp, serdes.hpp, ...) should not have to pay for.
// Only include this header where the CoeffCtx adapter is actually needed
// (Phase 2+: RegularDpf / NoisyVole instantiation).
//
// Requires the extra include directories wired into CMakeLists.txt for the
// vendored libOTe/cryptoTools headers AND their CMake-generated config.h
// files (see PINS.md) — those config.h files already exist on disk as a
// side effect of Phase 0's documented `python3 build.py --setup` re-vendor
// step (libOTe's own CMake configure runs during that step even though the
// build itself is skipped by --setup).
#include "libOTe/Tools/CoeffCtx.h"

#include <cstddef>
#include <iterator>
#include <type_traits>

#include "sympsica/utils/common.hpp"
#include "sympsica/utils/field.hpp"

namespace sympsica {

struct CoeffCtxFp61 : osuCrypto::CoeffCtxInteger {
    // is G a field? Always true for Fp.
    template<typename G>
    bool isField() const { return true; }

    // is G characteristic 2 (x + x == 0)? Never, for Fp with odd prime P.
    template<typename G>
    bool characteristicTwo() const { return false; }

    // the bit size required to represent F; RegularDpf/NoisyVole perform a
    // binary decomposition of F using this many bits.
    template<typename F>
    osuCrypto::u64 bitSize() const { return 61; }

    // binary decomposition of x, little-endian over bitSize<F>() = 61 bits.
    // Same memcpy-based BitVector view as CoeffCtxInteger's generic version
    // (valid because Fp is trivially copyable and little-endian on every
    // host this project targets); declared explicitly here per W1.2 rather
    // than left to base-class inheritance, for interface-completeness with
    // the vendored CoeffCtxFp this mirrors.
    template<typename F>
    osuCrypto::BitVector binaryDecomposition(F& x) const {
        static_assert(std::is_trivially_copyable<F>::value,
                      "memcpy is used so must be trivially_copyable.");
        return { reinterpret_cast<osuCrypto::u8*>(&x), bitSize<F>() };
    }

    // Derive an Fp from 64 bits of randomness via Fp::from_u64's Mersenne
    // fold. NOTE (documented per W1.2): this folds the low 64 bits of `b`
    // mod P; values in [0, 2^64 mod P) = [0, 8) are reachable one more way
    // than the rest of [0, P) (an |8|/2^64 statistical bias) — negligible for
    // this PoC.
    template<typename F>
    void fromBlock(F& ret, const osuCrypto::block& b) const {
        ret = Fp::from_u64(b.template get<osuCrypto::u64>(0));
    }

    // plus/minus/mul mapped to Fp ops (W1.2). Forwarding-reference signature
    // matches CoeffCtxInteger's own generic plus/minus/mul so this safely
    // substitutes wherever libOTe's generic VOLE code calls
    // ctx.plus(ret, lhs, rhs) etc. — including through proxy/reference types,
    // not just plain Fp&.
    template<typename R, typename F1, typename F2>
    void plus(R&& ret, F1&& lhs, F2&& rhs) const { ret = lhs.add(rhs); }

    template<typename R, typename F1, typename F2>
    void minus(R&& ret, F1&& lhs, F2&& rhs) const { ret = lhs.sub(rhs); }

    template<typename R, typename F1, typename F2>
    void mul(R&& ret, F1&& lhs, F2&& rhs) const { ret = lhs.mul(rhs); }

    // mask(): intentionally NOT overridden. Unlike the vendored CoeffCtxFp
    // (which specializes via FpTraits<G>::value_type for a wider AND),
    // CoeffCtxInteger's generic mask() already handles Fp correctly: Fp is
    // 8 bytes (not a multiple of sizeof(block) == 16), so it takes the
    // generic byte-oriented branch (AND each byte against mask.get<u8>(0)).
    // Correct, just not micro-optimized — acceptable for this PoC.

    // deserialize()/serialize(): OVERRIDDEN (task-4 brief, obligation (d) —
    // CoeffCtx deserialize canonicality audit). Base class
    // CoeffCtxInteger::deserialize (vendor/libOTe/libOTe/Tools/CoeffCtx.h) is
    // a raw std::memcpy with NO range/reduction check whatsoever. The audit
    // (task-4-report.md has the full trace) found three call sites in
    // vendor/libOTe that route raw wire bytes through ctx.deserialize()
    // straight into an F (= Fp here) value:
    //   - NoisyVoleSender::send / NoisyVoleReceiver::recv (Vole/Noisy/*.h):
    //     `co_await chl.recv(buffer); ctx.deserialize(buffer..., msg.begin())`
    //     — the encrypted-share buffer arrives directly off the network.
    //   - RegularDpf::implExpand (Dpf/RegularDpf.h): both the interactively
    //     revealed `gamma` (`co_await sock.recv(buffer); ctx.deserialize(...)`)
    //     and a DPF key's `mLeafVals` loaded via RegularDpfKey::fromBytes
    //     (`ctx.deserialize(inputKey->mLeafVals..., gamma.begin())`) —
    //     the latter is disk/wire-loaded key material, not itself
    //     range-checked by fromBytes (which only copies raw bytes into the
    //     mLeafVals u8 buffer; the F-typed decode happens at this
    //     deserialize() call).
    // Every one of those call sites goes through THIS ctx object, so
    // overriding deserialize() here — not touching vendor/libOTe — closes
    // the gap: any non-canonical (>= P) 64-bit pattern crossing the wire
    // aborts via SYMPSICA_REQUIRE instead of silently becoming a Fp that
    // violates field.hpp's stated invariant ("every Fp value produced by
    // this class is the canonical representative in [0, P)"). ctx.fromBlock
    // (used pervasively elsewhere in RegularDpf/NoisyVole for PRG-derived
    // shares) is already safe: it goes through our fromBlock() override
    // above, which reduces via Fp::from_u64's Mersenne fold. Tasks 5-6: call
    // Fp values through this ctx (not a raw memcpy) on every value that has
    // crossed the wire, precisely so this check fires.
    template<typename SrcIter, typename DstIter>
    void deserialize(SrcIter&& begin, SrcIter&& end, DstIter&& dstBegin) const {
        osuCrypto::CoeffCtxInteger::deserialize(begin, end, dstBegin);

        using SrcType = std::remove_reference_t<decltype(*begin)>;
        using DstType = std::remove_reference_t<decltype(*dstBegin)>;
        if constexpr (std::is_same_v<DstType, Fp>) {
            auto srcN = std::distance(begin, end);
            auto dstN = static_cast<std::size_t>(srcN) * sizeof(SrcType) / sizeof(DstType);
            auto it = dstBegin;
            for (std::size_t i = 0; i < dstN; ++i, ++it) {
                SYMPSICA_REQUIRE((*it).v < Fp::P,
                    "CoeffCtxFp61::deserialize: non-canonical Fp value (>= P) crossed the wire");
            }
        }
    }

    template<typename SrcIter, typename DstIter>
    void serialize(SrcIter&& begin, SrcIter&& end, DstIter&& dstBegin) const {
        using SrcType = std::remove_reference_t<decltype(*begin)>;
        if constexpr (std::is_same_v<SrcType, Fp>) {
            for (auto it = begin; it != end; ++it) {
                SYMPSICA_REQUIRE((*it).v < Fp::P,
                    "CoeffCtxFp61::serialize: refusing to serialize a non-canonical Fp value");
            }
        }
        osuCrypto::CoeffCtxInteger::serialize(begin, end, dstBegin);
    }
};

} // namespace sympsica

#endif // SYMPSICA_UTILS_COEFF_CTX_HPP
