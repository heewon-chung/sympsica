#ifndef SYMPSICA_PROTOCOLS_DETAIL_SETUP_OT_HPP
#define SYMPSICA_PROTOCOLS_DETAIL_SETUP_OT_HPP

// protocols/detail/setup_ot.hpp — Setup's persistent silent-OT extension
// machinery (task-16-brief.md R-PKOP/R-CONT). Internal to protocols/setup.*;
// exposed as its own header (not kept file-local to setup.cpp) for exactly
// one reason: FC3 (CLM-B non-vacuity) needs a "direct call to the internal
// entry point" that runs the SAME base-OT step Setup::run uses, a SECOND
// time on a throwaway state, to prove PkOpCounter is actually capable of
// observing a base-OT execution -- i.e. that SC5's "constant across
// refills" assertion is not vacuously true. Included by setup.cpp and by
// test/protocols/kat_setup.cpp ONLY (R-HOST: this is production code
// exposing a test hook; the include direction is test -> production
// detail header, never the reverse).
//
// Why a persistent SilentOtExtSender/Receiver pair is needed at all (R-PKOP:
// "Base OTs run only inside Setup::run; refill_offline ... runs ZERO base
// OTs"): ztgate::generate_ot_pool (protocols/detail/ztgate_pipeline.hpp)
// constructs FRESH SilentOtExtSender/Receiver objects on every call, so
// calling it from refill_offline would silently re-run libOTe's own
// DefaultBaseOT (SimplestOT) bootstrap on every fill -- exactly the
// per-lifetime-once cost Setup exists to amortise. Reusing the SAME
// SilentOtExtSender/Receiver objects across every fill instead relies on a
// documented libOTe call chain (traced while implementing this task, not
// stated in any libOTe doc comment):
//   - SilentOtExtSender::configure()/SilentOtExtReceiver::configure() touch
//     only mRequestNumOts/mNumPartitions/mSizePer and re-`configure()` the
//     PPRF generator (gen()) -- they never touch mOtExtSender, the
//     IKNP/SoftSpoken object that actually holds the kappa=128 base OTs.
//   - RegularPprfSender/Receiver::configure() resets ITS OWN mBaseOTs
//     (`mBaseOTs.resize(0, 0)`), so `gen().hasBaseOts()` is false again
//     after every configure() -- and configure() is auto-invoked at the top
//     of every silentSendInplace()/silentReceive() call whose previous call
//     already cleared mRequestNumOts back to 0 (SilentOtExtSender::clear(),
//     called at the end of the public silentSend()/receive() wrappers).
//   - `hasBaseCors()` is therefore false on EVERY silentSend/silentReceive
//     call, so genBaseCors() runs every time -- but genBaseCors() only
//     invokes DefaultBaseOT (SimplestOT, the expensive public-key step) the
//     FIRST time, when `mOtExtSender` itself is unset
//     (`if (!mOtExtSender) { ...DefaultBaseOT...; mOtExtSender->setBaseOts(...); }`,
//     SilentOtExtSender.cpp); once mOtExtSender IS set (by run_base_ots(),
//     below), every subsequent genBaseCors() call instead runs
//     `mOtExtSender->send(...)` -- a cheap IKNP/SoftSpoken symmetric-key
//     extension off the SAME persisted base OTs, deriving a FRESH batch of
//     PPRF-level base correlations each time (correct: PPRF base
//     correlations are single-use per silent-OT expansion; the underlying
//     kappa=128 base OTs are the reusable, amortised part).
// Net effect: as long as ONE `SetupOtState` (below) is created once per
// Pools lifetime and its `sender`/`recver` are reused (never reconstructed)
// for every OT-pool fill -- Setup::run's initial fill AND every later
// refill_offline call -- the expensive SimplestOT base-OT step really does
// run exactly once, not just "as observed by PkOpCounter".

#include "coproto/Socket/Socket.h"
#include "cryptoTools/Common/Defines.h"
#include "cryptoTools/Crypto/PRNG.h"
#include "libOTe/TwoChooseOne/Silent/SilentOtExtReceiver.h"
#include "libOTe/TwoChooseOne/Silent/SilentOtExtSender.h"
#include "macoro/task.h"

#include "sympsica/core/share.hpp"
#include "sympsica/protocols/detail/ztgate_pipeline.hpp"

namespace sympsica::detail {

namespace oc = osuCrypto;

// Persistent state carried across Setup::run and every refill_offline call
// on the SAME Pools (R-CONT). Move-only (SilentOtExtSender/Receiver are not
// copyable) -- Pools stores this behind a unique_ptr (net.hpp's Channel
// pimpl precedent) so libOTe's Silent-OT headers stay out of setup.hpp.
struct SetupOtState {
    oc::SilentOtExtSender sender;
    oc::SilentOtExtReceiver recver;
    oc::PRNG ot_prng{oc::sysRandomSeed()};    // seeds silentSend/silentReceive's PRNG arg
    oc::PRNG proto_prng{oc::sysRandomSeed()}; // seeds generate_ztgates/beaver_triples' PRNG arg
};

// The ONE explicit base-OT execution call site (R-PKOP): runs libOTe's
// genBaseOts() on `st.sender` and on `st.recver` -- which, with
// ENABLE_SIMPLESTOT (PINS.md, CMakeLists.txt: "Base OTs + extensions needed
// by RegularDpf (DKG) + NoisyVole + SilentOT" / "Chou-Orlandi base OT"),
// bootstraps via the Chou-Orlandi SimplestOT DefaultBaseOT protocol at
// kappa=128 -- in the same role-opposite call order
// ztgate::generate_ot_pool already uses (Role::Receiver: sender-then-recver;
// Role::Sender: recver-then-sender) so the two parties' base-OT rounds
// don't interleave on one socket. Increments PkOpCounter once per
// genBaseOts() call (2 per invocation: once for `st.sender`, once for
// `st.recver`). Setup::run calls this EXACTLY ONCE per Pools lifetime;
// refill_offline NEVER calls it. Exposed at namespace scope (not static in
// setup.cpp) so FC3 can invoke it a second time directly, on a separate
// throwaway SetupOtState, and observe PkOpCounter increase.
macoro::task<> run_base_ots(Role role, SetupOtState& st, coproto::Socket& sock);

// Fills `pool` with `n` fresh random OTs in each direction using `st`'s
// PERSISTENT sender/recver (see the file header above for why this must NOT
// be ztgate::generate_ot_pool, which constructs fresh Sender/Receiver
// objects -- and would therefore re-run base OTs -- on every call).
// PRECOND: run_base_ots(role, st, sock) has already completed on `st`. Same
// role-opposite call order as generate_ot_pool, for the same
// interleaving-avoidance reason; `st.ot_prng` is the PRNG source (matches
// generate_ot_pool's own single-PRNG-per-party convention).
macoro::task<> fill_ot_pool(Role role, oc::u64 n, SetupOtState& st, coproto::Socket& sock,
                            ztgate::OtPool& pool);

} // namespace sympsica::detail

#endif // SYMPSICA_PROTOCOLS_DETAIL_SETUP_OT_HPP
