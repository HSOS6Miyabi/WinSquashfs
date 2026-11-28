// SPDX-License-Identifier: GPL-3.0-only
#include "xz_index.hpp"
#include "tar_header.hpp"
#include "concurrent_cache.hpp"
#include <lzma.h>
#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace archivefs {
namespace {
constexpr std::size_t buffer_size=64u<<10;
struct LzmaStream {
    lzma_stream value=LZMA_STREAM_INIT;
    LzmaStream()=default;
    LzmaStream(const LzmaStream&)=delete;
    LzmaStream& operator=(const LzmaStream&)=delete;
    ~LzmaStream(){lzma_end(&value);}
};
struct IndexDeleter {void operator()(lzma_index* index)const noexcept{lzma_index_end(index,nullptr);}};
using Index=std::unique_ptr<lzma_index,IndexDeleter>;
void require_ok(lzma_ret rc,const char* message){
    if(rc==LZMA_MEM_ERROR)throw std::bad_alloc();
    if(rc!=LZMA_OK)throw std::runtime_error(std::string(message)+": "+std::to_string(rc));
}
Index read_index(const RandomAccessSource& source,std::size_t budget){
    auto input=source.begin_read();
    LzmaStream stream;
    lzma_index* result=nullptr;
    require_ok(lzma_file_info_decoder(&stream.value,&result,budget,source.size()),"cannot initialize XZ index reader");
    std::array<std::byte,buffer_size> buffer{};
    auto& z=stream.value;
    std::uint64_t next=0;
    for(;;){
        if(!z.avail_in&&next<source.size()){
            auto count=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),source.size()-next));
            input->read_exact(next,std::span(buffer).first(count));
            z.next_in=reinterpret_cast<const uint8_t*>(buffer.data());z.avail_in=count;next+=count;
        }
        const auto before=z.avail_in;
        const auto rc=lzma_code(&z,LZMA_RUN);
        if(rc==LZMA_STREAM_END){
            Index index(result);
            if(!index||lzma_index_file_size(index.get())!=source.size()||
               lzma_index_uncompressed_size(index.get())>static_cast<std::uint64_t>(INT64_MAX))
                throw std::runtime_error("invalid XZ index size");
            input->finish();return index;
        }
        if(rc==LZMA_SEEK_NEEDED){
            if(z.seek_pos>source.size())throw std::runtime_error("XZ index seeks outside source");
            next=z.seek_pos;z.next_in=nullptr;z.avail_in=0;continue;
        }
        if(rc==LZMA_MEMLIMIT_ERROR){input->finish();return {};}
        require_ok(rc,"invalid XZ index");
        if(before==z.avail_in)throw std::runtime_error("XZ index reader did not advance");
    }
}
struct Filters {
    std::array<lzma_filter,LZMA_FILTERS_MAX+1> values{};
    Filters(){for(auto& f:values)f.id=LZMA_VLI_UNKNOWN;}
    ~Filters(){lzma_filters_free(values.data(),nullptr);}
};
class BlockDecoder {
public:
    BlockDecoder(const ReadSession& source,std::uint64_t source_size,const lzma_index_iter& it)
        :number_(it.block.number_in_file),size_(it.block.uncompressed_size){
        auto start=it.block.compressed_file_offset,total=it.block.total_size;
        if(start>source_size||total>source_size-start||total<LZMA_BLOCK_HEADER_SIZE_MIN||
           !it.stream.flags||!lzma_check_is_supported(it.stream.flags->check))
            throw std::runtime_error("invalid XZ block location or check");
        std::array<std::byte,LZMA_BLOCK_HEADER_SIZE_MAX> header{};
        source.read_exact(start,std::span(header).first(1));
        if(header[0]==std::byte{})throw std::runtime_error("missing XZ block header");
        block_.version=0;
        block_.header_size=lzma_block_header_size_decode(std::to_integer<uint8_t>(header[0]));
        block_.check=it.stream.flags->check;
        block_.filters=filters_.values.data();
        if(block_.header_size>total)throw std::runtime_error("XZ block header exceeds block size");
        source.read_exact(start,std::span(header).first(block_.header_size));
        require_ok(lzma_block_header_decode(&block_,nullptr,reinterpret_cast<const uint8_t*>(header.data())),"invalid XZ block header");
        if(block_.uncompressed_size!=LZMA_VLI_UNKNOWN&&block_.uncompressed_size!=size_)
            throw std::runtime_error("XZ block size differs from index");
        block_.uncompressed_size=size_;
        require_ok(lzma_block_compressed_size(&block_,it.block.unpadded_size),"invalid XZ block compressed size");
        if(lzma_block_total_size(&block_)!=total)throw std::runtime_error("inconsistent XZ block size");
        const auto memory=lzma_raw_decoder_memusage(block_.filters);
        if(memory==UINT64_MAX||memory>std::numeric_limits<std::size_t>::max())
            throw std::length_error("XZ decoder dictionary exceeds address space");
        require_ok(lzma_block_decoder(&stream_.value,&block_),"cannot initialize XZ block decoder");
        next_=start+block_.header_size;end_=start+total;
    }
    std::uint64_t number()const noexcept{return number_;}
    std::uint64_t position()const noexcept{return position_;}
    void read_at(const ReadSession& source,std::uint64_t offset,std::span<std::byte> output){
        if(offset<position_||offset>size_||output.size()>size_-offset)throw std::out_of_range("invalid XZ block read");
        std::array<std::byte,buffer_size> discard{};
        while(position_<offset){
            auto n=static_cast<std::size_t>(std::min<std::uint64_t>(discard.size(),offset-position_));
            exact(source,std::span(discard).first(n));
        }
        exact(source,output);
        if(position_==size_&&!eof_){
            // Consume Block Padding and Check bytes even when the caller's
            // buffer ends exactly on the final uncompressed byte.
            std::byte extra{};
            if(pump(source,std::span(&extra,1))!=0||!eof_)
                throw std::runtime_error("XZ block exceeds indexed size");
        }
    }
private:
    void exact(const ReadSession& source,std::span<std::byte> output){
        while(!output.empty()){
            auto n=pump(source,output);
            if(!n)throw std::runtime_error("short XZ block");
            output=output.subspan(n);
        }
    }
    std::size_t pump(const ReadSession& source,std::span<std::byte> output){
        if(eof_||output.empty())return 0;
        auto& z=stream_.value;
        z.next_out=reinterpret_cast<uint8_t*>(output.data());z.avail_out=output.size();
        while(z.avail_out&&!eof_){
            if(!z.avail_in&&next_<end_){
                auto n=static_cast<std::size_t>(std::min<std::uint64_t>(input_.size(),end_-next_));
                source.read_exact(next_,std::span(input_).first(n));next_+=n;
                z.next_in=reinterpret_cast<const uint8_t*>(input_.data());z.avail_in=n;
            }
            auto before=z.avail_in,out=z.avail_out;
            auto rc=lzma_code(&z,LZMA_RUN);
            auto n=out-z.avail_out;
            if(n>size_-position_)throw std::runtime_error("XZ block exceeds indexed size");
            position_+=n;
            if(rc==LZMA_STREAM_END){
                if(position_!=size_||next_-z.avail_in!=end_)throw std::runtime_error("XZ block length differs from index");
                eof_=true;break;
            }
            require_ok(rc,"XZ block decompression failed");
            if(before==z.avail_in&&!n)throw std::runtime_error("truncated XZ block");
        }
        return output.size()-z.avail_out;
    }
    Filters filters_;
    lzma_block block_{};
    LzmaStream stream_;
    std::array<std::byte,buffer_size> input_{};
    const std::uint64_t number_,size_;
    std::uint64_t next_=0,end_=0,position_=0;
    bool eof_=false;
};
class XzSource final:public RandomAccessSource {
    using Cache=ConcurrentCache<std::uint64_t,std::byte>;
public:
    XzSource(std::shared_ptr<const RandomAccessSource> source,Index index,std::size_t budget)
        :source_(std::move(source)),index_(std::move(index)),
         index_bytes_(static_cast<std::size_t>(lzma_index_memused(index_.get()))),
         block_budget_(std::min<std::size_t>(budget-std::min(budget,index_bytes_),16u<<20)),cache_(block_budget_){}
    std::uint64_t size()const noexcept override{return lzma_index_uncompressed_size(index_.get());}
    std::string filename()const override{return source_->filename();}
    void validate()const override{source_->validate();}
    void validate_path()const override{source_->validate_path();}
    bool source_changed()const noexcept override{return source_->source_changed();}
    std::size_t reserved()const noexcept{return index_bytes_+block_budget_;}
private:
    class Session final:public ReadSession {
    public:
        explicit Session(const XzSource& owner):owner_(owner){}
        void read_exact(std::uint64_t offset,std::span<std::byte> output)const override{
            if(finished_)throw std::logic_error("XZ read session already finished");
            if(offset>owner_.size()||output.size()>owner_.size()-offset)throw std::out_of_range("XZ read outside stream");
            while(!output.empty()){
                lzma_index_iter it;
                lzma_index_iter_init(&it,owner_.index_.get());
                if(lzma_index_iter_locate(&it,offset))throw std::runtime_error("XZ block lookup failed");
                auto within=offset-it.block.uncompressed_file_offset;
                auto count=static_cast<std::size_t>(std::min<std::uint64_t>(output.size(),it.block.uncompressed_size-within));
                if(!count)throw std::runtime_error("empty XZ block at requested offset");
                if(it.block.uncompressed_size<=owner_.block_budget_){
                    auto value=owner_.cache_.get(it.block.number_in_file,[&]{
                        Cache::Buffer bytes(static_cast<std::size_t>(it.block.uncompressed_size));
                        BlockDecoder decoder(input(),owner_.source_->size(),it);
                        decoder.read_at(input(),0,bytes);
                        owner_.source_->validate();
                        return bytes;
                    });
                    std::copy_n(value->data()+static_cast<std::size_t>(within),count,output.data());
                }else{
                    owner_.read_large(input(),it,within,output.first(count));
                }
                offset+=count;output=output.subspan(count);
            }
        }
        void finish()override{
            if(finished_)return;
            if(input_)input_->finish();
            else owner_.source_->validate();
            input_.reset();finished_=true;
        }
    private:
        ReadSession& input()const{
            if(!input_)input_=owner_.source_->begin_read();
            return *input_;
        }
        const XzSource& owner_;
        mutable std::unique_ptr<ReadSession> input_;
        bool finished_=false;
    };
    void read_large(const ReadSession& source,const lzma_index_iter& it,std::uint64_t offset,std::span<std::byte> output)const{
        // Large blocks must not require an equally large allocation. Retain
        // one streaming decoder and reuse it for forward reads in that block.
        std::lock_guard lock(cursor_mutex_);
        try{
            if(!cursor_||cursor_->number()!=it.block.number_in_file||cursor_->position()>offset)
                cursor_=std::make_unique<BlockDecoder>(source,source_->size(),it);
            cursor_->read_at(source,offset,output);
        }catch(...){cursor_.reset();throw;}
    }
public:
    std::unique_ptr<ReadSession> begin_read()const override{return std::make_unique<Session>(*this);}
private:
    std::shared_ptr<const RandomAccessSource> source_;
    Index index_;
    const std::size_t index_bytes_,block_budget_;
    mutable Cache cache_;
    mutable std::mutex cursor_mutex_;
    mutable std::unique_ptr<BlockDecoder> cursor_;
};
}
IndexedXz index_xz_tar(std::shared_ptr<const RandomAccessSource> source,std::size_t budget){
    if(!source||source->size()<6||budget<(128u<<10))return {};
    std::array<std::byte,6> magic{};
    source->read_exact(0,magic);
    constexpr std::array signature{std::byte{0xfd},std::byte{0x37},std::byte{0x7a},std::byte{0x58},std::byte{0x5a},std::byte{0}};
    if(magic!=signature)return {};
    auto index=read_index(*source,budget);
    if(!index||lzma_index_uncompressed_size(index.get())<512||lzma_index_memused(index.get())>budget)return {};
    auto indexed=std::make_shared<XzSource>(source,std::move(index),budget);
    std::array<std::byte,512> header{};
    indexed->read_exact(0,header);
    if(!tar_header(header,source->filename()))return {};
    return {indexed,indexed->reserved()};
}
}
