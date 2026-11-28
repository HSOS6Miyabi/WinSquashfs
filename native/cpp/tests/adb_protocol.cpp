// SPDX-License-Identifier: GPL-3.0-only
#include "../device_provider.hpp"
#include <array>
#include <iostream>
using namespace archivefs;
int main(int argc,char** argv){
    if(argc!=3)return 99;
    AdbOptions options;options.port=static_cast<std::uint16_t>(std::stoi(argv[1]));options.io_timeout=std::chrono::milliseconds(1200);
    auto mode=std::string(argv[2]);options.root=mode=="root"||mode=="denied";
    auto provider=make_adb_provider(options);
    try{
        auto devices=provider->devices();
        if(mode=="empty")return devices.empty()?0:98;
        if(devices.size()!=2||devices.front().id!="serial-1"||devices.back().state!="offline")return 97;
        const std::string name="中文 '文件\n😀.bin";
        auto entries=provider->children("serial-1","/storage");
        if(entries.size()!=2||entries[0].name!=name||entries[0].size!=0x100000100ULL||!entries[1].directory)return 96;
        auto stat=provider->stat("serial-1",entries[0].id);
        if(stat.size!=entries[0].size||stat.revision!=entries[0].revision)return 95;
        std::array<std::byte,9> bytes{};provider->read_exact("serial-1",stat.id,0x100000003ULL,bytes);
        for(unsigned i=0;i<bytes.size();++i)if(bytes[i]!=std::byte(i*29))return 94;
        return 0;
    }catch(DeviceFailure const& e){
        std::cerr<<e.what()<<'\n';
        if(mode=="absent"&&e.code()==DeviceError::unavailable)return 0;
        if(mode=="denied"&&e.code()==DeviceError::permission)return 0;
        if(mode=="timeout"&&e.code()==DeviceError::timeout)return 0;
        return 1;
    }catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 2;}
}
