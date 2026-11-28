// SPDX-License-Identifier: GPL-3.0-only
#include "archive_tree.hpp"
#include "archive_reader.hpp"
#include <cstdlib>
#include <array>
#include <cstring>
#include <string_view>

using namespace archivefs;
namespace {
class Input final: public RandomAccessSource {
    struct Session final:ReadSession{
        std::span<const std::byte> data;
        explicit Session(std::span<const std::byte> bytes):data(bytes){}
        void read_exact(std::uint64_t offset,std::span<std::byte> out)const override{
            if(offset>data.size()||out.size()>data.size()-offset)std::abort();
            if(!out.empty())std::memcpy(out.data(),data.data()+offset,out.size());
        }
        void finish()override{}
    };
    std::span<const std::byte> data_;
public:
    explicit Input(std::span<const std::byte> bytes):data_(bytes){}
    std::uint64_t size()const noexcept override{return data_.size();}
    std::unique_ptr<ReadSession> begin_read()const override{return std::make_unique<Session>(data_);}
    void validate()const override{}
};
class MetadataInput final:public ArchiveBackend {
public:
    explicit MetadataInput(std::span<const std::uint8_t> data){
        for(std::size_t i=0,entries=0;i+3<=data.size()&&entries<32;++entries){
            auto parent=static_cast<InodeId>(1+2*(data[i++]%4));
            auto child=static_cast<InodeId>(1+(data[i++]%8));
            auto requested=data[i++]%33;
            auto count=std::min<std::size_t>(requested,data.size()-i);
            auto name=std::string(reinterpret_cast<const char*>(data.data()+i),count);
            i+=count;
            directories_[(parent-1)/2].push_back({std::move(name),stat(child)});
        }
    }
    ArchiveInfo info()const override{return {ArchiveFormat::squashfs,1,4096,4096};}
    FileInfo stat(InodeId id)const override{
        if(id<1||id>8)throw std::out_of_range("inode");
        return {id,id%2?FileKind::directory:FileKind::regular,id%2?0u:128u,0};
    }
    std::vector<DirectoryEntry> list(InodeId id)const override{
        if(stat(id).kind!=FileKind::directory)throw std::invalid_argument("not a directory");
        return directories_[(id-1)/2];
    }
    void read_exact(InodeId id,std::uint64_t offset,std::span<std::byte> out)const override{
        auto f=stat(id);
        if(f.kind!=FileKind::regular||offset>f.size||out.size()>f.size-offset)std::abort();
        for(auto& value:out)value=std::byte(static_cast<unsigned char>((id+offset++)&255));
    }
    void validate_source()const override{}
private:
    std::array<std::vector<DirectoryEntry>,4> directories_;
};
void fuzz_metadata(std::span<const std::uint8_t> data){
    ArchiveTree tree(std::make_shared<ArchiveReader>(std::make_shared<MetadataInput>(data),4096,4096));
    auto root=tree.root();
    for(auto& child:tree.children(root)){
        if(tree.find(tree.path_of(child))!=child)std::abort();
        if(child->info.kind==FileKind::regular){
            std::array<std::byte,32> out{};
            auto offset=data.empty()?0u:data[0];
            auto length=tree.read(child,offset,out);
            if(length!=std::min<std::uint64_t>(32,offset>=128?0:128-offset))std::abort();
            for(std::size_t i=0;i<length;++i)if(out[i]!=std::byte(static_cast<unsigned char>((child->info.inode+offset+i)&255)))std::abort();
        }
    }
    try{
        auto result=tree.search(root,u"~",8);
        if(result.nodes.size()>8)std::abort();
        for(auto& child:result.nodes)if(tree.find(tree.path_of(child))!=child)std::abort();
    }catch(const std::runtime_error&){}
    if(data.size()%8==0){try{tree.verify();}catch(const std::runtime_error&){}}
}
}
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,std::size_t size){
    auto bytes=std::string_view(reinterpret_cast<const char*>(data),size);
    try{
        auto u=to_utf16(bytes);
        if(to_utf8(u)!=bytes)std::abort();
    }catch(const std::invalid_argument&){}
    auto name=encode_windows_name(bytes);
    if(name.empty()||name.size()>255||name==u"."||name==u".."||name.back()==u' '||name.back()==u'.')std::abort();
    for(auto ch:name)if(ch<32||std::u16string_view(u"<>:\"/\\|?*").find(ch)!=std::u16string_view::npos)std::abort();
    auto round=to_utf16(to_utf8(name));
    if(round!=name)std::abort();
    Input input(std::as_bytes(std::span(data,size)));
    (void)detect_format(input);
    fuzz_metadata(std::span(data,size));
    return 0;
}
