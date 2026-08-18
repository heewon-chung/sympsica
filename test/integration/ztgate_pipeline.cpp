#include "ztgate_pipeline.hpp"

#include <utility>

#include "coproto/Socket/Socket.h"
#include "libOTe/TwoChooseOne/Silent/SilentOtExtReceiver.h"
#include "libOTe/TwoChooseOne/Silent/SilentOtExtSender.h"

#include "sympsica/utils/common.hpp"
#include "sympsica/utils/serdes.hpp"

namespace sympsica::ztgate {

namespace {

// Bit j of a party's own 61-bit XOR half.
inline oc::u8 bit_at(oc::u64 v, oc::u64 j) { return static_cast<oc::u8>((v >> j) & 1u); }

// libOTe's silent OT wants a reasonably sized request; below this the LPN
// parameterisation is not worth configuring and the fixed base-OT cost
// dominates anyway.
constexpr oc::u64 kMinSilentOts = 128;

} // namespace

// --- step 4 ------------------------------------------------------------------

std::array<oc::u64, kNumDigits> digit_split(oc::u64 v)
{
    v &= kRejectedMask;  // 2^61 - 1, i.e. mask to the low 61 bits

    std::array<oc::u64, kNumDigits> d{};
    for (oc::u64 j = 0; j + 1 < kNumDigits; ++j)
        d[j] = (v >> (kDigitBits * j)) & ((oc::u64(1) << kDigitBits) - 1);

    // The top digit is narrower (61 = 3*16 + 13), so it gets its own mask.
    d[kNumDigits - 1] =
        (v >> (kDigitBits * (kNumDigits - 1))) & ((oc::u64(1) << kTopDigitBits) - 1);
    return d;
}

// --- correlated randomness ---------------------------------------------------

OtSlice OtPool::take(oc::u64 n)
{
    SYMPSICA_REQUIRE(n <= remaining(),
                     "OtPool::take: pool exhausted (OTs must never be reused)");
    OtSlice s;
    s.send = oc::span<const std::array<oc::block, 2>>(send.data() + cursor_, n);
    s.recv = oc::span<const oc::block>(recv.data() + cursor_, n);
    s.choices.append(choices, n, cursor_);
    cursor_ += n;
    return s;
}

oc::DpfMult OtPool::take_mult(Role role, oc::u64 n)
{
    auto s = take(n);
    oc::DpfMult m;
    m.init(party_idx(role), n);
    m.setBaseOts(s.send, s.recv, s.choices);
    return m;
}

macoro::task<> generate_ot_pool(Role role, oc::u64 n, oc::PRNG& prng,
                                coproto::Socket& sock, OtPool& pool)
{
    auto req = std::max<oc::u64>(n, kMinSilentOts);

    oc::SilentOtExtSender sender;
    oc::SilentOtExtReceiver recver;
    sender.configure(req);
    recver.configure(req);

    pool.send.resize(req);
    pool.recv.resize(req);
    pool.choices.resize(req);

    // Both directions share one socket, so they must not interleave: party 0
    // runs the sender leg first and party 1 the receiver leg first.
    if (role == Role::Receiver) {
        co_await sender.silentSend(pool.send, prng, sock);
        co_await recver.silentReceive(pool.choices, pool.recv, prng, sock, oc::OTType::Random);
    } else {
        co_await recver.silentReceive(pool.choices, pool.recv, prng, sock, oc::OTType::Random);
        co_await sender.silentSend(pool.send, prng, sock);
    }
}

// --- step 2: GMW rejection test ---------------------------------------------

macoro::task<> gmw_all_ones(Role role, oc::span<const oc::u64> halves,
                            oc::DpfMult& mult, coproto::Socket& sock,
                            std::vector<oc::u8>& opened)
{
    auto n = static_cast<oc::u64>(halves.size());
    opened.assign(n, 0);
    if (n == 0)
        co_return;

    // Flat layout: mask i's bit j of the current level lives at i*width + j.
    auto width = kMaskBits;
    oc::BitVector cur(n * width);
    for (oc::u64 i = 0; i < n; ++i)
        for (oc::u64 j = 0; j < width; ++j)
            cur[i * width + j] = bit_at(halves[i], j);

    oc::u64 and_gates = 0;
    while (width > 1) {
        auto pairs = width / 2;
        auto carry = width % 2;              // odd level: the last bit rides along
        auto next_width = pairs + carry;

        oc::BitVector x(n * pairs), y(n * pairs), xy(n * pairs);
        for (oc::u64 i = 0; i < n; ++i) {
            for (oc::u64 j = 0; j < pairs; ++j) {
                x[i * pairs + j] = cur[i * width + 2 * j];
                y[i * pairs + j] = cur[i * width + 2 * j + 1];
            }
        }

        // One GMW round: componentwise AND of two XOR-shared bit vectors, via
        // libOTe's Beaver-triple bit multiplier (DpfMult::multiplyBits builds
        // the triples from the random OTs we handed it).
        co_await mult.multiplyBits(x, y, xy, sock);
        and_gates += n * pairs;

        oc::BitVector next(n * next_width);
        for (oc::u64 i = 0; i < n; ++i) {
            for (oc::u64 j = 0; j < pairs; ++j)
                next[i * next_width + j] = xy[i * pairs + j];
            if (carry)
                next[i * next_width + pairs] = cur[i * width + width - 1];
        }
        cur = std::move(next);
        width = next_width;
    }
    SYMPSICA_REQUIRE(and_gates == kAndGates * n,
                     "gmw_all_ones: AND-gate count does not match the plan's 60 per mask");

    // Open ONLY the final bit. Values cross the wire through serdes (the
    // project's single wire codec) as one length-prefixed u64 vector of 0/1.
    std::vector<u64> mine(n);
    for (oc::u64 i = 0; i < n; ++i)
        mine[i] = cur[i];

    std::vector<u8> out_buf(u64_vec_wire_size(n));
    write_u64_vec(out_buf, mine);
    co_await sock.send(std::move(out_buf));

    std::vector<u8> in_buf(u64_vec_wire_size(n));
    co_await sock.recv(in_buf);
    auto peer = read_u64_vec(in_buf);
    SYMPSICA_REQUIRE(peer.size() == n, "gmw_all_ones: peer opened the wrong number of bits");

    for (oc::u64 i = 0; i < n; ++i) {
        SYMPSICA_REQUIRE(peer[i] <= 1 && mine[i] <= 1,
                         "gmw_all_ones: opened share is not a bit");
        opened[i] = static_cast<oc::u8>(mine[i] ^ peer[i]);
    }
}

// --- step 3: Gilboa B2A ------------------------------------------------------

macoro::task<> gilboa_b2a(Role role, oc::span<const oc::u64> halves,
                          oc::DpfMult& mult, coproto::Socket& sock,
                          std::vector<Fp>& mask_shares)
{
    // `role` is accepted for signature symmetry with the other two steps but
    // is not read: this step is fully symmetric, and the party index libOTe
    // needs is already baked into `mult` by OtPool::take_mult.
    (void)role;

    auto n = static_cast<oc::u64>(halves.size());
    mask_shares.assign(n, Fp(0));
    if (n == 0)
        co_return;

    auto total = n * kMaskBits;
    CoeffCtxFp61 ctx;

    // For bit j of mask i let x_R, x_S be the two XOR shares. Feed the bit
    // sharing as the boolean input and (x_R, x_S) themselves as an ADDITIVE
    // F_p sharing of Y = x_R + x_S. libOTe's bit-times-field multiplier then
    // returns an additive sharing of
    //     b * Y = (x_R xor x_S) * (x_R + x_S),
    // which equals b for every (x_R, x_S) in {0,1}^2: b = 0 forces the product
    // to 0, and b = 1 forces Y = 1. That is Gilboa's B2A with the cross term
    // x_R * x_S supplied by the COT, written so libOTe does the work.
    oc::BitVector x(total);
    auto y = ctx.makeVec<Fp>(total);
    auto xy = ctx.makeVec<Fp>(total);
    for (oc::u64 i = 0; i < n; ++i) {
        for (oc::u64 j = 0; j < kMaskBits; ++j) {
            auto b = bit_at(halves[i], j);
            x[i * kMaskBits + j] = b;
            y[i * kMaskBits + j] = Fp(b);
        }
    }

    co_await mult.multiply<Fp, CoeffCtxFp61>(x.getSpan<const oc::u8>(), y, xy, sock, ctx);

    for (oc::u64 i = 0; i < n; ++i) {
        Fp acc(0);
        Fp pow(1);
        for (oc::u64 j = 0; j < kMaskBits; ++j) {
            // Canonicality (task-4-report.md (d)): every value here has just
            // come back from a libOTe protocol that materialises F_p from wire
            // bytes, so check the invariant field.hpp states rather than
            // assume it.
            SYMPSICA_REQUIRE(xy[i * kMaskBits + j].v < Fp::P,
                             "gilboa_b2a: non-canonical F_p share out of DpfMult::multiply");
            acc = acc.add(xy[i * kMaskBits + j].mul(pow));
            pow = pow.add(pow);  // 2^j, all of which are < p for j <= 60
        }
        SYMPSICA_REQUIRE(acc.v < Fp::P, "gilboa_b2a: non-canonical mask share");
        mask_shares[i] = acc;
    }
}

// --- the whole pipeline ------------------------------------------------------

Fp read_leaf_val(oc::span<const oc::u8> leaf_vals, oc::u64 index)
{
    SYMPSICA_REQUIRE((index + 1) * 8 <= static_cast<oc::u64>(leaf_vals.size()),
                     "read_leaf_val: index past the end of mLeafVals");
    u64 v = 0;
    for (oc::u64 i = 0; i < 8; ++i)
        v |= static_cast<u64>(leaf_vals[index * 8 + i]) << (8 * i);
    SYMPSICA_REQUIRE(v < Fp::P, "read_leaf_val: non-canonical F_p in a DPF key");
    return Fp(v);
}

macoro::task<> generate_ztgates(Role role, coproto::Socket& sock,
                                const PipelineOpts& opts, oc::PRNG& prng,
                                OtPool& pool, std::vector<ZtGateOut>& out,
                                PipelineStats& stats)
{
    auto count = opts.count;
    auto ot_start = pool.consumed();
    stats = PipelineStats{};
    out.assign(count, ZtGateOut{});
    if (count == 0)
        co_return;

    SYMPSICA_REQUIRE(opts.forced_mask_halves.size() <= count,
                     "generate_ztgates: more forced masks than gates");

    // --- step 1: mask sampling ---------------------------------------------
    // Each party samples its own private uniform 61-bit half; nothing is sent.
    std::vector<oc::u64> halves(count);
    for (oc::u64 i = 0; i < count; ++i) {
        halves[i] = (i < opts.forced_mask_halves.size())
                        ? (opts.forced_mask_halves[i] & kRejectedMask)
                        : (prng.get<oc::u64>() & kRejectedMask);
    }

    // --- step 2: rejection --------------------------------------------------
    // Every branch below is driven by publicly opened bits, so both parties
    // walk the identical control flow and stay in lockstep.
    std::vector<oc::u64> pending(count);
    for (oc::u64 i = 0; i < count; ++i)
        pending[i] = i;

    for (;;) {
        std::vector<oc::u64> pending_halves(pending.size());
        for (std::size_t k = 0; k < pending.size(); ++k)
            pending_halves[k] = halves[pending[k]];

        auto mult = pool.take_mult(role, kAndGates * pending.size());
        std::vector<oc::u8> opened;
        co_await gmw_all_ones(role, pending_halves, mult, sock, opened);

        std::vector<oc::u64> still_bad;
        for (std::size_t k = 0; k < pending.size(); ++k) {
            if (opened[k]) {
                still_bad.push_back(pending[k]);
                ++stats.rejected;
            }
        }
        if (still_bad.empty())
            break;

        ++stats.resample_rounds;
        SYMPSICA_REQUIRE(stats.resample_rounds <= opts.max_resample_rounds,
                         "generate_ztgates: rejection loop exceeded max_resample_rounds");

        // A rejected mask is discarded and resampled — including a forced one,
        // which is precisely what the injection test observes.
        for (auto i : still_bad)
            halves[i] = prng.get<oc::u64>() & kRejectedMask;
        pending = std::move(still_bad);
    }

    // --- step 3: B2A --------------------------------------------------------
    std::vector<Fp> mask_shares;
    {
        auto mult = pool.take_mult(role, kMaskBits * count);
        co_await gilboa_b2a(role, halves, mult, sock, mask_shares);
    }

    // --- steps 4 and 5 ------------------------------------------------------
    CoeffCtxFp61 ctx;

    // The public payload is 1, additively shared as (1, 0): the receiver
    // contributes 1 and the sender 0.
    auto values = ctx.makeVec<Fp>(kNumDigits);
    for (oc::u64 k = 0; k < kNumDigits; ++k)
        values[k] = (role == Role::Receiver) ? Fp(1) : Fp(0);

    for (oc::u64 g = 0; g < count; ++g) {
        auto& gate = out[g];
        gate.corr_id = g;
        gate.mask_half = halves[g];
        gate.mask_share = mask_shares[g];

        // step 4 — free: the digit slices of this party's own half already ARE
        // XOR shares of r's digits.
        gate.digit_shares = digit_split(halves[g]);
        std::vector<oc::u64> points(gate.digit_shares.begin(), gate.digit_shares.end());

        // step 5 — one interactive keyGen per gate, all four digit trees in it.
        oc::RegularDpf<Fp, CoeffCtxFp61> dpf;
        dpf.init(party_idx(role), kDomain, kNumDigits, ctx);
        SYMPSICA_REQUIRE(dpf.baseOtCount() == kOtsDkg,
                         "generate_ztgates: RegularDpf base-OT count is not the expected 68");

        auto s = pool.take(kOtsDkg);
        dpf.setBaseOts(s.send, s.recv, s.choices);

        co_await dpf.keyGen(oc::span<oc::u64>(points), values, prng, gate.key, sock, ctx);

        // Canonicality at the libOTe boundary: mLeafVals is what
        // RegularDpf::implExpand later feeds straight into
        // CoeffCtxFp61::deserialize (task-4-report.md (d), call site 3).
        SYMPSICA_REQUIRE(gate.key.mLeafVals.size() == kNumDigits * 8,
                         "generate_ztgates: unexpected mLeafVals size");
        for (oc::u64 k = 0; k < kNumDigits; ++k)
            (void)read_leaf_val(gate.key.mLeafVals, k);
    }

    stats.ots_consumed = pool.consumed() - ot_start;
}

} // namespace sympsica::ztgate
