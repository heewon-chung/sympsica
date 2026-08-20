#include "sympsica/protocols/query.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sympsica/gates/beaver.hpp"
#include "sympsica/gates/symdiff.hpp"
#include "sympsica/utils/common.hpp"
#include "sympsica/utils/crash.hpp"
#include "sympsica/utils/serdes.hpp"

// query.cpp — Protocol 4 + full evaluation (task-17-brief.md W5.2-W5.4).
//
// Wire layout (R-ROUND0, binding):
//
//   Round 0 (EVERY query, before path selection): Channel::exchange() of a
//   fixed 9-byte record {u64 my_size LE, u8 announce}, announce = (|st.J| >
//   Params::U_MAX). Both parties learn the OTHER party's current raw-set
//   size and announce bit from this ONE round -- this is what SwitchRule::
//   decide() needs (nA/nB/counterpartAnnounced) BEFORE either party can
//   pick a path, and it is ALSO the "+ my current size" W5.3 step 2 asks
//   for (no separate size resend on the incremental leg) and the "+ sizes"
//   W5.4 asks for (ditto, full leg).
//
//   Incremental-only round: Channel::exchange() of a FIXED-LENGTH,
//   length-PREFIX-FREE array of exactly Params::U_MAX raw LE u64 words
//   (8*u_max bytes each way) -- J~, this party's padded dirty-bucket set.
//   No length prefix: both parties already know the length is exactly
//   u_max by construction (R-ROUND0), so write_u64_vec's built-in count
//   prefix would be pure overhead here.
//
//   Full-path-only round: Channel::exchange() of serdes.hpp's ordinary
//   length-prefixed u64 vector (write_u64_vec/read_u64_vec) carrying
//   `supports` -- this party's padded occupied-bucket set, padded to
//   min(my_size, Params::M). Unlike J~, this length is NOT fixed across
//   parties (it tracks each party's own my_size), so the wire format keeps
//   serdes.hpp's normal length prefix; the receiving side additionally
//   cross-checks the parsed count against min(counterpart_size, M) (known
//   already from round 0) as a protocol-desync guard, but the length
//   prefix itself is what makes read_u64_vec self-describing.
//
// PRG / pad sampler (R-SEED): a small production splitmix64-based sampler,
// seeded by mixing (seed, tag, query_no), tags "pad" (incremental) /
// "padfull" (full) -- see sample_pad_indices() below. This mirrors (does
// NOT reuse -- production cannot depend on test/**) the SAME public-domain
// splitmix64 algorithm test/utils/fixture_support.hpp already pins for KAT
// determinism; the two independent implementations are expected to (and,
// by construction, do) compute bit-identical splitmix64 outputs for the
// same state, since the algorithm itself is what's shared, not the code.
//
// R-SYND (syndrome construction): built LOCALLY, never sent -- for each
// beta in the evaluation set, this party's share of (d1..d7)(beta) is
// +table.row(beta) if Receiver, -table.row(beta) (Fp::neg(), which already
// maps 0 -> 0) if Sender; an unoccupied bucket reads back the zero row
// (PowerSumTable::row's own documented padding-row semantics), so a padded
// beta needs no special-casing here.

namespace sympsica {

namespace {

// --- explicit-LE helpers for the two non-serdes-primitive wire pieces in
// this file (round 0's record, J~'s fixed raw array) -- same "explicit LE
// for scalars beyond write_fp/write_u64_vec" allowance core/state.cpp's own
// local helpers already establish (R4).
void write_u64_le(u8* dst, u64 x) {
    for (int i = 0; i < 8; ++i) dst[i] = static_cast<u8>(x >> (8 * i));
}

u64 read_u64_le(const u8* src) {
    u64 x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<u64>(src[i]) << (8 * i);
    return x;
}

// --- production splitmix64 pad sampler (R-SEED) -------------------------

u64 splitmix64_next(u64& state) {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Folds a short ASCII tag into a u64 (FNV-1a-style, same construction as
// splitmix64's own pairing -- see this file's top comment on why this
// duplicates rather than includes the pinned test digest).
u64 fnv1a_tag(const char* tag) {
    u64 h = 0xCBF29CE484222325ull;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(tag); *p != 0; ++p) {
        h ^= *p;
        h *= 0x100000001B3ull;
    }
    return h;
}

u64 pad_prg_seed(u64 seed, const char* tag, u64 query_no) {
    u64 state = seed ^ fnv1a_tag(tag);
    (void)splitmix64_next(state);
    state ^= query_no;
    (void)splitmix64_next(state);
    return state;
}

// sample_pad_indices(seed, tag, query_no, count, exclude) — rejection-
// samples `count` DISTINCT bucket indices in [1, Params::M] from the PRG
// seeded by (seed, tag, query_no), skipping any index already in `exclude`
// OR already picked earlier in THIS call (both W5.3/W5.4: "collisions with
// the counterpart's set are fine per spec" -- only self-collisions and
// collisions with `exclude` are rejected).
std::vector<u32> sample_pad_indices(u64 seed, const char* tag, u64 query_no, u64 count,
                                     const std::set<u32>& exclude) {
    std::vector<u32> out;
    out.reserve(count);
    std::unordered_set<u32> picked;
    picked.reserve(count);
    u64 state = pad_prg_seed(seed, tag, query_no);
    while (out.size() < count) {
        u64 r = splitmix64_next(state);
        u32 idx = static_cast<u32>(r % Params::M) + 1;
        if (exclude.count(idx) != 0 || picked.count(idx) != 0) continue;
        picked.insert(idx);
        out.push_back(idx);
    }
    return out;
}

// --- R-SYND: local syndrome share construction ---------------------------

std::array<Share, Params::K> build_syndrome(const PartyState& st, u32 beta, Role role) {
    std::array<Fp, Params::K> row = st.table.row(beta); // zero row if unoccupied
    std::array<Share, Params::K> out{};
    for (u64 k = 0; k < Params::K; ++k) {
#ifdef SYMPSICA_WRONG_SIGN
        // FC1 [TV-F8] (task-19-brief.md R-FCFLAGS): TEST-ONLY wrong
        // construction -- the Sender's sign convention is flipped from
        // -row to +row (losing R-SYND's receiver-+/sender-- asymmetry
        // entirely), matching TV-F8's literal "flip one party's sign
        // convention" text. NEVER defined in the default build (CMake
        // option SYMPSICA_WRONG_SIGN defaults OFF, same precedent as
        // SYMPSICA_NO_FILTER); exists only so a separately-configured
        // build dir can demonstrate FC1's asymmetric-set count corruption.
        out[k] = Share{row[k]};
#else
        out[k] = Share{role == Role::Sender ? row[k].neg() : row[k]};
#endif
    }
    return out;
}

// eval_union(role, st, pools, ch, union_set) — R-EXHAUST guard, then the
// shared "build syndromes -> SymDiffEvaluator::eval_buckets" step common to
// both the incremental and full paths. betas is the evaluation set, SORTED
// ASCENDING (R-SYND: both parties derive it identically from the SAME
// exchanged union, so both sides' `betas`/`new_shares` line up index-for-
// index without any further coordination).
std::pair<std::vector<u32>, std::vector<Share>> eval_union(Role role, const PartyState& st,
                                                             Pools& pools, Channel& ch,
                                                             const std::set<u32>& union_set) {
    std::vector<u32> betas(union_set.begin(), union_set.end()); // std::set already ascending

    // R-EXHAUST: an undersized pool is a clean pre-evaluation abort -- NO
    // state has been mutated by anything above this point (round 0 / the
    // J~-or-supports exchange only read st.*, never wrote it).
    SYMPSICA_REQUIRE(pools.triples.remaining() >= 44 * betas.size() &&
                          pools.gates.remaining() >= 4 * betas.size(),
                      "Query::run: pool exhaustion -- refill_offline is required between "
                      "queries (R-EXHAUST); Query never refills its own pools");

    std::vector<std::array<Share, Params::K>> syndromes;
    syndromes.reserve(betas.size());
    for (u32 beta : betas) syndromes.push_back(build_syndrome(st, beta, role));

    BeaverEngine engine(role);
    std::vector<Share> new_shares =
        SymDiffEvaluator::eval_buckets(betas, syndromes, engine, pools.triples, pools.gates, ch);
    return {std::move(betas), std::move(new_shares)};
}

} // namespace

namespace detail {

// commit(...) — the ATOMIC COMMIT shared by both paths (FT6: only ever
// called AFTER eval_union has fully returned). `replace` selects W5.4's
// REPLACE semantics (cache := exactly {beta -> new[beta] : beta in
// union}, t_share := sum(new)) vs W5.3's ACCUMULATE semantics (cache/
// t_share updated only for buckets in `union`, everything else untouched).
//
// Task-20 fix round 1 (M1): moved OUT of the anonymous namespace above (and
// declared in query.hpp) so test/protocols/kat_query.cpp's FC3 negative can
// call the REAL commit once on synthetic data instead of hand-deriving both
// sides of a comparison from the same two numbers (which was a tautology --
// see kat_query.cpp's own FC3 comment). Fix round 2: placed in `detail`,
// this codebase's own established precedent for "internal, but needs
// cross-TU reach including tests" (core/pools.hpp's CorrelationPool<T>,
// protocols/setup.hpp's SetupOtState) -- see query.hpp's own comment. No
// live Channel / no networking either way, so this is a pure, cheap local
// call.
void commit(PartyState& st, const std::vector<u32>& betas, const std::vector<Share>& new_shares,
            bool replace, const std::string& state_path) {
    if (replace) {
        Fp total(0);
        for (const Share& sh : new_shares) total = total.add(sh.v);
        st.cache.clear();
#ifdef SYMPSICA_PARTIAL_CACHE_COMMIT
        // task-28-brief.md PLAN-REVIEW REVISIONS R3 (R6-NOTAUTO): TEST-ONLY
        // wrong construction -- every cache slot gets betas[0]'s share
        // instead of its own. cache.size() is UNCHANGED (still
        // betas.size() entries) and t_share below is still computed
        // correctly from the REAL new_shares, so query_no/J.size()/
        // cache.size()/my_size all still match a correct commit exactly --
        // this is precisely "a partial commit that preserves the four
        // current scalar fields" (task-28-brief.md R3's own required FC
        // shape). NEVER defined in the default build (option defaults OFF,
        // SYMPSICA_DOUBLE_APPLY_COMMIT precedent); exists only so
        // apps/crash_post_check.cpp's reconstructed-cache/t_share check can
        // be demonstrated failing against a REAL wrong value.
        for (std::size_t i = 0; i < betas.size(); ++i)
            st.cache[betas[i]] = new_shares.empty() ? Share{Fp(0)} : new_shares[0];
#else
        for (std::size_t i = 0; i < betas.size(); ++i) st.cache[betas[i]] = new_shares[i];
#endif
        st.t_share = Share{total};
    } else {
        Fp delta(0);
        for (std::size_t i = 0; i < betas.size(); ++i) {
            Fp old_val(0);
            auto it = st.cache.find(betas[i]);
            if (it != st.cache.end()) old_val = it->second.v;
            delta = delta.add(new_shares[i].v.sub(old_val));
            st.cache[betas[i]] = new_shares[i];
        }
#ifdef SYMPSICA_DOUBLE_APPLY_COMMIT
        // task-20-brief.md M1 fix round 1 (R6-NOTAUTO): TEST-ONLY wrong
        // construction -- literally reproduces FC3's described bug
        // ("double-applying the SAME commit delta drifts t_share") by
        // applying `delta` twice instead of once. NEVER defined in the
        // default build (option defaults OFF, SYMPSICA_NO_FILTER
        // precedent); exists only so kat_query.cpp's structural-shape
        // assertion can be demonstrated failing against a REAL wrong value.
        st.t_share.v = st.t_share.v.add(delta).add(delta);
#else
        st.t_share.v = st.t_share.v.add(delta);
#endif
    }
    st.J.clear();
    ++st.query_no;

    // R-CRASH: everything above is pure in-memory PartyState mutation;
    // this is the LAST point before persistence. A crash here leaves the
    // ON-DISK file exactly as it was before this query (save() has not
    // been called yet), even though *this* (the in-memory PartyState) has
    // already been mutated -- the in-memory copy is discarded along with
    // the process, so only the disk file's atomicity matters.
    crash_point("pre-serialize");
    st.save(state_path);
}

} // namespace detail

namespace {

// INV2 = (P+1)/2 = 2^60 exactly (P = 2^61-1 is odd; 2*2^60 = 2^61 = P+1 ==
// 1 mod P) -- R-CONVERT's field inverse of 2.
constexpr Fp INV2 = Fp(1ull << 60);

// convert(role, t_share, nA, nB) — R-CONVERT, step 5 (W5.3/W5.4's shared
// final step): result = affine(-inv2, t_share, inv2*(nA+nB)) with the
// additive constant landing ONLY on the Receiver (core/share.hpp's affine()
// always adds `b`; the CALLER decides which party passes a nonzero one --
// this is exactly that decision, made once, here). Reduces to c_R =
// inv2*(nA+nB-t_R) on the Receiver and c_S = -inv2*t_S on the Sender,
// matching W5.3 step 5 literally.
Share convert(Role role, const Share& t_share, u64 nA, u64 nB) {
    Fp a = INV2.neg();
#ifdef SYMPSICA_WRONG_CONVERT
    // FC2 [TV-F9 at E2E scale] (task-19-brief.md R-FCFLAGS): TEST-ONLY
    // wrong construction -- the Receiver-form additive constant is applied
    // UNCONDITIONALLY, regardless of `role` (i.e. the Sender ALSO adds the
    // public nA+nB term it must never add). NEVER defined in the default
    // build (CMake option SYMPSICA_WRONG_CONVERT defaults OFF); exists
    // only so a separately-configured build dir can demonstrate FC2's
    // count corruption at schedule/E2E scale (Task 17's kat_query.cpp FC1
    // already covers the unit-level leg of the same defect).
    Fp b = INV2.mul(Fp::from_u64(nA).add(Fp::from_u64(nB)));
#else
    Fp b = role == Role::Receiver ? INV2.mul(Fp::from_u64(nA).add(Fp::from_u64(nB))) : Fp(0);
#endif
    return affine(a, t_share, b);
}

// --- incremental path (W5.3) ---------------------------------------------

Share run_incremental(Role role, PartyState& st, Pools& pools, Channel& ch,
                       const std::string& state_path, u64 seed, u64 my_size,
                       u64 counterpart_size) {
    SYMPSICA_REQUIRE(st.J.size() <= Params::U_MAX,
                      "Query::run: |J| exceeds u_max on the incremental path (SwitchRule "
                      "invariant violation)");
    const u64 pad_count = Params::U_MAX - st.J.size();
    std::vector<u32> pad = sample_pad_indices(seed, "pad", st.query_no, pad_count, st.J);

    std::vector<u64> j_tilde;
    j_tilde.reserve(Params::U_MAX);
    for (u32 beta : st.J) j_tilde.push_back(beta);
    for (u32 beta : pad) j_tilde.push_back(beta);
    SYMPSICA_REQUIRE(j_tilde.size() == Params::U_MAX,
                      "Query::run: J~ padding produced the wrong size (internal bug)");

    std::vector<u8> out_buf(8 * Params::U_MAX);
    for (u64 i = 0; i < Params::U_MAX; ++i) write_u64_le(out_buf.data() + i * 8, j_tilde[i]);
    std::vector<u8> in_buf(8 * Params::U_MAX);
    ch.exchange(out_buf, in_buf);

    std::set<u32> union_set;
    for (u64 v : j_tilde) union_set.insert(static_cast<u32>(v));
    for (u64 i = 0; i < Params::U_MAX; ++i) {
        union_set.insert(static_cast<u32>(read_u64_le(in_buf.data() + i * 8)));
    }

    auto [betas, new_shares] = eval_union(role, st, pools, ch, union_set);
    detail::commit(st, betas, new_shares, /*replace=*/false, state_path);
    return convert(role, st.t_share, my_size, counterpart_size);
}

// --- full path (W5.4) -----------------------------------------------------

Share run_full(Role role, PartyState& st, Pools& pools, Channel& ch, const std::string& state_path,
               u64 seed, u64 my_size, u64 counterpart_size) {
    const u64 target = std::min(my_size, Params::M);

    // R-OCCUPIED (task-18-brief.md controller ruling; found via a real
    // two-process E2E crash): "occupied" must be the buckets whose row is
    // NON-ZERO, not every KEY st.table's underlying map happens to hold.
    // PowerSumTable::edit(id, -1, ...) (a delete) decrements a row's power
    // sums but never ERASES the map entry, even once every value hits
    // exactly zero -- so a bucket whose only id(s) were all later deleted
    // leaves a "ghost" all-zero row behind. table.hpp's own row() contract
    // already treats an ABSENT row identically to an all-zero row (the
    // "padding row" convention); this filter simply extends that same
    // equivalence to a row that is PRESENT but happens to be all-zero.
    //
    // This filter is EXACT, not a heuristic, for any t in [0, 7] ids netted
    // into one bucket: the Newton's-identities argument below holds for
    // t <= 7 because a row only ever stores K=7 power sums (p_1..p_7), so
    // the recursion cannot reach past e_7. What actually BOUNDS a real
    // bucket's occupancy is the protocol's own T=4 no-overflow assumption
    // (Params::T) -- K=7 is the storage DEPTH the table happens to keep,
    // not itself an occupancy cap; T=4 is what real buckets stay under.
    // by Newton's identities, power sums p_1..p_t determine the elementary
    // symmetric polynomials e_1..e_t of the bucket's sigma-values via the
    // standard recursion (e_1 = p_1; k*e_k = sum_{i=1}^{k} (-1)^(i-1) e_{k-i} p_i);
    // if p_1 == p_2 == ... == p_t == 0, that recursion forces
    // e_1 == e_2 == ... == e_t == 0 too, which makes the bucket's own
    // characteristic polynomial prod_i (X - sigma(x_i)) collapse to X^t --
    // i.e. every sigma(x_i) == 0. But sigma(x) = g^x (Encoder::sigma) is
    // never 0 in F_p^* (g is a generator, F_p^* has no zero divisors), a
    // contradiction unless t == 0. So "row is all-zero" <=> "bucket holds
    // nothing" exactly, matching the plan's own "occupied buckets of own
    // table" reading -- not merely "every key ever touched".
    std::set<u32> occupied;
    for (const auto& [beta, row] : st.table.rows()) {
        bool nonzero = false;
        for (const Fp& v : row) {
            if (v.v != 0) {
                nonzero = true;
                break;
            }
        }
        if (nonzero) occupied.insert(beta);
    }
    SYMPSICA_REQUIRE(occupied.size() <= target,
                      "Query::run: occupied buckets exceed min(my_size, m) (invariant violation)");
    const u64 pad_count = target - occupied.size();
    std::vector<u32> pad = sample_pad_indices(seed, "padfull", st.query_no, pad_count, occupied);

    std::vector<u64> supports;
    supports.reserve(target);
    for (u32 beta : occupied) supports.push_back(beta);
    for (u32 beta : pad) supports.push_back(beta);
    SYMPSICA_REQUIRE(supports.size() == target,
                      "Query::run: supports padding produced the wrong size (internal bug)");

    std::vector<u8> out_buf(u64_vec_wire_size(supports.size()));
    write_u64_vec(out_buf, supports);

    const u64 counterpart_target = std::min(counterpart_size, Params::M);
    std::vector<u8> in_buf(u64_vec_wire_size(counterpart_target));
    ch.exchange(out_buf, in_buf);
    std::vector<u64> their_supports = read_u64_vec(in_buf);
    SYMPSICA_REQUIRE(their_supports.size() == counterpart_target,
                      "Query::run: peer supports length disagrees with round-0 size "
                      "(protocol desync)");

    std::set<u32> union_set;
    for (u64 v : supports) union_set.insert(static_cast<u32>(v));
    for (u64 v : their_supports) union_set.insert(static_cast<u32>(v));

    auto [betas, new_shares] = eval_union(role, st, pools, ch, union_set);
    detail::commit(st, betas, new_shares, /*replace=*/true, state_path);
    return convert(role, st.t_share, my_size, counterpart_size);
}

} // namespace

SwitchRule::Path SwitchRule::decide(const Params& pp, u64 nA, u64 nB, u64 myJ, bool firstQuery,
                                     bool counterpartAnnounced) {
    (void)pp;
    if (firstQuery || 2 * Params::U_MAX >= std::min(nA, Params::M) + std::min(nB, Params::M)) {
        return Path::FullPublic;
    }
#ifdef SYMPSICA_WRONG_BOUNDARY
    // FC4 [TV-F12 negative] (task-19-brief.md R-FCFLAGS / controller ruling
    // R-FC4-REAL): TEST-ONLY wrong construction -- the boundary comparison
    // is relaxed from strict `>` to `>=`, so a query with |J| == u_max
    // EXACTLY wrongly announces (SC4's own real Query::run schedule drives
    // |J| to exactly 1024, expecting Incremental/no-announce; under this
    // flag it wrongly takes FullAnnounced instead). NEVER defined in the
    // default build (CMake option SYMPSICA_WRONG_BOUNDARY defaults OFF,
    // same precedent as the other three R-FCFLAGS flags); exists only so a
    // separately-configured build dir can demonstrate FC4's boundary
    // assertions failing against a REAL wrong construction, not a
    // standalone comparison of two lambdas (the tautology the controller's
    // review caught in an earlier draft of this file).
    if (myJ >= Params::U_MAX || counterpartAnnounced) {
        return Path::FullAnnounced;
    }
#else
    if (myJ > Params::U_MAX || counterpartAnnounced) {
        return Path::FullAnnounced;
    }
#endif
    return Path::Incremental;
}

Share Query::run(Role role, PartyState& st, Pools& pools, Channel& ch, const Params& params,
                  const std::string& state_path, u64 seed, bool force_full,
                  SwitchRule::Path* path_out) {
    // R6-N2 guard (task-26-brief.md, plan-review R3): `params` was
    // otherwise unused in this function (hence the `(void)params;` this
    // replaces) -- st.table/st.J were already built under whatever oracle
    // was in effect at Update::apply/SaltManager::refresh time, and this
    // function only exchanges/evaluates those precomputed rows, never
    // recomputing G.of(id) itself. That is exactly why the guard belongs
    // here too: it is the one check that confirms the oracle a caller
    // THINKS is current (params.oracle) actually matches the one st's
    // stored rows were built under, catching a stale reload before this
    // query answers with silently-wrong buckets. Covers BOTH the ordinary
    // caller AND SaltManager::refresh's own internal forced-full call
    // (by the time refresh() reaches that call it has already updated
    // st.oracle_salt/params.oracle together, so this passes trivially
    // there).
    st.require_salt_match(params.oracle);
    const bool first_query = (st.query_no == 0);
    const bool my_announce = (st.J.size() > Params::U_MAX);

    // Round 0 (R-ROUND0): fixed 9-byte record {u64 my_size, u8 announce}.
    // Runs UNCONDITIONALLY, even when force_full is set (task-18-brief.md
    // R-FORCEFULL): the counterpart's size is still needed by convert()
    // below regardless of how the path was chosen.
    u8 out0[9];
    write_u64_le(out0, st.my_size);
    out0[8] = my_announce ? 1 : 0;
    u8 in0[9];
    ch.exchange(std::span<const u8>(out0, 9), std::span<u8>(in0, 9));
#ifdef SYMPSICA_STALE_SIZES
    // FC5 [stale sizes] (task-19-brief.md R-FCFLAGS): TEST-ONLY wrong
    // construction standing in for "skip round 0's size exchange" (Task
    // 17's R-ROUND0 folded the plan's literal "step-2 size exchange" into
    // round 0 -- same quantity, see task-19-report.md for the mapping).
    // The wire round STILL happens (round-0 byte counts/round structure
    // stay unchanged, so the two processes never desync) but the freshly
    // EXCHANGED size is discarded -- `counterpart_size` is pinned to 0,
    // simulating a party that never actually incorporates the counterpart's
    // real, current size. NEVER defined in the default build (CMake option
    // SYMPSICA_STALE_SIZES defaults OFF); exists only so a separately-
    // configured build dir can demonstrate FC5's E2E-3 count corruption
    // (the Receiver's convert() constant silently drops the +nB term).
    const u64 counterpart_size = 0;
#else
    const u64 counterpart_size = read_u64_le(in0);
#endif
    const bool counterpart_announce = in0[8] != 0;

    // R-FORCEFULL: force_full skips SwitchRule::decide() entirely and takes
    // the full path directly -- FullPublic is the forced value (not
    // FullAnnounced): both land on the identical run_full() branch below
    // (the switch's case list already merges them), so the choice of which
    // literal enumerator to force is purely cosmetic UNLESS a caller reads
    // it back via path_out, in which case FullPublic is the semantically
    // correct label (maintenance's "supports exchange; no announce bit").
    SwitchRule::Path path = force_full
                                 ? SwitchRule::Path::FullPublic
                                 : SwitchRule::decide(params, st.my_size, counterpart_size,
                                                       st.J.size(), first_query, counterpart_announce);
    if (path_out != nullptr) *path_out = path;

    switch (path) {
        case SwitchRule::Path::Incremental:
            return run_incremental(role, st, pools, ch, state_path, seed, st.my_size,
                                    counterpart_size);
        case SwitchRule::Path::FullPublic:
        case SwitchRule::Path::FullAnnounced:
            return run_full(role, st, pools, ch, state_path, seed, st.my_size, counterpart_size);
    }
    SYMPSICA_REQUIRE(false, "Query::run: SwitchRule::decide returned an unhandled path "
                             "(internal bug)");
    return Share{Fp(0)}; // unreachable; SYMPSICA_REQUIRE(false, ...) aborts above.
}

u64 Query::open_count(Channel& ch, Share mine) { return open(ch, mine).v; }

} // namespace sympsica
