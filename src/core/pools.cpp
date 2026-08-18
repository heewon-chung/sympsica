#include "sympsica/core/pools.hpp"

#include <utility>

#include "sympsica/utils/common.hpp"

namespace sympsica {
namespace detail {

template <typename T>
T CorrelationPool<T>::take() {
    SYMPSICA_REQUIRE(!order_.empty(), "CorrelationPool::take: pool exhausted");
    u64 id = order_.front();
    auto it = available_.find(id);
    T item = std::move(it->second.first);
    order_.erase(it->second.second);
    available_.erase(it);
    consumed_set_.insert(id);
    consumed_log_.push_back(id);
    return item;
}

template <typename T>
T CorrelationPool<T>::take_by_id(u64 corr_id) {
    SYMPSICA_REQUIRE(!consumed_set_.count(corr_id),
                      "CorrelationPool::take_by_id: corr_id already consumed (double-consume)");
    auto it = available_.find(corr_id);
    SYMPSICA_REQUIRE(it != available_.end(),
                      "CorrelationPool::take_by_id: unknown corr_id (never refilled)");
    T item = std::move(it->second.first);
    order_.erase(it->second.second);
    available_.erase(it);
    consumed_set_.insert(corr_id);
    consumed_log_.push_back(corr_id);
    return item;
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
