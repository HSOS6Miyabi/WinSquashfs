// SPDX-License-Identifier: GPL-3.0-only
#include "decoder_backend.hpp"
#include "archive_tree.hpp"
#include "projection.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <atomic>
#include <future>
#include <iostream>
#include <random>

using namespace archivefs;
namespace {
void require(bool result,const char* message){if(!result)throw std::runtime_error(message);}
using Configure=int(*)(struct archive*);
std::vector<std::byte> make_archive(Configure format,Configure filter,std::span<const std::byte> data,std::string_view name="payload.bin"){
    std::unique_ptr<struct archive,decltype(&archive_write_free)> writer(archive_write_new(),archive_write_free);
    require(static_cast<bool>(writer),"allocate writer");
    auto* a=writer.get();
    auto check=[&](int r){if(r!=ARCHIVE_OK){auto* error=archive_error_string(a);throw std::runtime_error(error?error:"archive writer error");}};
    check(format(a));check(filter(a));
    std::vector<std::byte> output(data.size()*2+(1u<<20));
    std::size_t used=0;
    check(archive_write_set_bytes_per_block(a,0));
    check(archive_write_open_memory(a,output.data(),output.size(),&used));
    std::unique_ptr<struct archive_entry,decltype(&archive_entry_free)> entry(archive_entry_new(),archive_entry_free);
    require(static_cast<bool>(entry),"allocate entry");
    archive_entry_set_pathname(entry.get(),std::string(name).c_str());
    archive_entry_set_filetype(entry.get(),AE_IFREG);
    archive_entry_set_perm(entry.get(),0644);
    archive_entry_set_size(entry.get(),static_cast<la_int64_t>(data.size()));
    check(archive_write_header(a,entry.get()));
    require(archive_write_data(a,data.data(),data.size())==static_cast<la_ssize_t>(data.size()),"write fixture");
    check(archive_write_finish_entry(a));check(archive_write_close(a));
    output.resize(used);return output;
}
class MemorySource final:public RandomAccessSource {
    class Session final:public ReadSession {
    public:
        explicit Session(const MemorySource& source):source_(source){++source_.active;}
        ~Session()override{--source_.active;}
        void read_exact(std::uint64_t offset,std::span<std::byte> buffer)const override{
            if(offset>source_.data.size()||buffer.size()>source_.data.size()-offset)throw std::out_of_range("memory fixture");
            source_.bytes_read+=buffer.size();
            std::copy_n(source_.data.data()+static_cast<std::size_t>(offset),buffer.size(),buffer.data());
        }
        void finish()override{}
    private:const MemorySource& source_;
    };
public:
    explicit MemorySource(std::vector<std::byte> bytes):data(std::move(bytes)){}
    std::uint64_t size()const noexcept override{return data.size();}
    std::unique_ptr<ReadSession> begin_read()const override{return std::make_unique<Session>(*this);}
    void validate()const override{}
    mutable std::atomic<std::size_t> bytes_read{0},active{0};
    const std::vector<std::byte> data;
};
void exercise(Configure format,Configure filter,ArchiveFormat expected,const std::vector<std::byte>& payload,bool indexed=false){
    auto source=std::make_shared<MemorySource>(make_archive(format,filter,payload));
    auto backend=open_decoder_backend(source,{});
    require(backend->info().format==expected,"format identification");
    require(source->active==0,"no source session retained after indexing");
    auto root=backend->list(backend->info().root);
    require(root.size()==1&&root[0].name=="payload.bin","file tree");
    auto id=root[0].info.inode;
    require(root[0].info.size==payload.size(),"file size");
    if(indexed){
        require(source->bytes_read<payload.size()/2,"TAR indexing skips payload data");
        auto before=source->bytes_read.load();
        std::array<std::byte,123> sample{};
        backend->read_exact(id,700000,sample);
        require(std::equal(sample.begin(),sample.end(),payload.begin()+700000),"TAR physical offset");
        require(source->bytes_read-before==sample.size(),"TAR reads only requested bytes");
    }
    std::vector<std::future<void>> workers;
    for(unsigned worker=0;worker<6;++worker)workers.push_back(std::async(std::launch::async,[&,worker]{
        std::mt19937 rng(worker+32);
        for(unsigned i=0;i<12;++i){
            const auto offset=rng()%(payload.size()-257);
            std::array<std::byte,257> sample{};
            backend->read_exact(id,offset,sample);
            require(std::equal(sample.begin(),sample.end(),payload.begin()+offset),"concurrent random read");
        }
    }));
    for(auto& future:workers)future.get();
    require(source->active==0,"no source session retained after reading");
    auto tree=std::make_shared<ArchiveTree>(std::make_shared<ArchiveReader>(backend,1u<<20,256u<<10));
    auto mount=mount_projection(tree);
    auto entry=mount->open(u"\\payload.bin");
    require(entry&&entry->info.size==payload.size(),"mounted metadata");
    std::array<std::byte,123> sample{};
    require(mount->read(entry,65531,sample)==sample.size(),"mounted positioned read");
    require(std::equal(sample.begin(),sample.end(),payload.begin()+65531),"mounted data");
    require(source->active==0,"no session retained by mounted reader");
}
void gzip_checkpoints(){
    std::vector<std::byte> payload((13u<<20)+211);
    std::mt19937 rng(39021);
    for(auto& b:payload)b=static_cast<std::byte>(rng()&255);
    auto raw=make_archive(archive_write_set_format_pax_restricted,archive_write_add_filter_none,payload);
    auto cut=(7u<<20)+123;
    auto gzip=make_archive(archive_write_set_format_raw,archive_write_add_filter_gzip,std::span(raw).first(cut));
    auto second=make_archive(archive_write_set_format_raw,archive_write_add_filter_gzip,std::span(raw).subspan(cut));
    gzip.insert(gzip.end(),second.begin(),second.end());
    auto source=std::make_shared<MemorySource>(gzip);
    auto backend=open_decoder_backend(source,{},4u<<20);
    require(backend->info().format==ArchiveFormat::tar&&backend->info().compression==ARCHIVE_FILTER_GZIP,"indexed GZIP TAR");
    require(backend->info().image_size==gzip.size(),"compressed size");
    require(backend->cache_reservation_bytes()>0&&backend->cache_reservation_bytes()<=2u<<20,"bounded GZIP checkpoint cache");
    auto entry=backend->list(backend->info().root).at(0);
    std::array<std::byte,513> sample{};
    auto before=source->bytes_read.load();
    for(auto offset:{(12u<<20)+17,(4u<<20)+93,(8u<<20)+333,(7u<<20)+13}){
        backend->read_exact(entry.info.inode,offset,sample);
        require(std::equal(sample.begin(),sample.end(),payload.begin()+offset),"GZIP checkpoint read");
    }
    require(source->bytes_read-before<5u<<20,"GZIP backward reads avoid prefix replay");
    require(source->active==0,"GZIP source handles released");
    auto small=open_decoder_backend(source,{},256u<<10);
    require(small->cache_reservation_bytes()<=128u<<10,"GZIP checkpoint coarsening");
    small->read_exact(entry.info.inode,(12u<<20)+17,sample);
    require(std::equal(sample.begin(),sample.end(),payload.begin()+(12u<<20)+17),"coarsened GZIP checkpoint read");
    gzip.resize(gzip.size()-5);
    bool rejected=false;
    try{(void)open_decoder_backend(std::make_shared<MemorySource>(std::move(gzip)),{},4u<<20);}
    catch(const std::runtime_error&){rejected=true;}
    require(rejected,"truncated GZIP member rejected");
    std::cout<<"PASS bounded GZIP checkpoints, concatenated members, backwards reads and truncated members\n";
}
}
int main(){
    try{
        std::vector<std::byte> payload(1u<<20);
        std::mt19937 rng(2026);
        for(auto& byte:payload)byte=static_cast<std::byte>(rng()&255);
        exercise(archive_write_set_format_pax_restricted,archive_write_add_filter_none,ArchiveFormat::tar,payload,true);
        for(auto filter:{archive_write_add_filter_gzip,archive_write_add_filter_bzip2,archive_write_add_filter_xz,
                        archive_write_add_filter_zstd,archive_write_add_filter_lz4,archive_write_add_filter_lzma,
                        archive_write_add_filter_lzip,archive_write_add_filter_compress})
            exercise(archive_write_set_format_pax_restricted,filter,ArchiveFormat::tar,payload);
        gzip_checkpoints();
        exercise(archive_write_set_format_zip,archive_write_add_filter_none,ArchiveFormat::zip,payload);
        exercise(archive_write_set_format_7zip,archive_write_add_filter_none,ArchiveFormat::seven_zip,payload);
        exercise(archive_write_set_format_ar_bsd,archive_write_add_filter_none,ArchiveFormat::ar,payload);
        exercise(archive_write_set_format_iso9660,archive_write_add_filter_none,ArchiveFormat::iso9660,payload);
        std::cout<<"PASS real archive formats, direct TAR offsets, concurrent in-memory decoding and mounted projections\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
