// Task 2 self-verification (task-2 brief, "Verification expected from you").
// Throwaway self-checks, NOT the formal KAT suite — Task 3 owns kat_field.cpp
// and the formal FLD/ENC golden vectors (kat_encoding.cpp).
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <random>
#include <sstream>
#include <vector>

#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/encoding.hpp"
#include "sympsica/utils/field.hpp"
#include "sympsica/utils/net.hpp"
#include "sympsica/utils/params.hpp"
#include "sympsica/utils/serdes.hpp"

using namespace sympsica;

TEST(Task2SelfCheck, FLD2Identity) {
    Fp pm1(Fp::P - 1);
    EXPECT_EQ(pm1.add(Fp(2)), Fp(1));
    EXPECT_EQ(pm1.mul(pm1), Fp(1));
}

TEST(Task2SelfCheck, FromU64MaxIsSeven) {
    EXPECT_EQ(Fp::from_u64(~0ull), Fp(7));
}

TEST(Task2SelfCheck, InverseRoundTrip) {
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<u64> dist(1, Fp::P - 1);
    for (int i = 0; i < 1000; ++i) {
        Fp x(dist(rng));
        EXPECT_EQ(x.mul(x.inv()), Fp(1));
    }
}

TEST(Task2SelfCheck, GeneratorSearchTerminatesAndIsValid) {
    Encoder enc;
    u64 g = enc.generator();
    ASSERT_GT(g, 1u);
    std::fprintf(stderr, "[Task2SelfCheck] found generator g = %llu\n",
                 static_cast<unsigned long long>(g));

    static constexpr std::array<u64, 12> factors = {
        2ull, 3ull, 5ull, 7ull, 11ull, 13ull, 31ull, 41ull, 61ull, 151ull, 331ull, 1321ull
    };
    for (u64 q : factors) {
        EXPECT_NE(Fp(g).pow((Fp::P - 1) / q), Fp(1)) << "q = " << q;
    }
    // full order check: g^(p-1) == 1
    EXPECT_EQ(Fp(g).pow(Fp::P - 1), Fp(1));

    EXPECT_EQ(enc.sigma(0), Fp(1));
    EXPECT_EQ(enc.sigma(1), Fp(g));
}

TEST(Task2SelfCheck, SigmaGenericToyField101) {
    // ENC-2 concrete row hook: F_101, g = 2.
    EXPECT_EQ(Encoder::sigma_generic(2, 101, 0), 1u);
    EXPECT_EQ(Encoder::sigma_generic(2, 101, 1), 2u);
    EXPECT_EQ(Encoder::sigma_generic(2, 101, 10), 1024u % 101u);
}

TEST(Task2SelfCheck, BucketOracleRange) {
    BucketOracle oracle; // epoch 0, all-zero salt
    for (u64 id = 0; id < 1000; ++id) {
        u32 b = oracle.of(id);
        EXPECT_GE(b, 1u);
        EXPECT_LE(b, static_cast<u32>(BucketOracle::M));
    }
    // all-zero salt state is directly observable
    for (u8 byte : oracle.salt()) EXPECT_EQ(byte, 0);
}

TEST(Task2SelfCheck, BucketOracleRefreshedChangesSalt) {
    std::array<u8, 4> rR{1, 2, 3, 4};
    std::array<u8, 4> rS{5, 6, 7, 8};
    BucketOracle fresh = BucketOracle::refreshed(rR, rS);
    bool all_zero = true;
    for (u8 byte : fresh.salt()) all_zero &= (byte == 0);
    EXPECT_FALSE(all_zero);
}

TEST(Task2SelfCheck, ParamsInstantiateAndEcho) {
    Params pp = Params::instantiate();
    EXPECT_EQ(pp.g(), Encoder().generator());
    for (u8 byte : pp.salt()) EXPECT_EQ(byte, 0); // epoch 0

    std::ostringstream oss;
    pp.echo(oss);
    EXPECT_NE(oss.str().find("p = 2305843009213693951"), std::string::npos);
}

TEST(Task2SelfCheck, SerdesFpRoundTrip) {
    std::array<u8, 8> buf{};
    write_fp(buf, Fp(1));
    // byte dump of write_fp(1) == 01 00 00 00 00 00 00 00
    std::array<u8, 8> expected{1, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(buf, expected);
    EXPECT_EQ(read_fp(buf), Fp(1));

    std::mt19937_64 rng(1234);
    std::uniform_int_distribution<u64> dist(0, Fp::P - 1);
    for (int i = 0; i < 1000; ++i) {
        Fp x(dist(rng));
        std::array<u8, 8> b{};
        write_fp(b, x);
        EXPECT_EQ(read_fp(b), x);
    }
}

TEST(Task2SelfCheck, SerdesU64VecRoundTrip) {
    std::mt19937_64 rng(5678);
    std::uniform_int_distribution<u64> dist;
    for (std::size_t n : {0u, 1u, 5u, 100u}) {
        std::vector<u64> values(n);
        for (auto& v : values) v = dist(rng);

        std::vector<u8> buf(u64_vec_wire_size(values.size()));
        write_u64_vec(buf, values);
        auto round_tripped = read_u64_vec(buf);
        EXPECT_EQ(round_tripped, values);
    }
}

// Corrupt length-prefix regression: a declared count near 2^61 makes the old
// `in.size() >= u64_vec_wire_size(count)` check overflow (8 + 8*count wraps
// around u64) and pass despite the 8-byte buffer being far too small. Must
// be rejected via SYMPSICA_REQUIRE (abort), not reach std::vector's
// allocator with a huge count.
TEST(Task2SelfCheck, ReadU64VecRejectsOverflowingLengthPrefix) {
    std::array<u8, 8> buf{};
    u64 huge_count = (u64(1) << 61) - 1; // 8 + 8*huge_count wraps to a tiny value
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<u8>(huge_count >> (8 * i));
    }
    EXPECT_DEATH({ read_u64_vec(buf); }, "read_u64_vec: length prefix exceeds buffer");
}

// coeff_ctx.hpp: exercises CoeffCtxFp61 end-to-end (bitSize, isField,
// characteristicTwo, fromBlock, plus/minus/mul) to confirm the adapter
// actually compiles AND links, not merely parses.
//
// binaryDecomposition() is intentionally NOT exercised here: it constructs a
// real osuCrypto::BitVector via a constructor whose implementation lives in
// cryptoTools/Common/BitVector.cpp, which is not compiled/linked into this
// target (Phase 0 deliberately left libOTe/cryptoTools unbuilt; see
// task-2-report.md, "Concerns"). The method is still declared in
// CoeffCtxFp61 and does compile (template code is only instantiated, and
// only then does it need that symbol, when actually called) — this test's
// job is exactly to confirm that calling it is what's deferred, not the
// declaration.
TEST(Task2SelfCheck, CoeffCtxFp61Basic) {
    CoeffCtxFp61 ctx;
    EXPECT_TRUE(ctx.isField<Fp>());
    EXPECT_FALSE(ctx.characteristicTwo<Fp>());
    EXPECT_EQ(ctx.bitSize<Fp>(), 61u);

    Fp a(5), b(7), r(0);
    ctx.plus(r, a, b);
    EXPECT_EQ(r, Fp(12));
    ctx.minus(r, a, b);
    EXPECT_EQ(r, a.sub(b));
    ctx.mul(r, a, b);
    EXPECT_EQ(r, Fp(35));

    osuCrypto::block blk(0, ~0ull);
    Fp from_block(0);
    ctx.fromBlock(from_block, blk);
    EXPECT_EQ(from_block, Fp::from_u64(~0ull));
}

// net.hpp/net.cpp: Ruling 4's stub — Channel's constructor is REQUIRED to
// abort (SYMPSICA_REQUIRE(false, ...)) until Phase 2 wires coproto's TCP
// backend (see task-2-report.md's "Concerns" for why). Verified via death
// test rather than a live loopback smoke test, which this stub cannot
// support.
TEST(Task2SelfCheck, ChannelStubAborts) {
    EXPECT_DEATH({ Channel ch("127.0.0.1:0", true); }, "net: wired at Phase 2");
}
