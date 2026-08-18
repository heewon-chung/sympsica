#ifndef SYMPSICA_PROTOCOLS_DETAIL_ZTGATE_PIPELINE_HPP
#define SYMPSICA_PROTOCOLS_DETAIL_ZTGATE_PIPELINE_HPP

// protocols/detail/ztgate_pipeline — W2.1's mask-and-key pipeline: end-to-end
// generation of ZtGates (the offline correlated randomness the online
// z-test consumes).
//
// MOVED to production (task-16-brief.md, W5.1, R-HOST consolidation): this
// file originated under test/integration/ (task-5, W2.1) as "library code
// that happens to live under test/ until Phase 5". Phase 5's Setup
// (protocols/setup.cpp) is the production caller, and R-HOST forbids
// production code including a test header or calling a "test copy" -- so
// this is now the ONE production copy; test/integration/ztgate_pipeline.hpp
// is a thin forwarding shim (`#include` of this file) so every existing
// Phase 2-4 test call site keeps compiling unchanged. No gtest dependency,
// no inline TEST() bodies -- this was already written as reusable library
// code, so the move is a file relocation plus a header-guard/include-path
// rename, not a rewrite (task-5-report.md's own framing, now made literal).
//
// One ZtGate is produced by exactly this sequence (plan text W2.1, verbatim):
//
//   1. Mask sampling      — each party samples a private uniform 61-bit string
//                           (r_R resp. r_S); the logical mask is the INTEGER
//                           r = r_R XOR r_S in [0, 2^61-1].
//   2. Rejection          — the single bad value r = 2^61-1 (= p, whose field
//                           value 0 breaks digit comparison) is rejected
//                           WITHOUT revealing r: the 61-bit AND of the
//                           XOR-shared bits is computed by a GMW boolean
//                           circuit (60 AND gates) and ONLY that 1-bit result
//                           is opened. If 1 (prob 2^-61), discard and resample.
//   3. B2A                — the 61 XOR-shared bits are converted to additive
//                           F_p shares via Gilboa (one COT per bit); each party
//                           then locally computes its share of
//                           m = sum_j b_j 2^j mod p.
//   4. Digit derivation   — free and carry-less: the four 16-bit digit slices
//                           of r_R and r_S are already XOR shares of r's
//                           canonical digits (XOR sharing is bitwise).
//   5. DKG                — ONE interactive RegularDpf<Fp, CoeffCtxFp61>
//                           keyGen call, domain 2^16, numPoints = 4, the four
//                           XOR-shared digit points, values = additive shares
//                           of the public payload 1. Output: ONE multi-point
//                           RegularDpfKey per party per gate.
//
// Socket type. Everything here takes a `coproto::Socket&`, which is already
// libOTe's type-erased socket handle: coproto::LocalAsyncSocket (used by
// test/integration's in-process tests) and coproto's asio/TCP socket (via
// sympsica::Channel::socket()) both convert into it, so no local socket type
// is baked into the API.
//
// Correlation sources. Random OTs are NOT generated inside the pipeline; an
// OtPool is filled once (libOTe silent OT, both directions) and passed in.
// That mirrors what Setup does at Phase 5 — one silent-OT expansion amortised
// over every gate — and lets tests reuse one pool across several runs.

#include <array>
#include <cstdint>
#include <vector>

#include "coproto/Socket/Socket.h"
#include "cryptoTools/Common/BitVector.h"
#include "cryptoTools/Common/Defines.h"
#include "cryptoTools/Crypto/PRNG.h"
#include "libOTe/Dpf/DpfMult.h"
#include "libOTe/Dpf/RegularDpf.h"
#include "macoro/task.h"

#include "sympsica/utils/coeff_ctx.hpp"
#include "sympsica/utils/field.hpp"

namespace sympsica::ztgate {

namespace oc = osuCrypto;

// --- parameters -------------------------------------------------------------
// Frozen by the task-5 brief's "Global constants": c = 4 digit chunks of
// w = 16 bits (top digit 13 bits), DPF domain 2^16, numPoints = 4.
inline constexpr oc::u64 kMaskBits = 61;
inline constexpr oc::u64 kNumDigits = 4;
inline constexpr oc::u64 kDigitBits = 16;
inline constexpr oc::u64 kTopDigitBits = 13;
inline constexpr oc::u64 kDomain = oc::u64(1) << kDigitBits;

// The one rejected mask value: r = 2^61-1 = p, whose field value is 0.
inline constexpr oc::u64 kRejectedMask = Fp::P;

// AND gates in the 61-bit tree reduction: 61->31->16->8->4->2->1, i.e.
// 30 + 15 + 8 + 4 + 2 + 1 = 60 (plan text W2.1: "60 AND gates").
inline constexpr oc::u64 kAndGates = 60;

// Random OTs consumed per gate, per party:
//   - DKG   : RegularDpf::baseOtCount() = numPoints * (depth + 1) = 4 * 17 = 68
//   - GMW   : one OT per AND gate                                        = 60
//   - B2A   : one OT per mask bit                                        = 61
// A rejected mask costs kAndGates more (its resample re-runs step 2 only).
inline constexpr oc::u64 kOtsDkg = kNumDigits * (kDigitBits + 1);
inline constexpr oc::u64 kOtsPerGate = kOtsDkg + kAndGates + kMaskBits;

// R is party 0 and S is party 1 throughout (libOTe's partyIdx convention).
enum class Role : oc::u64 { Receiver = 0, Sender = 1 };
inline oc::u64 party_idx(Role r) { return static_cast<oc::u64>(r); }

// --- step 4: digit derivation ------------------------------------------------
// Carry-less little-endian 16-bit digit split of a 61-bit value: digits
// [0..2] are 16 bits wide, digit 3 is the narrow top digit (13 bits). Applied
// to a party's own XOR half r_X this yields XOR shares of r's digits, because
// XOR sharing is bitwise and this split does not carry.
//
// `v` is masked to 61 bits first, so a caller may pass a full 64-bit word
// (ZT-3 pins exactly that case).
std::array<oc::u64, kNumDigits> digit_split(oc::u64 v);

// --- correlated randomness ---------------------------------------------------
// A pool of random OTs held by ONE party in both roles simultaneously:
//   send[i]    — the two messages this party offered as OT sender
//   recv[i]    — the message this party learned as OT receiver
//   choices[i] — this party's OT-receiver choice bit
// so that peer.send[i][choices[i]] == recv[i]. That is exactly the shape
// oc::DpfMult / oc::RegularDpf::setBaseOts consume.
struct OtSlice {
    oc::span<const std::array<oc::block, 2>> send;
    oc::span<const oc::block> recv;
    oc::BitVector choices;
};

class OtPool {
public:
    std::vector<std::array<oc::block, 2>> send;
    std::vector<oc::block> recv;
    oc::BitVector choices;

    oc::u64 size() const { return send.size(); }
    oc::u64 consumed() const { return cursor_; }
    oc::u64 remaining() const { return size() - cursor_; }

    // Carve the next `n` OTs off the pool. Aborts (SYMPSICA_REQUIRE) if the
    // pool is exhausted — a silent wrap-around would reuse OTs and destroy
    // security, so this must never be a soft failure.
    OtSlice take(oc::u64 n);

    // Convenience: take(n) wired straight into a fresh DpfMult.
    oc::DpfMult take_mult(Role role, oc::u64 n);

private:
    oc::u64 cursor_ = 0;
};

// Fill `pool` with `n` random OTs in each direction using libOTe silent OT.
// Both parties must call this with the same `n`. The two directions run
// sequentially in role-opposite order (party 0 sends first, party 1 receives
// first) so a single socket carries both without interleaving.
macoro::task<> generate_ot_pool(Role role, oc::u64 n, oc::PRNG& prng,
                                coproto::Socket& sock, OtPool& pool);

// --- step 2: GMW rejection test ---------------------------------------------
// For each 61-bit XOR half in `halves`, compute the AND of all 61 bits of the
// reconstructed r via a GMW tree (60 AND gates over 6 rounds, batched across
// every entry of `halves`) and OPEN only that single bit. `opened[i]` is
// therefore [r_i == 2^61-1] and nothing else about r_i leaks.
//
// `mult` must have at least kAndGates * halves.size() base OTs available.
macoro::task<> gmw_all_ones(Role role, oc::span<const oc::u64> halves,
                            oc::DpfMult& mult, coproto::Socket& sock,
                            std::vector<oc::u8>& opened);

// --- step 3: Gilboa B2A ------------------------------------------------------
// Convert the 61 XOR-shared bits of each mask into an additive F_p share of
// m = sum_j b_j 2^j mod p. `mult` must have at least kMaskBits *
// halves.size() base OTs available.
macoro::task<> gilboa_b2a(Role role, oc::span<const oc::u64> halves,
                          oc::DpfMult& mult, coproto::Socket& sock,
                          std::vector<Fp>& mask_shares);

// --- the whole pipeline ------------------------------------------------------
struct ZtGateOut {
    // Stable per-gate correlation identifier (the gate's index within the run;
    // Phase 5 will key the online opening by it).
    oc::u64 corr_id = 0;

    // This party's XOR half r_X of the logical mask r = r_R XOR r_S. Exposed
    // for test assertions only — Setup keeps it private to the party.
    oc::u64 mask_half = 0;

    // Step 4's XOR shares of r's digits, == digit_split(mask_half).
    std::array<oc::u64, kNumDigits> digit_shares{};

    // Step 3's additive F_p share of m == r.
    Fp mask_share{};

    // Step 5's multi-point DPF key (all four digit trees in one key).
    oc::RegularDpfKey key;
};

struct PipelineStats {
    oc::u64 rejected = 0;         // masks that opened to 1 and were resampled
    oc::u64 resample_rounds = 0;  // GMW rounds beyond the first
    oc::u64 ots_consumed = 0;
};

struct PipelineOpts {
    oc::u64 count = 1;

    // Step-1 injection (W2.4 boundary-mask reuse: "skip step-1 sampling, keep
    // steps 2-5"). Entry i replaces the sampled r_X of gate i; entries beyond
    // forced_mask_halves.size() are sampled normally. A forced mask that gets
    // rejected in step 2 IS resampled at random, exactly like a sampled one.
    // Test-support knob (task-16-brief.md R-HOST): production's Setup never
    // sets this; only Phase-2/4/5 boundary-mask tests do (e.g. FC4).
    std::vector<oc::u64> forced_mask_halves;

    // Safety valve on the resample loop. Rejection has probability 2^-61, so
    // in a sampled run this is never approached; it bounds the forced-rejection
    // tests and turns a hypothetical livelock into a loud abort.
    oc::u64 max_resample_rounds = 8;
};

// Run steps 1-5 for `opts.count` gates. Both parties call this symmetrically
// over the same socket with the same `opts.count`; every control-flow branch
// is driven by publicly opened values, so the two sides stay in lockstep.
macoro::task<> generate_ztgates(Role role, coproto::Socket& sock,
                                const PipelineOpts& opts, oc::PRNG& prng,
                                OtPool& pool, std::vector<ZtGateOut>& out,
                                PipelineStats& stats);

// --- helpers shared with the tests -------------------------------------------
// Decode one F_p from a RegularDpfKey::mLeafVals slot. libOTe writes those
// bytes through CoeffCtxFp61::serialize (a host-endian memcpy, see the audit
// note in coeff_ctx.hpp), so on the little-endian hosts this project targets
// they are byte-identical to serdes' LE wire rule; this decoder reads them
// with explicit LE shifts either way and aborts on a non-canonical value.
Fp read_leaf_val(oc::span<const oc::u8> leaf_vals, oc::u64 index);

} // namespace sympsica::ztgate

#endif // SYMPSICA_PROTOCOLS_DETAIL_ZTGATE_PIPELINE_HPP
