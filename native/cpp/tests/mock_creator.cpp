// SPDX-License-Identifier: GPL-3.0-only
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <clocale>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace fs=std::filesystem;
int run(const std::vector<fs::path>& args){
    if(args.size()==2&&args[1]=="fail")return 17;
    if(args.size()==2&&args[1]=="echo"){
#ifdef _WIN32
        _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
#endif
        char buffer[4096];std::uint64_t n=0;while(std::cin){std::cin.read(buffer,sizeof(buffer));n+=static_cast<std::uint64_t>(std::cin.gcount());}std::cout<<"input="<<n<<'\n';std::cerr<<"done\n";return 0;
    }
    std::setlocale(LC_ALL,"C.UTF-8");
#ifdef _WIN32
    const auto tool=args[0].stem();
#else
    const auto tool=args[0].filename();
#endif
fs::path output;std::vector<char> content(8193,'a');std::array<char,1028> header{};
    if(tool=="mksquashfs"){
        auto it=std::find(args.begin(),args.end(),fs::path("-noappend"));if(it==args.end()||it-args.begin()<3)throw std::runtime_error("missing SquashFS output");output=*(it-1);std::copy_n("hsqs",4,header.begin());
    }else if(tool=="mkdwarfs"){
        auto it=std::find(args.begin(),args.end(),fs::path("-o"));if(it==args.end()||it+1==args.end())throw std::runtime_error("missing DwarFS output");output=*(it+1);std::copy_n("DWARFS",6,header.begin());
    }else if(tool=="mkfs.erofs"||tool=="archivefs-tar2sqfs"){
#ifdef _WIN32
        _setmode(_fileno(stdin),_O_BINARY);
#endif
        output=args.back();
        if(tool=="archivefs-tar2sqfs")std::copy_n("hsqs",4,header.begin());
        else{header[1024]=static_cast<char>(0xe2);header[1025]=static_cast<char>(0xe1);header[1026]=static_cast<char>(0xf5);header[1027]=static_cast<char>(0xe0);}
        auto a=archive_read_new();if(!a)throw std::bad_alloc();struct Guard{struct archive* a;~Guard(){archive_read_free(a);}}guard{a};
        archive_read_support_format_tar(a);archive_read_support_filter_none(a);
        if(archive_read_open_fd(a,0,65536)!=ARCHIVE_OK)throw std::runtime_error("cannot open TAR stream");
        struct archive_entry* e=nullptr;bool seen=false;for(;;){int result=archive_read_next_header(a,&e);if(result==ARCHIVE_EOF)break;if(result!=ARCHIVE_OK)throw std::runtime_error(archive_error_string(a));std::string name=archive_entry_pathname_utf8(e);if(name=="data.bin"){if(archive_entry_size(e)!=8193)throw std::runtime_error("wrong file size");std::size_t pos=0;while(pos<content.size()){auto n=archive_read_data(a,content.data()+pos,content.size()-pos);if(n<=0)throw std::runtime_error("short TAR stream");pos+=static_cast<std::size_t>(n);}seen=true;}else archive_read_data_skip(a);}
        if(!seen)throw std::runtime_error("missing data.bin in TAR");
    }else throw std::runtime_error("unexpected tool name");
    std::ofstream f(output,std::ios::binary);f.write(header.data(),header.size());f.write(content.data(),static_cast<std::streamsize>(content.size()));f.close();if(!f)throw std::runtime_error("output failed");std::cout<<"created\n";return 0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<fs::path> args;for(int i=0;i<argc;++i)args.emplace_back(argv[i]);return run(args);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
#else
int main(int argc,char** argv){try{std::vector<fs::path> args;for(int i=0;i<argc;++i)args.emplace_back(argv[i]);return run(args);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
#endif
