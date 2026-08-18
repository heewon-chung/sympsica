// task-4 brief, obligation (c) SANITY: after wiring libOTe as a real linked
// CMake dependency, a TU constructing oc::RegularDpf-related headers and
// oc::NoisyVoleSender/Receiver with our CoeffCtxFp61 must COMPILE (and
// link). This is a compile/link-only probe — full protocol runs are Tasks
// 5-6's job; it exists to prove the real build (not the Phase-1 header-only
// include-path shim) actually produces usable, linkable symbols.
#include <gtest/gtest.h>

#include "libOTe/Dpf/RegularDpf.h"
#include "libOTe/Vole/Noisy/NoisyVoleReceiver.h"
#include "libOTe/Vole/Noisy/NoisyVoleSender.h"

#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/field.hpp"

using namespace sympsica;

TEST(LibOTeLinkProbe, RegularDpfConstructsWithCoeffCtxFp61) {
    osuCrypto::RegularDpf<Fp, CoeffCtxFp61> dpf;
    EXPECT_EQ(dpf.mPartyIdx, 0u);
    EXPECT_FALSE(dpf.hasBaseOts());
}

TEST(LibOTeLinkProbe, NoisyVoleTypesAreNameableWithCoeffCtxFp61) {
    // NoisyVoleSender/Receiver's send()/recv() are templates only
    // instantiated when actually called (Tasks 5-6's job); this probe just
    // needs the types to be complete/nameable under our CoeffCtx adapter,
    // which exercises the same libOTe headers (CoeffCtx.h, BitVector.h,
    // Coproto.h) RegularDpf pulls in.
    using Sender = osuCrypto::NoisyVoleSender<Fp, Fp, CoeffCtxFp61>;
    using Receiver = osuCrypto::NoisyVoleReceiver<Fp, Fp, CoeffCtxFp61>;
    EXPECT_GT(sizeof(Sender) + sizeof(Receiver), 0u);
}
