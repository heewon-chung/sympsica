#include "sympsica/protocols/salt.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <span>

#include "sympsica/protocols/query.hpp"
#include "sympsica/utils/common.hpp"

// salt.cpp — Protocol W5.5's salt-refresh maintenance event, R-SCHED2's
// ScheduleSync pair-drift guard, and W5.6's stub OverflowChecker
// (task-18-brief.md).
//
// r_R/r_S generation (SCOPE NOTE): this PoC derives each party's own
// 32-byte contribution deterministically from the SAME --seed party_main
// already threads through to Query::run's own pad sampler (R-SEED
// precedent, query.cpp), via a small local splitmix64 stream tagged
// "saltR"/"saltS" (per-role, not per-party-identity -- the Receiver always
// draws from the "saltR" stream, the Sender always from "saltS") and mixed
// with st.query_no so consecutive refreshes on the same party draw
// distinct bytes. This mirrors -- does NOT reuse, production cannot depend
// on test/** -- query.cpp's own pad_prg_seed/splitmix64_next construction
// (same public-domain algorithm, independently duplicated). A production
// deployment wanting cryptographic freshness independent of a party's own
// --seed input would instead draw r_R/r_S from real OS entropy; that is
// out of this PoC's scope (same deterministic-seed convention Query::run's
// own R-SEED padding already established) and is documented here, not
// silently upgraded.

namespace sympsica {

namespace {

u64 splitmix64_next(u64& state) {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

u64 fnv1a_fold_bytes(const void* data, std::size_t n) {
    u64 h = 0xCBF29CE484222325ull; // FNV-1a offset basis (same constant query.cpp's fnv1a_tag uses)
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull; // FNV-1a prime
    }
    return h;
}

std::array<u8, 32> sample_salt_contribution(u64 seed, const char* tag, u64 query_no) {
    u64 state = seed ^ fnv1a_fold_bytes(tag, std::strlen(tag));
    (void)splitmix64_next(state);
    state ^= query_no;
    (void)splitmix64_next(state);

    std::array<u8, 32> out{};
    for (int word = 0; word < 4; ++word) {
        u64 r = splitmix64_next(state);
        for (int b = 0; b < 8; ++b) out[static_cast<std::size_t>(word) * 8 + b] = static_cast<u8>(r >> (8 * b));
    }
    return out;
}

void write_u64_le(u8* dst, u64 x) {
    for (int i = 0; i < 8; ++i) dst[i] = static_cast<u8>(x >> (8 * i));
}

u64 read_u64_le(const u8* src) {
    u64 x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<u64>(src[i]) << (8 * i);
    return x;
}

} // namespace

// --- SaltManager -------------------------------------------------------

bool SaltManager::due(u64 query_no, u64 phi, bool maintenance_day, bool force) {
    if (force || maintenance_day) return true;
    if (phi == 0 || query_no == 0) return false;
    return (query_no % phi) == 0;
}

Share SaltManager::refresh(Role role, PartyState& st, Params& params, Pools& pools, Channel& ch,
                            const std::string& state_path, u64 seed) {
    const std::array<u8, 32> mine =
        sample_salt_contribution(seed, role == Role::Receiver ? "saltR" : "saltS", st.query_no);
    std::array<u8, 32> theirs{};
    ch.exchange(std::span<const u8>(mine.data(), mine.size()),
                std::span<u8>(theirs.data(), theirs.size()));

    // R-SALTX: input ordering fixed as H(r_R || r_S) regardless of which
    // role THIS party plays.
    const std::array<u8, 32>& r_R = (role == Role::Receiver) ? mine : theirs;
    const std::array<u8, 32>& r_S = (role == Role::Receiver) ? theirs : mine;

    params.oracle = BucketOracle::refreshed(r_R, r_S);
    st.table.rebuild(st.my_ids, params.encoder, params.oracle);

    // R-FORCEFULL: the unified maintenance event -- immediately run
    // Query's forced-full path. Query::run's own commit() clears st.J and
    // advances st.query_no exactly as any other committed query.
    return Query::run(role, st, pools, ch, params, state_path, seed, /*force_full=*/true);
}

// --- ScheduleSync --------------------------------------------------------

u64 ScheduleSync::digest(const std::string& day, bool query, bool maintenance) {
    u64 h = fnv1a_fold_bytes(day.data(), day.size());
    const unsigned char tail[3] = {0, query ? static_cast<unsigned char>(1) : static_cast<unsigned char>(0),
                                    maintenance ? static_cast<unsigned char>(1) : static_cast<unsigned char>(0)};
    for (unsigned char b : tail) {
        h ^= b;
        h *= 0x100000001B3ull;
    }
    return h;
}

void ScheduleSync::sync(Channel& ch, const std::string& day, bool query, bool maintenance) {
    const u64 mine = digest(day, query, maintenance);
    u8 out[8];
    write_u64_le(out, mine);
    u8 in[8];
    ch.exchange(std::span<const u8>(out, 8), std::span<u8>(in, 8));
    const u64 theirs = read_u64_le(in);
    SYMPSICA_REQUIRE(mine == theirs,
                      "ScheduleSync::sync: schedule PAIR DRIFT detected (R-SCHED2) -- the two "
                      "parties' day/query/maintenance markers disagree");
}

// --- OverflowChecker -----------------------------------------------------

bool OverflowChecker::check(const PartyState& st, u64 t_prime) {
    (void)st;
    (void)t_prime;
    SYMPSICA_REQUIRE(false,
                      "OverflowChecker::check: detector is reference-only in PoC -- see plan "
                      "W5.6 (ref/reference.py's detect_overflow); the depth-T' detector needs "
                      "syndromes to depth 2T'-1=13 > K=7, and an MPC deep-scan circuit is out "
                      "of PoC scope");
}

} // namespace sympsica
