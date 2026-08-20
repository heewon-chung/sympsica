#include "sympsica/core/pools.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

#include "sympsica/utils/common.hpp"

namespace sympsica {
namespace detail {

namespace {

// zeroize_dpf_key — task-24-brief.md W6.6(iv)/R6-DPFKEY: overwrites every
// field of a RegularDpfKey with zero bytes, using only RegularDpfKey's own
// PUBLIC API (vendor/ is read-only; nothing here modifies it).
// RegularDpfKey (vendor/libOTe/libOTe/Dpf/RegularDpf.h) is a plain struct:
//   block mSeed;                     -- trivially copyable
//   Matrix<block> mCorrectionWords;  -- MatrixView<T>::setZero() (memset)
//   Matrix<u8> mCorrectionBits;      -- same
//   std::vector<u8> mLeafVals;       -- plain std::fill
// This is called on the POOL's own residual copy AFTER a plain (non-move)
// copy has already handed an independent, untouched copy to the caller
// (see take()/take_by_id() below) -- so this function's job is exactly and
// only to scrub the pool's own leftover bytes, never the value the caller
// receives.
void zeroize_dpf_key(oc::RegularDpfKey& k) {
    k.mSeed = oc::ZeroBlock; // extern const, cryptoTools/Common/block.h -- unambiguously the
                              // all-zero block (block()'s own default-initialization semantics
                              // are trivial-type-dependent; ZeroBlock avoids relying on that).
    k.mCorrectionWords.setZero();
    k.mCorrectionBits.setZero();
    std::fill(k.mLeafVals.begin(), k.mLeafVals.end(), oc::u8(0));
}

} // namespace

template <typename T>
T CorrelationPool<T>::take() {
    SYMPSICA_REQUIRE(!order_.empty(), "CorrelationPool::take: pool exhausted");
    u64 id = order_.front();
    auto it = available_.find(id);
    if constexpr (std::is_same_v<T, ZtGate>) {
        // task-24-brief.md R6-DPFKEY/SC7: COPY (not move) the pool's own
        // entry, so `item` (handed to the caller) and `it->second.first`
        // (the pool's own storage) become two FULLY INDEPENDENT copies --
        // unlike a move, a Matrix/vector copy allocates fresh storage, so
        // every field of the pool's copy (mSeed AND the correction
        // words/bits/leaf vals) genuinely still holds live key bytes at
        // this point, not just whatever a move would happen to leave
        // behind. zeroize_dpf_key() then scrubs exactly that copy, in
        // place, before it is erased below; last_consumed_ keeps the
        // now-zeroized residual for debug_last_consumed() (SC7's "inspect
        // the pool's storage after a take" test hook).
        T item = it->second.first;
        zeroize_dpf_key(it->second.first.key);
        last_consumed_ = it->second.first;
        order_.erase(it->second.second);
        available_.erase(it);
        consumed_set_.insert(id);
        consumed_log_.push_back(id);
        return item;
    } else {
        T item = std::move(it->second.first);
        order_.erase(it->second.second);
        available_.erase(it);
        consumed_set_.insert(id);
        consumed_log_.push_back(id);
        return item;
    }
}

template <typename T>
T CorrelationPool<T>::take_by_id(u64 corr_id) {
    SYMPSICA_REQUIRE(!consumed_set_.count(corr_id),
                      "CorrelationPool::take_by_id: corr_id already consumed (double-consume)");
    auto it = available_.find(corr_id);
    SYMPSICA_REQUIRE(it != available_.end(),
                      "CorrelationPool::take_by_id: unknown corr_id (never refilled)");
    if constexpr (std::is_same_v<T, ZtGate>) {
        // Same rationale as take() above.
        T item = it->second.first;
        zeroize_dpf_key(it->second.first.key);
        last_consumed_ = it->second.first;
        order_.erase(it->second.second);
        available_.erase(it);
        consumed_set_.insert(corr_id);
        consumed_log_.push_back(corr_id);
        return item;
    } else {
        T item = std::move(it->second.first);
        order_.erase(it->second.second);
        available_.erase(it);
        consumed_set_.insert(corr_id);
        consumed_log_.push_back(corr_id);
        return item;
    }
}

template <typename T>
void CorrelationPool<T>::refill(std::vector<T>&& items) {
    for (auto& item : items) {
        u64 id = item.corr_id;
        SYMPSICA_REQUIRE(!available_.count(id) && !consumed_set_.count(id),
                          "CorrelationPool::refill: duplicate corr_id across pool lifetime");
        order_.push_back(id);
        auto pos = std::prev(order_.end());
        available_.emplace(id, std::make_pair(std::move(item), pos));
        ++generated_;
    }
}

// Explicit instantiation for the two production correlation types (R2):
// keeps the pool implementation out of the header despite being a template.
template class CorrelationPool<Triple>;
template class CorrelationPool<ZtGate>;

} // namespace detail
} // namespace sympsica
