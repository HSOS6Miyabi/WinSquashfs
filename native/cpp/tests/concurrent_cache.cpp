// SPDX-License-Identifier: GPL-3.0-only
#include "../concurrent_cache.hpp"
#include "../cache_budget.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using Cache = archivefs::ConcurrentCache<int, unsigned char>;
using namespace std::chrono_literals;
static void require(bool condition) { if (!condition) throw std::runtime_error("cache test failed"); }

int main() {
    Cache cache(64, 8);
    std::atomic<int> calls{0};
    std::barrier start(16);
    std::vector<std::jthread> threads;
    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            auto value = cache.get(1, [&] {
                ++calls;
                std::this_thread::sleep_for(25ms);
                return Cache::Buffer(16, 42);
            });
            require(value->size() == 16 && value->front() == 42);
        });
    }
    threads.clear();
    require(calls == 1 && cache.statistics().hits == 15);

    std::barrier parallel(2);
    std::atomic<int> entered{0};
    auto task = [&](int key) {
        cache.get(key, [&] {
            ++entered;
            parallel.arrive_and_wait();
            return Cache::Buffer(16, static_cast<unsigned char>(key));
        });
    };
    std::jthread one(task, 2), two(task, 3);
    one.join(); two.join();
    require(entered == 2);

    cache.get(4, [] { return Cache::Buffer(16, 4); });
    cache.get(1, []() -> Cache::Buffer { throw std::runtime_error("unexpected reload"); });
    auto lease = cache.get(2, []() -> Cache::Buffer { throw std::runtime_error("unexpected reload"); });
    cache.get(5, [] { return Cache::Buffer(32, 5); });
    require(cache.statistics().bytes <= 64 && cache.statistics().evictions == 2);
    require(lease->front() == 2);
    auto reload = cache.get(3, [] { return Cache::Buffer(16, 33); });
    require(reload->front() == 33);

    int retries = 0;
    try {
        cache.get(99, [&]() -> Cache::Buffer { ++retries; throw std::runtime_error("decode error"); });
        require(false);
    } catch (const std::runtime_error&) {}
    require(cache.get(99, [&] { ++retries; return Cache::Buffer(1, 99); })->front() == 99);
    require(retries == 2);

    cache.get(8, [] { return Cache::Buffer(128, 8); });
    require(cache.get(8, [] { return Cache::Buffer(128, 9); })->front() == 9);
    require(cache.statistics().bytes <= 64);
    Cache empty(0, 0);
    empty.get(1, [] { return Cache::Buffer(1, 0); });
    require(empty.statistics().entries == 0 && empty.statistics().bytes == 0);

    Cache count_limit(1000, 2);
    for (int i = 0; i < 50; ++i) count_limit.get(i, [] { return Cache::Buffer(); });
    require(count_limit.statistics().entries == 2);
    auto budget=std::make_shared<archivefs::CacheBudget>(100);
    {
        auto first=std::make_shared<archivefs::CacheBudget::Reservation>(budget,64);
        auto second=std::make_shared<archivefs::CacheBudget::Reservation>(budget,64);
        require(first->bytes()==64&&second->bytes()==36&&budget->reserved()==100);
        auto no_cache=std::make_shared<archivefs::CacheBudget::Reservation>(budget,64);
        require(no_cache->bytes()==0);
        first.reset();
        auto third=std::make_shared<archivefs::CacheBudget::Reservation>(budget,64);
        require(third->bytes()==64&&budget->reserved()==100);
    }
    require(budget->reserved()==0);
    std::barrier reserved(16);
    for(int i=0;i<16;++i)threads.emplace_back([&]{
        archivefs::CacheBudget::Reservation reservation(budget,20);
        reserved.arrive_and_wait();
        require(budget->reserved()<=100);
    });
    threads.clear();
    require(budget->reserved()==0);
    std::cout << "PASS coalescing, independent loads, LRU, byte and entry budgets, lease lifetime, failure retry\n";
}
