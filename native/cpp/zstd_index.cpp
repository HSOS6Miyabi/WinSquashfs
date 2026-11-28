// SPDX-License-Identifier: GPL-3.0-only
#include "zstd_index.hpp"
#include "tar_header.hpp"
#include "concurrent_cache.hpp"
#include <zstd.h>
#include <xxhash.h>
#include <array>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace archivefs {
namespace {
constexpr std::size_t chunk_size=64u<<10;
constexpr std::uint32_t seek_magic=0x8f92eab1,table_magic=0x184d2a5e;
std::uint32_t le32(std::span<const std::byte> data){
    return std::to_integer<std::uint32_t>(data[0])|
           (std::to_integer<std::uint32_t>(data[1])<<8)|
           (std::to_integer<std::uint32_t>(data[2])<<16)|
           (std::to_integer<std::uint32_t>(data[3])<<24);
}
struct Frame {
    std::uint64_t logical,physical;
    std::uint32_t compressed,uncompressed,checksum;
    bool checked;
};
struct Table {
    std::vector<Frame> frames;
    std::uint64_t logical_size=0;
};
void verify_empty_frame(const ReadSession& source,const Frame& frame);
std::optional<Table> read_table(const RandomAccessSource& source,std::size_t budget){
    if(source.size()<17)return {};
    auto input=source.begin_read();
    std::array<std::byte,4> first{};
    input->read_exact(0,first);
    auto magic=le32(first);
    if(magic!=ZSTD_MAGICNUMBER&&(magic&0xfffffff0u)!=0x184d2a50u){input->finish();return {};}
    std::array<std::byte,9> footer{};
    input->read_exact(source.size()-footer.size(),footer);
    if(le32(std::span(footer).subspan(5))!=seek_magic){input->finish();return {};}
    const auto count=le32(footer);
    const auto descriptor=std::to_integer<unsigned>(footer[4]);
    if(descriptor&0x7c)throw std::runtime_error("unsupported Zstandard seek table descriptor");
    const bool checked=(descriptor&0x80)!=0;
    const std::uint64_t stride=checked?12:8;
    const auto table_size=static_cast<std::uint64_t>(count)*stride+9;
    if(table_size>UINT32_MAX||table_size+8>source.size())
        throw std::runtime_error("invalid Zstandard seek table size");
    const auto start=source.size()-table_size-8;
    std::array<std::byte,8> header{};
    input->read_exact(start,header);
    if(le32(header)!=table_magic||le32(std::span(header).subspan(4))!=table_size)
        throw std::runtime_error("invalid Zstandard seek table header");
    if(count>budget/sizeof(Frame)){input->finish();return {};}
    Table result;
    result.frames.reserve(count);
    std::array<std::byte,chunk_size> bytes{};
    std::uint64_t compressed=0,logical=0;
    for(std::uint64_t i=0;i<count;){
        auto entries=static_cast<std::size_t>(std::min<std::uint64_t>(count-i,bytes.size()/stride));
        auto read_size=static_cast<std::size_t>(entries*stride);
        input->read_exact(start+8+i*stride,std::span(bytes).first(read_size));
        for(std::size_t j=0;j<entries;++j){
            auto entry=std::span(bytes).subspan(static_cast<std::size_t>(j*stride));
            auto c=le32(entry),u=le32(entry.subspan(4));
            if(c<8||compressed>start||c>start-compressed||
               u>static_cast<std::uint64_t>(INT64_MAX)-logical)
                throw std::runtime_error("Zstandard seek table frame range exceeds stream");
            Frame frame{logical,compressed,c,u,checked?le32(entry.subspan(8)):0,checked};
            if(u)result.frames.push_back(frame);
            else verify_empty_frame(*input,frame);
            compressed+=c;logical+=u;
        }
        i+=entries;
    }
    if(compressed!=start)throw std::runtime_error("Zstandard seek table does not cover compressed frames");
    result.logical_size=logical;
    input->finish();
    return result;
}
void zcheck(std::size_t result){
    if(ZSTD_isError(result))throw std::runtime_error(ZSTD_getErrorName(result));
}
class Decoder {
public:
    explicit Decoder(const Frame& frame):frame_(frame),
        stream_(ZSTD_createDStream(),ZSTD_freeDStream),hash_(XXH64_createState(),XXH64_freeState),
        next_(frame.physical),end_(frame.physical+frame.compressed){
        if(!stream_||!hash_)throw std::bad_alloc();
        zcheck(ZSTD_initDStream(stream_.get()));
        if(frame_.checked&&XXH64_reset(hash_.get(),0)!=XXH_OK)throw std::runtime_error("cannot initialize Zstandard checksum");
    }
    std::uint64_t key()const noexcept{return frame_.physical;}
    std::uint64_t position()const noexcept{return position_;}
    void read_at(const ReadSession& source,std::uint64_t offset,std::span<std::byte> output){
        if(offset<position_||offset>frame_.uncompressed||output.size()>frame_.uncompressed-offset)
            throw std::out_of_range("invalid Zstandard frame read");
        std::array<std::byte,chunk_size> discard{};
        while(position_<offset){
            auto count=static_cast<std::size_t>(std::min<std::uint64_t>(discard.size(),offset-position_));
            exact(source,std::span(discard).first(count));
        }
        exact(source,output);
        if(position_==frame_.uncompressed&&!eof_){
            std::byte extra{};
            if(pump(source,std::span(&extra,1))||!eof_)throw std::runtime_error("Zstandard frame exceeds indexed size");
        }
    }
private:
    void exact(const ReadSession& source,std::span<std::byte> out){
        while(!out.empty()){
            auto count=pump(source,out);
            if(!count)throw std::runtime_error("short Zstandard frame");
            out=out.subspan(count);
        }
    }
    std::size_t pump(const ReadSession& source,std::span<std::byte> output){
        if(eof_||output.empty())return 0;
        ZSTD_outBuffer out{output.data(),output.size(),0};
        while(out.pos<out.size&&!eof_){
            if(input_.pos==input_.size&&next_<end_){
                auto count=static_cast<std::size_t>(std::min<std::uint64_t>(buffer_.size(),end_-next_));
                source.read_exact(next_,std::span(buffer_).first(count));
                next_+=count;input_={buffer_.data(),count,0};
            }
            auto before=input_.pos,previous=out.pos;
            auto remaining=ZSTD_decompressStream(stream_.get(),&out,&input_);
            zcheck(remaining);
            auto count=out.pos-previous;
            if(count>frame_.uncompressed-position_)throw std::runtime_error("Zstandard frame exceeds indexed size");
            position_+=count;
            if(frame_.checked&&count&&XXH64_update(hash_.get(),output.data()+previous,count)!=XXH_OK)
                throw std::runtime_error("Zstandard checksum update failed");
            if(!remaining){
                if(position_!=frame_.uncompressed||next_-(input_.size-input_.pos)!=end_)
                    throw std::runtime_error("Zstandard frame length differs from seek table");
                if(frame_.checked&&static_cast<std::uint32_t>(XXH64_digest(hash_.get()))!=frame_.checksum)
                    throw std::runtime_error("Zstandard seek table checksum mismatch");
                eof_=true;break;
            }
            if(before==input_.pos&&!count)throw std::runtime_error("truncated Zstandard frame");
        }
        return out.pos;
    }
    const Frame frame_;
    std::unique_ptr<ZSTD_DStream,decltype(&ZSTD_freeDStream)> stream_;
    std::unique_ptr<XXH64_state_t,decltype(&XXH64_freeState)> hash_;
    std::array<std::byte,chunk_size> buffer_{};
    ZSTD_inBuffer input_{nullptr,0,0};
    std::uint64_t next_,end_,position_=0;
    bool eof_=false;
};
void verify_empty_frame(const ReadSession& source,const Frame& frame){
    std::array<std::byte,8> header{};
    source.read_exact(frame.physical,header);
    if((le32(header)&0xfffffff0u)==0x184d2a50u){
        if(static_cast<std::uint64_t>(le32(std::span(header).subspan(4)))+8!=frame.compressed)
            throw std::runtime_error("Zstandard skippable frame length differs from seek table");
        if(frame.checked&&static_cast<std::uint32_t>(XXH64("",0,0))!=frame.checksum)
            throw std::runtime_error("Zstandard empty frame checksum mismatch");
    }else{
        Decoder decoder(frame);
        decoder.read_at(source,0,{});
    }
}
class ZstdSource final:public RandomAccessSource {
    using Cache=ConcurrentCache<std::uint64_t,std::byte>;
public:
    ZstdSource(std::shared_ptr<const RandomAccessSource> source,Table table,std::size_t budget)
        :source_(std::move(source)),table_(std::move(table)),
         index_bytes_(table_.frames.capacity()*sizeof(Frame)),
         frame_budget_(std::min<std::size_t>(budget-std::min(budget,index_bytes_),16u<<20)),cache_(frame_budget_){}
    std::uint64_t size()const noexcept override{return table_.logical_size;}
    std::string filename()const override{return source_->filename();}
    void validate()const override{source_->validate();}
    void validate_path()const override{source_->validate_path();}
    bool source_changed()const noexcept override{return source_->source_changed();}
    std::size_t reserved()const noexcept{return index_bytes_+frame_budget_;}
private:
    const Frame& locate(std::uint64_t offset)const{
        auto it=std::upper_bound(table_.frames.begin(),table_.frames.end(),offset,
            [](std::uint64_t p,const Frame& frame){return p<frame.logical;});
        if(it==table_.frames.begin())throw std::runtime_error("Zstandard frame lookup failed");
        --it;
        if(offset-it->logical>=it->uncompressed)throw std::runtime_error("Zstandard seek table gap");
        return *it;
    }
    void read_large(const ReadSession& source,const Frame& frame,std::uint64_t offset,std::span<std::byte> output)const{
        std::lock_guard lock(mutex_);
        try{
            if(!cursor_||cursor_->key()!=frame.physical||cursor_->position()>offset)
                cursor_=std::make_unique<Decoder>(frame);
            cursor_->read_at(source,offset,output);
        }catch(...){cursor_.reset();throw;}
    }
    class Session final:public ReadSession {
    public:
        explicit Session(const ZstdSource& owner):owner_(owner){}
        void read_exact(std::uint64_t offset,std::span<std::byte> output)const override{
            if(finished_)throw std::logic_error("Zstandard read session already finished");
            if(offset>owner_.size()||output.size()>owner_.size()-offset)throw std::out_of_range("Zstandard read outside stream");
            while(!output.empty()){
                const auto& frame=owner_.locate(offset);
                auto within=offset-frame.logical;
                auto count=static_cast<std::size_t>(std::min<std::uint64_t>(output.size(),frame.uncompressed-within));
                if(frame.uncompressed<=owner_.frame_budget_){
                    auto value=owner_.cache_.get(frame.physical,[&]{
                        Cache::Buffer bytes(frame.uncompressed);
                        Decoder decoder(frame);
                        decoder.read_at(input(),0,bytes);
                        owner_.source_->validate();
                        return bytes;
                    });
                    std::copy_n(value->data()+static_cast<std::size_t>(within),count,output.data());
                }else owner_.read_large(input(),frame,within,output.first(count));
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
        const ZstdSource& owner_;
        mutable std::unique_ptr<ReadSession> input_;
        bool finished_=false;
    };
public:
    std::unique_ptr<ReadSession> begin_read()const override{return std::make_unique<Session>(*this);}
private:
    std::shared_ptr<const RandomAccessSource> source_;
    const Table table_;
    const std::size_t index_bytes_,frame_budget_;
    mutable Cache cache_;
    mutable std::mutex mutex_;
    mutable std::unique_ptr<Decoder> cursor_;
};
}
IndexedZstd index_zstd_tar(std::shared_ptr<const RandomAccessSource> source,std::size_t budget){
    if(!source||budget<(128u<<10))return {};
    auto table=read_table(*source,budget);
    if(!table||table->logical_size<512)return {};
    auto indexed=std::make_shared<ZstdSource>(source,std::move(*table),budget);
    std::array<std::byte,512> header{};
    indexed->read_exact(0,header);
    if(!tar_header(header,source->filename()))return {};
    return {indexed,indexed->reserved()};
}
}
