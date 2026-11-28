// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "random_access_source.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <utility>

namespace archivefs {
struct OpenProgress {
    explicit OpenProgress(std::u16string name):label(std::move(name)){}
    const std::u16string label;
    const std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> size{0},offset{0};
    std::atomic<bool> done{false};
    void advance(std::uint64_t end) noexcept {
        auto current=offset.load(std::memory_order_relaxed);
        while(current<end&&!offset.compare_exchange_weak(current,end,std::memory_order_relaxed)){}
    }
};
using OpenProgressSink=std::function<void(std::shared_ptr<OpenProgress>)>;

class ProgressSource final:public RandomAccessSource {
    class Session final:public ReadSession {
    public:
        Session(std::unique_ptr<ReadSession> inner,std::weak_ptr<OpenProgress> progress)
            :inner_(std::move(inner)),progress_(std::move(progress)){}
        void read_exact(std::uint64_t offset,std::span<std::byte> data)const override{
            inner_->read_exact(offset,data);
            if(auto p=progress_.lock();p&&!p->done.load(std::memory_order_relaxed))
                p->advance(offset+data.size());
        }
        void finish()override{inner_->finish();}
    private:
        std::unique_ptr<ReadSession> inner_;
        std::weak_ptr<OpenProgress> progress_;
    };
public:
    ProgressSource(std::shared_ptr<const RandomAccessSource> source,std::shared_ptr<OpenProgress> progress)
        :source_(std::move(source)),progress_(progress){
        if(progress)progress->size.store(source_->size(),std::memory_order_relaxed);
    }
    std::uint64_t size()const noexcept override{return source_->size();}
    std::unique_ptr<ReadSession> begin_read()const override{return std::make_unique<Session>(source_->begin_read(),progress_);}
    void validate()const override{source_->validate();}
    void validate_path()const override{source_->validate_path();}
    bool source_changed()const noexcept override{return source_->source_changed();}
private:
    std::shared_ptr<const RandomAccessSource> source_;
    std::weak_ptr<OpenProgress> progress_;
};
}
