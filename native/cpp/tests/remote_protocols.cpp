// SPDX-License-Identifier: GPL-3.0-only
#include "remote_projection.hpp"
#include "remote_control.hpp"
#include "remote_recovery.hpp"
#include <array>
#include <cstring>
#include <iostream>
#ifdef ARCHIVEFS_WITH_WINFSP
#include "winfsp_native.hpp"
#include <windows.h>
#endif
using namespace archivefs;
namespace {
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
#ifdef ARCHIVEFS_WITH_WINFSP
void mounted_test(const std::shared_ptr<Projection>& p,const std::shared_ptr<Projection>& n){
    NativeMount dav(p,u"",u"\\$sfs\\dav");
    NativeMount root(n,u"",u"\\$sfs\\root");
    namespace fs=std::filesystem;
    fs::path dir(L"\\\\$sfs\\dav\\test");
    check(fs::is_directory(dir),"mounted connection directory");
    check(fs::is_directory(L"\\\\$sfs\\root\\dav\\test"),"mounted namespace directory");
    check(fs::exists(dir/L"a.txt"),"mounted file exists");
    struct Handle{
        HANDLE value;
        ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
        void close(){check(CloseHandle(value)!=0,"close remote file");value=INVALID_HANDLE_VALUE;}
    };
    auto open=[](const fs::path& path,DWORD disposition){
        HANDLE h=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,disposition,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Open mounted remote file");
        return Handle{h};
    };
    auto seek=[](HANDLE h,LONGLONG position){LARGE_INTEGER pos{};pos.QuadPart=position;check(SetFilePointerEx(h,pos,nullptr,FILE_BEGIN)!=0,"seek remote file");};
    auto file=open(dir/L"a.txt",OPEN_EXISTING);DWORD done=0;char data[4]{};
    seek(file.value,2);check(ReadFile(file.value,data,4,&done,nullptr)&&done==4&&!std::memcmp(data,"2345",4),"mounted positioned read");
    seek(file.value,3);check(WriteFile(file.value,"AB",2,&done,nullptr)&&done==2,"mounted positioned write");check(FlushFileBuffers(file.value)!=0,"mounted flush");
    seek(file.value,2);check(ReadFile(file.value,data,4,&done,nullptr)&&done==4&&!std::memcmp(data,"2AB5",4),"mounted read after write");
    seek(file.value,6);check(SetEndOfFile(file.value)!=0,"mounted truncate");check(FlushFileBuffers(file.value)!=0,"mounted truncate flush");file.close();
    check(fs::file_size(dir/L"a.txt")==6,"mounted truncated size");
    auto created=open(dir/L"b.txt",CREATE_NEW);check(WriteFile(created.value,"AB",2,&done,nullptr)&&done==2,"mounted create and write");check(FlushFileBuffers(created.value)!=0,"mounted new file flush");created.close();
    fs::rename(dir/L"b.txt",dir/L"c.txt");check(fs::exists(dir/L"c.txt"),"mounted rename");
    check(fs::remove(dir/L"c.txt"),"mounted deletion");
    auto source=open(dir/L"a.txt",OPEN_EXISTING);
    auto request=encode_remote_control(RemoteCommand::copy,u"\\test\\server-copy.txt");
    check(DeviceIoControl(source.value,remote_control_code,request.data(),static_cast<DWORD>(request.size()),nullptr,0,&done,nullptr)!=0,"mounted server-side copy");
    source.close();
    check(fs::file_size(dir/L"server-copy.txt")==6,"server copy size");
    check(fs::remove(dir/L"server-copy.txt"),"delete copied file");
    std::cout<<"PASS WinFsp mounted remote WebDAV read/write/flush/rename and namespace\n";
}
#endif
}
int main(int argc,char** argv){
    try{
        if(argc!=4)return 2;
        RemoteConnection c;c.name="test";c.endpoint=argv[1];c.root="/dav";c.cache_mib=8;c.read_cache_mib=0;c.metadata_seconds=0;
        auto mode=std::string_view(argv[3]);
        if(mode=="writeback"||mode=="recover"||mode=="conflict"||mode=="lost-response")c.cache_mode="writeback";
        auto p=remote_projection(RemoteProtocol::dav,{c},argv[2]);
        auto n=namespace_projection();n->replace_routes({{u"dav",p}});
#ifdef ARCHIVEFS_WITH_WINFSP
        if(std::string_view(argv[3])=="winfsp"){mounted_test(p,n);return 0;}
#endif
        if(mode=="recover"||mode=="conflict"||mode=="lost-response"){
            auto file=p->open(u"\\test\\a.txt");check(static_cast<bool>(file),"recover source");
            std::string x="AB";p->write(file,3,std::as_bytes(std::span(x)),false,false);
            if(mode=="lost-response"){
                bool failed=false;try{p->flush(file);}catch(const RemoteFailure&){failed=true;}
                check(failed,"upload response loss");
            }
            file.reset();n.reset();p.reset();
            p=remote_projection(RemoteProtocol::dav,{c},argv[2]);file=p->open(u"\\test\\a.txt");check(static_cast<bool>(file),"restored pending file");
            std::array<std::byte,4> b{};check(p->read(file,2,b)==4&&!std::memcmp(b.data(),"2AB5",4),"restored local data");
            bool blocked=false;try{p->flush(file);}catch(const RemoteFailure& e){blocked=e.code()==RemoteError::conflict;}
            check(blocked,"recovered file does not upload implicitly");
            if(mode=="conflict"){
                auto other=make_remote_transport(c);std::string changed="XXXXXXXXXX";
                other->replace("/a.txt",changed.size(),[&](std::uint64_t pos,std::span<std::byte> out){auto count=std::min(out.size(),changed.size()-static_cast<std::size_t>(pos));std::memcpy(out.data(),changed.data()+static_cast<std::size_t>(pos),count);return count;});
                bool conflict=false;try{p->recover(file,false);}catch(const RemoteFailure& e){conflict=e.code()==RemoteError::conflict;}
                check(conflict,"recovery preserves changed remote version");
                p->recover(file,true);
                check(p->read(file,0,b)==4&&!std::memcmp(b.data(),"XXXX",4),"discard pending upload");
            }else p->recover(file,false);
            std::cout<<"PASS persistent recovery, explicit retry and conflict handling\n";return 0;
        }
        auto top=n->open(u"\\");check(top&&n->list(top).size()==1,"namespace root");
        auto root=n->open(u"\\dav\\test");check(root&&n->list(root).size()==1,"connection directory");
        auto file=n->open(u"\\dav\\test\\a.txt");check(file&&n->query_info(file).size==10,"stat");
        std::array<std::byte,4> b{};check(n->read(file,2,b)==4&&!std::memcmp(b.data(),"2345",4),"range");
        std::string x="AB";check(n->write(file,3,std::as_bytes(std::span(x)),false,false)==2,"write");
        n->flush(file);check(n->read(file,2,b)==4&&!std::memcmp(b.data(),"2AB5",4),"updated");
        n->resize(file,6);n->flush(file);check(n->query_info(file).size==6,"truncate");
        auto created=n->create_file(u"\\dav\\test\\b.txt",false);n->write(created,0,std::as_bytes(std::span(x)),false,false);n->flush(created);
        n->rename(created,u"\\dav\\test\\c.txt",false);check(n->open(u"\\dav\\test\\c.txt")!=nullptr,"rename");
        n->remove(created);check(n->open(u"\\dav\\test\\c.txt")==nullptr,"delete");
        std::cout<<"PASS remote WebDAV range, writes, cached truncation, create, rename, delete and namespace\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
