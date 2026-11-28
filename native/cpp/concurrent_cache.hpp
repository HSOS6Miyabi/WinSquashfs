// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace archivefs {

// Immutable, byte-budgeted LRU cache. A shared future combines simultaneous
// requests for the same key, while unrelated keys load outside the mutex.
template<class Key, class Element, class Hash = std::hash<Key>>
class ConcurrentCache {
public:
    using Buffer = std::vector<Element>;
    using Value = std::shared_ptr<const Buffer>;
    struct Statistics { std::uint64_t hits, misses, evictions; std::size_t bytes, entries; };

    explicit ConcurrentCache(std::size_t budget, std::size_t entry_limit = 65536)
        : budget_(budget), entry_limit_(entry_limit) {}
    ConcurrentCache(const ConcurrentCache&) = delete;
    ConcurrentCache& operator=(const ConcurrentCache&) = delete;

    template<class Loader>
    Value get(const Key& key, Loader&& loader) {
        std::shared_future<Value> future;
        std::shared_ptr<std::promise<Value>> producer;
        {
            std::lock_guard lock(mutex_);
            auto it = entries_.find(key);
            if (it != entries_.end()) {
                ++hits_;
                if (it->second.ready)
                    lru_.splice(lru_.begin(), lru_, it->second.position);
                future = it->second.future;
            } else {
                ++misses_;
                producer = std::make_shared<std::promise<Value>>();
                future = producer->get_future().share();
                entries_.emplace(key, Entry{future, {}, 0, false});
            }
        }
        if (!producer) return future.get();

        try {
            auto value = std::make_shared<const Buffer>(std::forward<Loader>(loader)());
            const auto bytes = value->capacity() * sizeof(Element);
            {
                std::lock_guard lock(mutex_);
                auto it = entries_.find(key);
                if (bytes <= budget_ && entry_limit_ != 0) {
                    // Evict before adding to avoid unsigned arithmetic overflow.
                    while (!lru_.empty() &&
                           (resident_ > budget_ - bytes || lru_.size() >= entry_limit_)) {
                        auto victim = entries_.find(lru_.back());
                        resident_ -= victim->second.bytes;
                        entries_.erase(victim);
                        lru_.pop_back();
                        ++evictions_;
                    }
                    lru_.push_front(key);
                    it->second.position = lru_.begin();
                    it->second.bytes = bytes;
                    it->second.ready = true;
                    resident_ += bytes;
                    producer->set_value(value);
                } else {
                    producer->set_value(value);
                    entries_.erase(it);
                }
            }
            return value;
        } catch (...) {
            const auto error = std::current_exception();
            {
                std::lock_guard lock(mutex_);
                auto it = entries_.find(key);
                if (it != entries_.end()) {
                    if (it->second.ready) {
                        resident_ -= it->second.bytes;
                        lru_.erase(it->second.position);
                    }
                    entries_.erase(it);
                }
                producer->set_exception(error);
            }
            std::rethrow_exception(error);
        }
    }

    Statistics statistics() const {
        std::lock_guard lock(mutex_);
        return {hits_, misses_, evictions_, resident_, lru_.size()};
    }

private:
    struct Entry {
        std::shared_future<Value> future;
        typename std::list<Key>::iterator position;
        std::size_t bytes;
        bool ready;
    };
    const std::size_t budget_, entry_limit_;
    mutable std::mutex mutex_;
    std::unordered_map<Key, Entry, Hash> entries_;
    std::list<Key> lru_;
    std::size_t resident_ = 0;
    std::uint64_t hits_ = 0, misses_ = 0, evictions_ = 0;
};

} // namespace archivefs
