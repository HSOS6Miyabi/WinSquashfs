// SPDX-License-Identifier: GPL-3.0-only
#include "../device_provider.hpp"
#include <algorithm>
#include <array>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace archivefs;
namespace {
void check(bool ok, const char* message) { if(!ok)throw std::runtime_error(message); }
constexpr std::uint64_t sparse_offset=0x100000003ULL;
constexpr std::string_view filename="中文 '文件\n😀.bin";
constexpr std::size_t payload_size=262144+37;
std::byte expected(std::uint64_t index) {
    return std::byte((index*29+(index>>8))&255);
}
}
int main(int argc,char** argv) {
    if(argc!=4&&argc!=5) {
        std::cerr<<"usage: adb_live_tests SERIAL DIRECTORY plain|root [PORT]\n";
        return 2;
    }
    try {
        AdbOptions options;
        std::string mode=argv[3];
        check(mode=="plain"||mode=="root","Invalid access mode");
        options.root=mode=="root";
        if(argc==5) {
            auto port=std::stoi(argv[4]);
            check(port>0&&port<=65535,"Invalid port");
            options.port=static_cast<std::uint16_t>(port);
        }
        auto provider=make_adb_provider(options);
        std::string serial=argv[1], parent=argv[2];
        auto devices=provider->devices();
        check(std::any_of(devices.begin(),devices.end(),[&](const auto& d){return d.id==serial&&d.state=="device";}),"Requested device is not online");
        auto entries=provider->children(serial,parent);
        auto lookup=[&](std::string_view name)->RemoteObject {
            auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& o){return o.name==name;});
            check(found!=entries.end(),"Expected Unicode or sparse fixture is missing");
            auto stat=provider->stat(serial,found->id);
            check(stat.directory==found->directory&&stat.size==found->size&&stat.revision==found->revision,"Directory and stat metadata differ");
            return stat;
        };
        auto file=lookup(filename);
        check(!file.directory&&file.size==payload_size,"Unexpected data file size");
        check(lookup("空目录").directory,"Directory metadata incorrect");
        auto verify=[&](std::uint64_t offset,std::size_t count) {
            std::vector<std::byte> data(count);
            provider->read_exact(serial,file.id,offset,data);
            for(std::size_t i=0;i<count;++i)check(data[i]==expected(offset+i),"Data mismatch");
        };
        verify(0,1);verify(63,4097);verify(65533,65539);verify(payload_size-32,32);
        std::vector<std::future<void>> reads;
        for(unsigned i=0;i<8;++i)
            reads.push_back(std::async(std::launch::async,verify,i*7919+17,8192));
        for(auto& read:reads)read.get();
        auto sparse=lookup("sparse.bin");
        check(sparse.size==sparse_offset+9,"64-bit sparse file metadata incorrect");
        std::array<std::byte,12> data{};
        provider->read_exact(serial,sparse.id,sparse_offset-3,data);
        constexpr std::string_view tail="ADB-large";
        for(unsigned i=0;i<3;++i)check(data[i]==std::byte{0},"Sparse prefix incorrect");
        for(std::size_t i=0;i<tail.size();++i)check(data[i+3]==std::byte(tail[i]),"64-bit offset data incorrect");
        if(options.root) {
            auto private_dir=lookup("private");
            check(private_dir.directory,"Root fixture directory missing");
            auto private_entries=provider->children(serial,private_dir.id);
            auto marker=std::find_if(private_entries.begin(),private_entries.end(),[](const auto& o){return o.name=="marker.bin";});
            check(marker!=private_entries.end()&&!marker->directory,"Root-only fixture missing");
            constexpr std::string_view message="ArchiveFS-root-only";
            check(marker->size==message.size(),"Root-only fixture size incorrect");
            auto marker_stat=provider->stat(serial,marker->id);
            check(marker_stat.size==message.size(),"Root-only stat failed");
            std::array<std::byte,message.size()> bytes{};
            provider->read_exact(serial,marker->id,0,bytes);
            for(std::size_t i=0;i<message.size();++i)check(bytes[i]==std::byte(message[i]),"Root-only data mismatch");
            std::cout<<"PASS root-only directory enumeration, stat and file read\n";
        }
        std::cout<<"PASS actual adbd directory, stat, Unicode, concurrent reads and 64-bit file offset ("<<mode<<")\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
