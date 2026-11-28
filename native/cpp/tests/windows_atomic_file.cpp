// SPDX-License-Identifier: GPL-3.0-only
#include "../windows_atomic_file.hpp"
#include <future>
#include <iostream>
#include <random>
#include <vector>
using namespace archivefs::detail;
namespace {
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
struct Temp{
    fs::path directory;
    Temp(){
        for(unsigned i=0;i<64;++i){
            directory=fs::temp_directory_path()/("archivefs-settings-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(std::random_device{}()));
            if(fs::create_directory(directory))return;
        }
        throw std::runtime_error("create temporary directory");
    }
    ~Temp(){std::error_code ec;fs::remove_all(directory,ec);}
};
}
int main(){try{
    Temp temp;
    auto path=temp.directory/L"settings.json";
    const std::string old(128u<<10,'a'),next(128u<<10,'b');
    write_text_atomic(path,old,"write initial settings");
    // A third-party reader denies delete sharing temporarily.
    FileHandle held(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
    check(held.value!=INVALID_HANDLE_VALUE,"open external reader");
    auto writer=std::async(std::launch::async,[&]{write_text_atomic(path,next,"replace locked settings");});
    // Depending on filesystem support, POSIX rename may publish immediately
    // or wait until the reader releases the destination.
    (void)writer.wait_for(150ms);
    held.close();
    writer.get();
    check(read_text(path,1u<<20)==next,"replacement after sharing conflict");
    // An existing delete-sharing handle remains a complete old snapshot.
    FileHandle snapshot(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
    check(snapshot.value!=INVALID_HANDLE_VALUE,"open snapshot");
    write_text_atomic(path,old,"replace settings with snapshot open");
    char ch=0;DWORD got=0;
    check(ReadFile(snapshot.value,&ch,1,&got,nullptr)&&got==1&&ch=='b',"old snapshot intact");
    snapshot.close();
    std::atomic<bool> done=false;
    auto reader=std::async(std::launch::async,[&]{
        while(!done.load()){auto s=read_text(path,1u<<20);check(s==old||s==next,"reader saw incomplete settings");}
    });
    try{
        std::vector<std::future<void>> writers;
        for(unsigned i=0;i<4;++i)writers.push_back(std::async(std::launch::async,[&,i]{
            for(unsigned j=0;j<12;++j)write_text_atomic(path,(i+j)%2?old:next,"concurrent settings replace");
        }));
        for(auto& w:writers)w.get();
    }catch(...){done=true;reader.get();throw;}
    done=true;reader.get();
    const auto before=read_text(path,1u<<20);
    check(SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_READONLY)!=0,"set destination read-only");
    bool rejected=false;
    try{write_text_atomic(path,"rejected","bounded settings replace",100ms);}
    catch(const std::system_error&){rejected=true;}
    check(SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL)!=0,"restore destination attributes");
    check(rejected,"permanent replacement failure reported");
    check(read_text(path,1u<<20)==before,"failed replacement preserves settings");
    check(std::distance(fs::directory_iterator(temp.directory),fs::directory_iterator{})==1,"temporary files cleaned");
    std::cout<<"PASS atomic settings snapshots, transient sharing conflicts, concurrent writers and bounded failure\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
