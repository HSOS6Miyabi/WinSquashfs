// SPDX-License-Identifier: GPL-3.0-only
#include "projection.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <fstream>
#include <iostream>
#include <random>

using namespace archivefs;
namespace fs=std::filesystem;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Temp{
    fs::path path;
    Temp(){path=fs::temp_directory_path()/("archivefs-device-"+std::to_string(std::random_device{}()));fs::create_directories(path);}
    ~Temp(){std::error_code ec;fs::remove_all(path,ec);}
};
class MockDevice final:public DeviceProvider{
public:
    std::atomic<unsigned> enumerations{0},listings{0},reads{0},stats{0};
    bool absent=false,empty=false;
    std::vector<std::byte> content;
    MockDevice(){
        content.resize(1028+8193);std::copy_n(reinterpret_cast<const std::byte*>("hsqs"),4,content.data());
        for(std::size_t i=1028;i<content.size();++i)content[i]=std::byte((i-1028)%251);
    }
    std::vector<RemoteDevice> devices()override{
        ++enumerations;
        if(absent)throw DeviceFailure(DeviceError::unavailable,"ADB Server absent");
        if(empty)return {};
        return {{"serial-a","手机","device"},{"serial-b","离线","offline"}};
    }
    std::string root_id()const override{return "/";}
    std::vector<RemoteObject> children(std::string_view device,std::string_view parent)override{
        check(device=="serial-a","device identity");++listings;
        if(parent!="/")return {};
        return {stat(device,"/a"),stat(device,"/b"),stat(device,"/c")};
    }
    RemoteObject stat(std::string_view,std::string_view object)override{
        ++stats;
        if(object=="/a")return {"/a","文件😀.bin",false,content.size(),1,"1"};
        if(object=="/b")return {"/b","镜像.squashfs",false,content.size(),1,"2"};
        if(object=="/c")return {"/c","目录",true,0,1,"3"};
        throw DeviceFailure(DeviceError::missing,"missing device file");
    }
    void read_exact(std::string_view,std::string_view,std::uint64_t offset,std::span<std::byte> output)override{
        ++reads;check(offset<=content.size()&&output.size()<=content.size()-offset,"device read bounds");
        std::copy_n(content.begin()+static_cast<std::ptrdiff_t>(offset),output.size(),output.begin());
    }
};
class FixtureBackend final:public ArchiveBackend{
    std::shared_ptr<const RandomAccessSource> source_;
    bool outer_;
public:
    explicit FixtureBackend(std::shared_ptr<const RandomAccessSource> source):source_(std::move(source)){
        std::array<std::byte,1> marker{};source_->read_exact(0,marker);outer_=marker[0]==std::byte{1};
    }
    ArchiveInfo info()const override{return {ArchiveFormat::squashfs,1,source_->size(),4096,0};}
    FileInfo stat(InodeId id)const override{
        if(id==1)return {1,FileKind::directory,0,1};
        if(id==2)return {2,FileKind::regular,source_->size()-1,1};
        throw std::out_of_range("fixture inode");
    }
    std::vector<DirectoryEntry> list(InodeId id)const override{
        check(id==1,"fixture directory");
        return {{outer_?"inner.squashfs":"data.txt",stat(2)}};
    }
    void read_exact(InodeId id,std::uint64_t offset,std::span<std::byte> output)const override{
        check(id==2,"fixture file");source_->read_exact(offset+1,output);
    }
    void validate_source()const override{source_->validate();}
    void validate_path()const override{source_->validate_path();}
    bool source_changed()const noexcept override{return source_->source_changed();}
};
}
int main(int argc,char** argv){try{
    check(argc==2,"module directory");
    auto source_open=make_archive_source_opener(argv[1],1u<<20,16u<<20);
    std::vector<std::shared_ptr<OpenProgress>> progress;
    auto pool=std::make_shared<ImagePool>(make_archive_opener(argv[1]),ImagePool::Settings{},source_open,
        [&](auto p){progress.push_back(std::move(p));});
    auto provider=std::make_shared<MockDevice>();
    auto projection=device_projection(provider,pool,u"adb");
    auto root=projection->open(u"\\");auto devices=projection->list(root);
    check(devices.size()==2&&provider->enumerations==1,"cached device list");
    auto device=projection->open(u"\\手机");
    auto entries=projection->list(device);
    check(entries.size()==3&&provider->listings==1,"device directory list");
    auto file=projection->open(u"\\手机\\文件😀.bin");
    check(file&&!file->info.directory&&file->info.size==provider->content.size(),"Unicode file");
    std::barrier sync(8);
    std::vector<std::jthread> threads;
    for(unsigned i=0;i<8;++i)threads.emplace_back([&,i]{
        sync.arrive_and_wait();
        std::array<std::byte,64> bytes{};
        auto offset=1028+i*64;
        check(projection->read(file,offset,bytes)==bytes.size(),"device range length");
        for(std::size_t k=0;k<bytes.size();++k)check(bytes[k]==std::byte((i*64+k)%251),"device range contents");
    });
    threads.clear();
    check(provider->reads==1,"concurrent device reads coalesced");
    auto count=provider->stats.load();
    auto reopened=projection->open(u"\\手机\\文件😀.bin");
    check(reopened->source==file->source,"same active file generation reused across opens");
    std::array<std::byte,1> cached{};
    check(projection->read(reopened,1028,cached)==1&&provider->reads==1,"reopened file reuses data cache");
    check(provider->stats==count,"reopened file reuses metadata validation");
    auto decoded=projection->open(u"\\手机\\镜像.squashfs\\data.bin");
    std::array<std::byte,128> bytes{};
    check(decoded&&!decoded->info.directory&&projection->read(decoded,17,bytes)==bytes.size(),"device archive read");
    for(std::size_t i=0;i<bytes.size();++i)check(bytes[i]==std::byte((i+17)%251),"archive contents");
    check(progress.size()==1&&progress.front()->done&&progress.front()->size==provider->content.size()&&progress.front()->offset>0,"archive opening progress lifecycle");
    check(projection->open(u"\\手机\\镜像.squashfs")->info.directory,"archive directory projection");
    check(progress.size()==1,"cached archive does not create notification");
    // A disappearance observed by enumeration retires all sources belonging
    // to that device, even when size and revision are unchanged on reconnection.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    provider->empty=true;
    check(projection->list(projection->open(u"\\")).empty(),"device removal observed");
    bool invalid=false;try{projection->read(file,1028,cached);}catch(const DeviceFailure&){invalid=true;}
    check(invalid,"old file generation invalidated on disconnect");
    provider->content[1028]=std::byte{177};
    provider->empty=false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto connected=projection->open(u"\\手机\\文件😀.bin");
    check(connected->source!=file->source,"reconnection starts new file generation");
    check(projection->read(connected,1028,cached)==1&&cached[0]==std::byte{177},"reconnection does not reuse retired data");
    auto new_image=projection->open(u"\\手机\\镜像.squashfs\\data.bin");
    check(new_image&&projection->read(new_image,0,cached)==1&&cached[0]==std::byte{177},"reconnection opens fresh archive");
    bool denied=false;try{projection->open(u"\\离线");}catch(const DeviceFailure& e){denied=e.code()==DeviceError::offline;}
    check(denied,"offline status");
    auto empty=std::make_shared<MockDevice>();empty->empty=true;
    auto e=device_projection(empty,pool,u"adb");check(e->list(e->open(u"\\")).empty(),"empty server");
    auto absent=std::make_shared<MockDevice>();absent->absent=true;
    unsigned errors=0;auto missing=device_projection(absent,pool,u"adb",[&](const DeviceFailure& x){check(x.code()==DeviceError::unavailable,"desktop error type");++errors;});
    bool unavailable=false;try{missing->open(u"\\");}catch(const DeviceFailure& e){unavailable=e.code()==DeviceError::unavailable;}
    check(unavailable&&errors==1,"absent server cannot be opened");
    Temp temp;
    auto path=temp.path/"outer.squashfs";
    {std::ofstream out(path,std::ios::binary);out.put(1);out.put(1);out.put(0);out<<"hello";}
    ArchiveSourceOpener nested=[](auto source){return std::make_shared<ArchiveTree>(std::make_shared<ArchiveReader>(std::make_shared<FixtureBackend>(std::move(source)),0));};
    auto nested_pool=std::make_shared<ImagePool>([nested](const fs::path& p,SourcePolicy policy){return nested(open_file_source(p,policy));},ImagePool::Settings{},nested);
    auto unc=unc_projection({u"path",temp.path},nested_pool);
    auto leaf=unc->open(u"\\outer.squashfs\\inner.squashfs\\inner.squashfs\\data.txt");
    std::array<std::byte,5> hello{};check(leaf&&unc->read(leaf,0,hello)==hello.size(),"recursive archive source");
    check(std::equal(hello.begin(),hello.end(),reinterpret_cast<const std::byte*>("hello")),"nested data");
    fs::remove(path);
    bool changed=false;try{unc->read(leaf,0,hello);}catch(...){changed=true;}check(changed,"nested source deletion");
    std::cout<<"PASS device cache, Unicode names, server availability, archive progress and recursive archive reads\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
