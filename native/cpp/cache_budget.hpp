// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace archivefs {
class CacheBudget {
public:
    explicit CacheBudget(std::size_t limit):limit_(limit){}
    class Reservation {
    public:
        Reservation(std::shared_ptr<CacheBudget> budget,std::size_t requested)
            :budget_(std::move(budget)),bytes_(budget_->claim(requested)){}
        Reservation(const Reservation&)=delete;
        Reservation& operator=(const Reservation&)=delete;
        ~Reservation(){budget_->used_.fetch_sub(bytes_,std::memory_order_relaxed);}
        std::size_t bytes()const noexcept{return bytes_;}
    private:
        std::shared_ptr<CacheBudget> budget_;
        std::size_t bytes_;
    };
    std::size_t reserved()const noexcept{return used_.load(std::memory_order_relaxed);}
private:
    std::size_t claim(std::size_t requested)noexcept{
        auto used=used_.load(std::memory_order_relaxed);
        for(;;){
            const auto bytes=std::min(requested,limit_-used);
            if(used_.compare_exchange_weak(used,used+bytes,std::memory_order_relaxed))return bytes;
        }
    }
    const std::size_t limit_;
    std::atomic<std::size_t> used_{0};
};
}
