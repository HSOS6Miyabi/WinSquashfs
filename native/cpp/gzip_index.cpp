// SPDX-License-Identifier: GPL-3.0-only
#include "gzip_index.hpp"
#include "tar_header.hpp"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace archivefs {
namespace {
constexpr std::size_t checkpoint_cost=64u<<10;
constexpr std::uint64_t initial_spacing=4u<<20;
constexpr std::size_t input_size=64u<<10;

void zcheck(int rc){
    if(rc==Z_MEM_ERROR)throw std::bad_alloc();
    if(rc!=Z_OK)throw std::runtime_error("GZIP decoder initialization failed: "+std::to_string(rc));
}
struct ZState {
    z_stream z{};
    ZState(){zcheck(inflateInit2(&z,MAX_WBITS+16));}
    explicit ZState(const ZState& original){
        zcheck(inflateCopy(&z,const_cast<z_stream*>(&original.z)));
        // The copied decoder retains its bit buffer and sliding dictionary.
        // Input is supplied again at the first unconsumed compressed byte.
        z.next_in=nullptr;z.avail_in=0;z.next_out=nullptr;z.avail_out=0;
    }
    ZState& operator=(const ZState&)=delete;
    ~ZState(){inflateEnd(&z);}
};
struct Checkpoint {
    std::uint64_t logical,compressed;
    std::unique_ptr<ZState> state;
};
class Decoder {
public:
    Decoder():state_(std::make_unique<ZState>()){}
    explicit Decoder(const Checkpoint& cp)
        :state_(std::make_unique<ZState>(*cp.state)),next_input_(cp.compressed),position_(cp.logical){}
    std::uint64_t position()const noexcept{return position_;}
    bool eof()const noexcept{return eof_;}
    Checkpoint checkpoint()const{
        return {position_,next_input_-state_->z.avail_in,std::make_unique<ZState>(*state_)};
    }
    std::size_t read(const ReadSession& source,std::uint64_t source_size,std::span<std::byte> output){
        const auto length=output.size();
        while(!output.empty()&&!eof_){
            fill(source,source_size);
            auto& z=state_->z;
            const auto amount=static_cast<uInt>(std::min<std::size_t>(output.size(),1u<<20));
            z.next_out=reinterpret_cast<Bytef*>(output.data());z.avail_out=amount;
            const auto before=z.avail_in;
            const auto rc=inflate(&z,Z_NO_FLUSH);
            const auto written=amount-z.avail_out;
            if(written>static_cast<std::uint64_t>(INT64_MAX)-position_)
                throw std::length_error("GZIP uncompressed stream exceeds signed 64-bit offsets");
            position_+=written;output=output.subspan(written);
            if(rc==Z_STREAM_END){next_member(source,source_size);continue;}
            if(rc==Z_MEM_ERROR)throw std::bad_alloc();
            if(rc!=Z_OK&&rc!=Z_BUF_ERROR)
                throw std::runtime_error(z.msg?z.msg:"invalid GZIP stream");
            if(!written&&before==z.avail_in)throw std::runtime_error("truncated GZIP stream");
        }
        return length-output.size();
    }
    void exact(const ReadSession& source,std::uint64_t size,std::span<std::byte> output){
        if(read(source,size,output)!=output.size())throw std::runtime_error("short GZIP read");
    }
private:
    void fill(const ReadSession& source,std::uint64_t size){
        auto& z=state_->z;
        if(z.avail_in||next_input_==size)return;
        const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(input_.size(),size-next_input_));
        source.read_exact(next_input_,std::as_writable_bytes(std::span(input_).first(count)));
        z.next_in=input_.data();z.avail_in=static_cast<uInt>(count);next_input_+=count;
    }
    void next_member(const ReadSession& source,std::uint64_t size){
        auto& z=state_->z;
        // Accept concatenated GZIP members and zero padding. Each member is
        // checked by zlib before advancing to the next one.
        for(;;){
            fill(source,size);
            if(!z.avail_in){eof_=true;return;}
            while(z.avail_in&&*z.next_in==0){++z.next_in;--z.avail_in;}
            if(z.avail_in)break;
        }
        auto* input=z.next_in;const auto remaining=z.avail_in;
        zcheck(inflateReset2(&z,MAX_WBITS+16));
        z.next_in=input;z.avail_in=remaining;z.next_out=nullptr;z.avail_out=0;
    }
    std::unique_ptr<ZState> state_;
    std::array<Bytef,input_size> input_{};
    std::uint64_t next_input_=0,position_=0;
    bool eof_=false;
};

class GzipSource final:public RandomAccessSource {
public:
    GzipSource(std::shared_ptr<const RandomAccessSource> source,std::size_t budget)
        :source_(std::move(source)){
        // The 64 KiB reservation per snapshot covers zlib's 32 KiB history,
        // copied inflate state and checkpoint metadata.
        const auto maximum=std::min<std::size_t>(budget/checkpoint_cost,16384);
        if(maximum<2)throw std::invalid_argument("insufficient GZIP index budget");
        auto input=source_->begin_read();
        Decoder decoder;
        std::array<std::byte,input_size> scratch{};
        auto spacing=initial_spacing;
        while(!decoder.eof()){
            const auto offset=decoder.position();
            const auto remaining=spacing-offset%spacing;
            const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(),remaining));
            decoder.read(*input,source_->size(),std::span(scratch).first(count));
            const auto current=decoder.position();
            if(decoder.eof()||current%spacing)continue;
            if(points_.size()>=maximum){
                // Increase spacing on very large streams, retaining every
                // second snapshot. Memory never grows with the file size.
                if(spacing>static_cast<std::uint64_t>(INT64_MAX)/2)
                    throw std::length_error("GZIP index interval overflow");
                spacing*=2;
                std::erase_if(points_,[&](const Checkpoint& cp){return cp.logical%spacing!=0;});
            }
            if(current%spacing==0)points_.push_back(decoder.checkpoint());
        }
        size_=decoder.position();
        input->finish();source_->validate();
    }
    std::uint64_t size()const noexcept override{return size_;}
    std::string filename()const override{return source_->filename();}
    void validate()const override{source_->validate();}
    void validate_path()const override{source_->validate_path();}
    bool source_changed()const noexcept override{return source_->source_changed();}
    std::size_t reserved()const noexcept{return points_.size()*checkpoint_cost;}
private:
    const Checkpoint* nearest(std::uint64_t offset)const noexcept{
        auto it=std::upper_bound(points_.begin(),points_.end(),offset,
            [](std::uint64_t p,const Checkpoint& cp){return p<cp.logical;});
        return it==points_.begin()?nullptr:&*std::prev(it);
    }
    std::unique_ptr<Decoder> fresh(const Checkpoint* cp)const{
        return cp?std::make_unique<Decoder>(*cp):std::make_unique<Decoder>();
    }
    std::unique_ptr<Decoder> acquire(std::uint64_t offset,const Checkpoint* cp)const{
        const auto minimum=cp?cp->logical:0;
        {
            std::lock_guard lock(mutex_);
            std::unique_ptr<Decoder>* best=nullptr;
            for(auto& slot:idle_)if(slot&&slot->position()>=minimum&&slot->position()<=offset&&
                (!best||slot->position()>(*best)->position()))best=&slot;
            if(best)return std::move(*best);
        }
        return fresh(cp);
    }
    void retain(std::unique_ptr<Decoder> decoder)const{
        if(!decoder||decoder->eof())return;
        {
            std::lock_guard lock(mutex_);
            for(auto& slot:idle_)if(!slot){slot=std::move(decoder);return;}
            std::swap(idle_[replace_],decoder);
            replace_=(replace_+1)%idle_.size();
        }
    }
    class Session final:public ReadSession {
    public:
        explicit Session(const GzipSource& source):source_(source){}
        void read_exact(std::uint64_t offset,std::span<std::byte> output)const override{
            if(finished_)throw std::logic_error("GZIP read session already finished");
            if(offset>source_.size_||output.size()>source_.size_-offset)
                throw std::out_of_range("GZIP read outside uncompressed stream");
            if(output.empty())return;
            if(!input_)input_=source_.source_->begin_read();
            const auto* cp=source_.nearest(offset);
            if(!decoder_)decoder_=source_.acquire(offset,cp);
            else if(decoder_->position()>offset||(cp&&decoder_->position()<cp->logical))
                decoder_=source_.fresh(cp);
            std::array<std::byte,input_size> scratch{};
            while(decoder_->position()<offset){
                const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(),offset-decoder_->position()));
                decoder_->exact(*input_,source_.source_->size(),std::span(scratch).first(count));
            }
            decoder_->exact(*input_,source_.source_->size(),output);
        }
        void finish()override{
            if(finished_)return;
            if(input_)input_->finish();
            input_.reset();finished_=true;
            source_.retain(std::move(decoder_));
        }
    private:
        const GzipSource& source_;
        mutable std::unique_ptr<ReadSession> input_;
        mutable std::unique_ptr<Decoder> decoder_;
        bool finished_=false;
    };
public:
    std::unique_ptr<ReadSession> begin_read()const override{return std::make_unique<Session>(*this);}
private:
    std::shared_ptr<const RandomAccessSource> source_;
    std::uint64_t size_=0;
    std::vector<Checkpoint> points_;
    mutable std::mutex mutex_;
    mutable std::array<std::unique_ptr<Decoder>,2> idle_;
    mutable std::size_t replace_=0;
};
}
IndexedGzip index_gzip_tar(std::shared_ptr<const RandomAccessSource> source,std::size_t budget){
    if(!source||source->size()<2||budget<2*checkpoint_cost)return {};
    std::array<std::byte,2> magic{};
    source->read_exact(0,magic);
    if(magic[0]!=std::byte{0x1f}||magic[1]!=std::byte{0x8b})return {};
    {
        auto input=source->begin_read();Decoder decoder;
        std::array<std::byte,512> header{};
        const auto n=decoder.read(*input,source->size(),header);
        input->finish();
        if(!tar_header(std::span(header).first(n),source->filename()))return {};
    }
    auto indexed=std::make_shared<GzipSource>(std::move(source),budget);
    return {indexed,indexed->reserved()};
}
}
