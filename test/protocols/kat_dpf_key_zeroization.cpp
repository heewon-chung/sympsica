// test/protocols/kat_dpf_key_zeroization.cpp — task-24-brief.md W6.6(iv)/
// R6-DPFKEY (SC7): "keys ... zeroized on consume" -- proven by directly
// inspecting the pool's own storage after a take(), via
// CorrelationPool<T>::debug_last_consumed() (pools.hpp), not inferred.
//
// FC5 [zeroization non-vacuity] is demonstrated OUT-OF-BAND (task-24-
// report.md): git-stash the zeroize_dpf_key() call sites in
// src/core/pools.cpp, rebuild, observe THIS test fail for real, restore,
// observe it pass again (the Phase-5 R-OCCUPIED precedent for proving a
// regression guard is genuine) -- not encoded as a second, always-compiled
// negative test here, since that would just be this same test run against
// a deliberately broken build, which is exactly what a git-stash
// demonstration already is.

#include <gtest/gtest.h>

#include "libOTe/Dpf/RegularDpf.h"

#include "sympsica/core/pools.hpp"
#include "sympsica/core/share.hpp"
#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/field.hpp"

namespace {

namespace oc = osuCrypto;
using sympsica::CoeffCtxFp61;
using sympsica::Fp;
using sympsica::Share;
using sympsica::u64;
using sympsica::ZtGate;
using sympsica::ZtGatePool;

// A DPF key populated with recognizably NON-ZERO bytes in every field --
// domain/numTrees match production (protocols/detail/ztgate_pipeline.hpp's
// kDomain=2^16, kNumDigits=4), but this test never runs the real DKG
// protocol: it only needs a key whose bytes are checkably nonzero before
// zeroization and checkably zero after, which a hand-populated key gives
// directly, without paying for a real two-party DKG round.
oc::RegularDpfKey make_nonzero_key() {
    oc::RegularDpfKey k;
    CoeffCtxFp61 ctx;
    k.resize<Fp, CoeffCtxFp61>(/*domain=*/oc::u64(1) << 16, /*numTrees=*/4, ctx);
    k.mSeed = oc::block(0x1111111111111111ull, 0x2222222222222222ull);
    for (u64 i = 0; i < k.mCorrectionWords.size(); ++i)
        k.mCorrectionWords(i) = oc::block(i + 1, i + 2);
    for (u64 i = 0; i < k.mCorrectionBits.size(); ++i) k.mCorrectionBits(i) = oc::u8(1 + (i % 2));
    for (u64 i = 0; i < k.mLeafVals.size(); ++i) k.mLeafVals[i] = oc::u8(0xAB ^ i);
    return k;
}

bool key_is_all_zero(const oc::RegularDpfKey& k) {
    if (!(k.mSeed == oc::ZeroBlock)) return false;
    for (u64 i = 0; i < k.mCorrectionWords.size(); ++i)
        if (!(k.mCorrectionWords(i) == oc::ZeroBlock)) return false;
    for (u64 i = 0; i < k.mCorrectionBits.size(); ++i)
        if (k.mCorrectionBits(i) != 0) return false;
    for (u64 i = 0; i < k.mLeafVals.size(); ++i)
        if (k.mLeafVals[i] != 0) return false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// SC7 [DPF-KEY], positive leg (take()): the pool's OWN residual storage is
// scrubbed; the CALLER's returned copy is the untouched real key.
// ---------------------------------------------------------------------------
TEST(DpfKeyZeroization, SC7_TakeScrubsPoolStorageButNotTheReturnedItem) {
    const oc::RegularDpfKey original = make_nonzero_key();
    ASSERT_FALSE(key_is_all_zero(original)) << "test setup bug: the 'nonzero' key is actually all-zero";

    ZtGatePool pool;
    ZtGate item;
    item.corr_id = 1;
    item.mask_m = Share{Fp(42)};
    item.key = original;

    std::vector<ZtGate> batch;
    batch.push_back(item);
    pool.refill(std::move(batch));

    ZtGate taken = pool.take();

    // The CALLER's copy: the real, untouched key -- correctness of the
    // protocol depends on this (a scrubbed key handed to the caller would
    // silently break every consumer, e.g. test/gates/kat_symdiff.cpp's
    // real z-test gate, which already exercises ZtGatePool::take() end to
    // end and stays green under this change -- see task-24-report.md).
    EXPECT_TRUE(taken.key == original) << "the item returned to the caller must be the REAL key, unmodified";

    // The POOL's own residual storage: fully scrubbed.
    const ZtGate& residual = pool.debug_last_consumed();
    EXPECT_TRUE(key_is_all_zero(residual.key))
        << "the pool's own stored copy must be zeroized after take() hands the real key to the caller";
}

// ---------------------------------------------------------------------------
// SC7, take_by_id() leg -- the double-consume-guarded entry point gets the
// same treatment (pools.cpp applies zeroize_dpf_key on both paths).
// ---------------------------------------------------------------------------
TEST(DpfKeyZeroization, SC7_TakeByIdScrubsPoolStorageButNotTheReturnedItem) {
    const oc::RegularDpfKey original = make_nonzero_key();

    ZtGatePool pool;
    ZtGate item;
    item.corr_id = 7;
    item.mask_m = Share{Fp(1)};
    item.key = original;

    std::vector<ZtGate> batch;
    batch.push_back(item);
    pool.refill(std::move(batch));

    ZtGate taken = pool.take_by_id(7);

    EXPECT_TRUE(taken.key == original);
    EXPECT_TRUE(key_is_all_zero(pool.debug_last_consumed().key));
}

// ---------------------------------------------------------------------------
// Sanity: Triple's pool (no key material) is unaffected by this change --
// debug_last_consumed() exists uniformly but zeroizes nothing for Triple.
// ---------------------------------------------------------------------------
TEST(DpfKeyZeroization, TriplePoolTakeIsUnaffected) {
    sympsica::TriplePool pool;
    sympsica::Triple t;
    t.corr_id = 3;
    t.a = Share{Fp(1)};
    t.b = Share{Fp(2)};
    t.c = Share{Fp(3)};
    std::vector<sympsica::Triple> batch;
    batch.push_back(t);
    pool.refill(std::move(batch));

    sympsica::Triple taken = pool.take();
    EXPECT_EQ(taken.a, t.a);
    EXPECT_EQ(taken.b, t.b);
    EXPECT_EQ(taken.c, t.c);
}
