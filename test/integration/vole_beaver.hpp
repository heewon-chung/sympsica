#ifndef SYMPSICA_TEST_INTEGRATION_VOLE_BEAVER_HPP
#define SYMPSICA_TEST_INTEGRATION_VOLE_BEAVER_HPP

// vole_beaver — W2.2's noisy-VOLE-based OLE and Beaver-triple assembly, built
// on oc::NoisyVoleSender/Receiver<Fp,Fp,CoeffCtxFp61> (vendor/libOTe/libOTe/
// Vole/Noisy/*.h). Companion to ztgate_pipeline.{hpp,cpp} (W2.1): same
// "library code that happens to live under test/ until Phase 5" ownership,
// same OtPool/Role types (reused from ztgate_pipeline.hpp), same
// coproto::Socket&-based two-party pattern.
//
// The actual NoisyVole correlation, read verbatim from the vendored headers
// (vendor/libOTe/libOTe/Vole/Noisy/NoisyVole{Sender,Receiver}.h): for a
// Sender-chosen `delta` (of type F) and a Receiver-chosen vector `c`,
//
//     a[j] = b[j] + c[j] * delta      for every j
//
// where the Receiver ends up holding (c, a) and the Sender ends up holding
// (delta, b). This IS an OLE (`u + v == x * y`) once the Sender's raw output
// is NEGATED: setting x = c, y = delta, u = a, v = -b gives
// u[j] + v[j] = a[j] - b[j] = c[j] * delta = x[j] * y. See ole_send's
// `negate` parameter and vole_beaver.cpp for the derivation, and
// task-6-report.md for the full trace.
//
// A HARD API CONSTRAINT (found while implementing, not stated in the brief):
// `delta` is a SCALAR shared across the WHOLE vector-length call — NoisyVole
// gives a "scalar-times-vector" correlation (one y, many x[j]'s), NOT an
// elementwise Hadamard product of two independently-varying vectors.
//
// CONTROLLER RULING (task-6 fix round 1, after the first report flagged this
// as Concern 1): a single batched call per cross term — where the Sender's
// per-triple secret would be one shared scalar across the whole 10^4-triple
// batch — is NOT acceptable for Phase-5 consumption; Setup's cost model
// ("noisy-VOLE Gilboa, 61 OTs/product") requires per-triple independent
// correlations on BOTH sides. `beaver_triples()` in vole_beaver.cpp
// therefore calls `ole_receive`/`ole_send` with a length-1 vector ONCE PER
// TRIPLE PER CROSS TERM (2 calls/triple, 61 OTs/call, 122 OTs/triple total)
// rather than batching the whole triple count into one call per cross term.
// `ole_receive`/`ole_send` themselves are unchanged and still batch freely
// when the caller wants a single shared `y` — see
// `Vole61.OlePrimitiveCorrectnessOver10000Correlations` in
// kat_dkg_vole61.cpp, which validates exactly that (primitive-only) shape.
//
// A SECOND hard constraint: the raw-array overloads of
// NoisyVoleSender::send/NoisyVoleReceiver::receive take pre-generated OT
// messages, but the Sender's `delta` is NOT free to choose independently of
// those messages — internally, `send()` computes
// `xb = ctx.binaryDecomposition(delta)` and uses `xb[i]` to select which of
// the two 61-OT-slice messages it reads at row i. For this to be
// mathematically consistent with a pre-generated OtSlice (whose choice bits
// were already fixed when the pool was filled), `delta` MUST equal the value
// encoded by that slice's `choices` bits — it cannot be chosen by the Sender
// at all. `ole_send` therefore derives `delta` FROM `ot.choices` and returns
// it as an output parameter, rather than accepting it as an input. This is
// fine for "random OLE correlations" (W2.2 requirement 2) and for Beaver
// triples (whose a/b shares are supposed to be random secrets anyway).

#include <vector>

#include "coproto/Socket/Socket.h"
#include "cryptoTools/Crypto/PRNG.h"
#include "macoro/task.h"

#include "sympsica/utils/field.hpp"
#include "ztgate_pipeline.hpp"

namespace sympsica::vole {

namespace oc = osuCrypto;
using sympsica::Fp;
using ztgate::OtSlice;
using ztgate::Role;

// One NoisyVole call's worth of base OTs: exactly 61 (CoeffCtxFp61::bitSize),
// REGARDLESS of the correlation-vector length `n` — NoisyVole amortises one
// 61-OT slice across an arbitrarily long PRG-expanded vector via one extra
// message round. `OtPool::take(kOlePerCallOts)` is the right call at every
// site below.
inline constexpr oc::u64 kOlePerCallOts = 61;

// Receiver side of one OLE-producing NoisyVole call. `x` is freely chosen by
// this party (the OLE's varying input); `u` is this party's additive share,
// assigned to size x.size() on return. `ot` must be a kOlePerCallOts-sized
// slice from THIS party's OtPool (the slice this party generated in its OT-
// sender role — see OtPool's class comment for the send/recv symmetry that
// makes this consistent with the peer's ole_send call).
macoro::task<> ole_receive(oc::span<const Fp> x, std::vector<Fp>& u, oc::PRNG& prng,
                            const OtSlice& ot, coproto::Socket& sock);

// Sender side. `y` is an OUTPUT: the value NoisyVole's OT layer forces (see
// the header note above) — derived from `ot.choices`, not chosen by the
// caller. `v` is this party's additive share, assigned to size n on return
// (negated NoisyVole output by default so that u[j] + v[j] == x[j] * y;
// `negate = false` exists ONLY for the deliberate sign-convention negative
// test — every positive caller leaves it at the default).
macoro::task<> ole_send(Fp& y, std::vector<Fp>& v, oc::u64 n, oc::PRNG& prng,
                         const OtSlice& ot, coproto::Socket& sock, bool negate = true);

// One party's shares of a batch of `n` Beaver triples (a, b, c): once both
// parties' shares are added elementwise, c[i] == a[i] * b[i] for every i.
// EVERY entry of a/b/c is independently random per triple, on BOTH parties'
// sides (per the controller ruling above) — also documented in
// task-6-report.md's "Beaver assembly algebra" section.
struct BeaverBatch {
    std::vector<Fp> a, b, c;
};

// Assemble `n` triples, each from its own pair of length-1 NoisyVole calls
// (per the controller ruling above). Consumes n * 2 * kOlePerCallOts OTs
// from `pool`. `corrupt_ct1_sign`: when true, skips the negation on the
// FIRST cross term's Sender share for EVERY triple (see ole_send's
// `negate`) — the deliberate convention-error negative test (W2.2
// requirement 5's "single... convention error" bullet); every positive
// caller leaves it false.
macoro::task<> beaver_triples(Role role, oc::u64 n, oc::PRNG& prng, ztgate::OtPool& pool,
                               coproto::Socket& sock, BeaverBatch& out,
                               bool corrupt_ct1_sign = false);

} // namespace sympsica::vole

#endif // SYMPSICA_TEST_INTEGRATION_VOLE_BEAVER_HPP
