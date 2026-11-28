// SPDX-License-Identifier: GPL-3.0-only
#include "archive_tree.hpp"
#include "decoder_backend.hpp"
#include "image_creation.hpp"
#include <charconv>
#include <clocale>
#ifdef ARCHIVEFS_WITH_WINFSP
#include "windows_services.hpp"
#endif
#include <iostream>
#include <memory>
#include <string_view>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace archivefs;
namespace {
#ifndef ARCHIVEFS_WITH_WINFSP
std::filesystem::path application_directory() {
#ifdef _WIN32
    std::wstring path(512,L'\0');
    for (;;) {
        auto count=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        if(!count) throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"GetModuleFileNameW");
        if(count<path.size()) {path.resize(count);return std::filesystem::path(path).parent_path();}
        if(path.size()>=32768) throw std::length_error("application path too long");
        path.resize(path.size()*2);
    }
#else
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}
#endif
#ifdef ARCHIVEFS_WITH_WINFSP
std::stop_source* console_stop=nullptr;
BOOL WINAPI cancel_console(DWORD event){if(event==CTRL_C_EVENT||event==CTRL_BREAK_EVENT){if(console_stop)console_stop->request_stop();return TRUE;}return FALSE;}
struct ConsoleCancellation {
    std::stop_source stop;
    ConsoleCancellation(){console_stop=&stop;if(!SetConsoleCtrlHandler(cancel_console,TRUE)){console_stop=nullptr;throw std::runtime_error("cannot install console handler");}}
    ~ConsoleCancellation(){SetConsoleCtrlHandler(cancel_console,FALSE);console_stop=nullptr;}
};
#endif
std::uint64_t number(std::u16string_view text) {
    auto str=to_utf8(text);
    std::uint64_t n=0;
    auto [ptr,ec]=std::from_chars(str.data(),str.data()+str.size(),n);
    if(ec!=std::errc{}||ptr!=str.data()+str.size()) throw std::invalid_argument("invalid unsigned number");
    return n;
}
std::string_view format_name(ArchiveFormat format) {
    switch(format) {
    case ArchiveFormat::squashfs:return "SquashFS";
    case ArchiveFormat::dwarfs:return "DwarFS";
    case ArchiveFormat::erofs:return "EROFS";
    default:return "Unknown";
    }
}
std::string_view codec_name(const ArchiveInfo& info) {
    if(info.format!=ArchiveFormat::squashfs) return "Image-defined compression";
    switch(info.compression) {
    case 1:return "gzip";case 2:return "lzma";case 3:return "lzo";case 4:return "xz";
    case 5:return "lz4";case 6:return "zstd";default:return "Unknown";
    }
}
struct UsageError:std::runtime_error {using std::runtime_error::runtime_error;};
struct Language {
    bool chinese=false;
    std::string_view tr(std::string_view en,std::string_view zh) const {return chinese?zh:en;}
    std::string_view kind(FileKind type) const {
        switch(type) {
        case FileKind::directory:return tr("Folder","文件夹");
        case FileKind::regular:return tr("File","文件");
        case FileKind::symlink:return tr("Symbolic link","符号链接");
        default:return tr("Special file","特殊文件");
        }
    }
};
std::shared_ptr<ArchiveReader> open_archive(const std::filesystem::path& source,const std::filesystem::path& modules) {
    std::shared_ptr<const RandomAccessSource> input=open_file_source(source);
    auto backend=open_decoder_backend(std::move(input),modules);
    auto info=backend->info();
    // A format-specific block size avoids splitting normal SquashFS blocks.
    auto page_size=info.block_size?std::clamp<std::size_t>(info.block_size,64u<<10,1u<<20):256u<<10;
    return std::make_shared<ArchiveReader>(std::move(backend),64u<<20,page_size);
}
int cli(std::vector<std::u16string> args) {
    Language language;
    auto modules=application_directory();
    auto tools=modules / "tools";
    for(;;) {
        if(args.size()>=2&&args[0]==u"--lang") {
            if(args[1]==u"zh-CN") language.chinese=true;
            else if(args[1]==u"en-US") language.chinese=false;
            else if(args[1]==u"system") {
#ifdef _WIN32
                language.chinese=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_CHINESE;
#endif
            } else throw std::invalid_argument("invalid language setting");
            args.erase(args.begin(),args.begin()+2);
        } else if(args.size()>=2&&args[0]==u"--decoder-directory") {
            modules=std::filesystem::path(args[1]);args.erase(args.begin(),args.begin()+2);
        } else if(args.size()>=2&&args[0]==u"--tools-directory") {
            tools=std::filesystem::path(args[1]);args.erase(args.begin(),args.begin()+2);
        } else break;
    }
    if(args.size()==1&&(args[0]==u"--help"||args[0]==u"help")) {
        std::cout<<language.tr("Usage: archivefs [--lang en-US|zh-CN|system] info|list|read|verify|unpack|pack|create|mount|eject|unc-start|unc-stop|unc-path|unc-open|unc-status|serve|doctor ...\n",
            "用法：archivefs [--lang en-US|zh-CN|system] info|list|read|verify|unpack|pack|create|mount|eject|unc-start|unc-stop|unc-path|unc-open|unc-status|serve|doctor ...\n");return 0;
    }
    if(args.size()==1&&(args[0]==u"--version"||args[0]==u"version")) {std::cout<<ARCHIVEFS_VERSION<<'\n';return 0;}
    if(args.empty()) throw UsageError("invalid command arguments");
    const auto command=args[0];
#ifdef ARCHIVEFS_WITH_WINFSP
    if(command==u"doctor"&&args.size()==1){std::cout<<winfsp_runtime_info()<<'\n';return 0;}
    if(command==u"unc-start"&&args.size()==1){ConsoleCancellation c;start_agent(c.stop.get_token());for(auto& s:Settings::load().shares())std::cout<<to_utf8(u"\\\\"+s.server+u"\\"+s.name)<<'\n';return 0;}
    if(command==u"unc-stop"&&args.size()==1){stop_agent();return 0;}
    if(command==u"unc-status"&&args.size()==1){std::cout<<(agent_running()?language.tr("Running","运行中"):language.tr("Stopped","已停止"))<<'\n';return 0;}
    if(command==u"unc-path"&&args.size()==2){std::cout<<to_utf8(Settings::load().unc_path(args[1]))<<'\n';return 0;}
    if(command==u"unc-open"&&args.size()==2){ConsoleCancellation c;open_unc(args[1],c.stop.get_token());return 0;}
    if(command==u"eject"&&args.size()==2){eject_volume(args[1]);return 0;}
    if(command==u"mount"&&(args.size()==2||args.size()==3)){ConsoleCancellation c;MountedVolume mount(args[1],modules,args.size()==3?args[2]:u"");std::cout<<to_utf8(mount.drive())<<std::endl;mount.wait(c.stop.get_token());return 0;}
    if(command==u"serve"&&(args.size()==3||args.size()==4)){auto idle=args.size()==4?number(args[3]):60;if(idle>86400)throw std::invalid_argument("invalid idle timeout");ConsoleCancellation c;serve_share({args[1],args[2]},modules,static_cast<unsigned>(idle),c.stop.get_token());return 0;}
#endif
    if(args.size()<2)throw UsageError("invalid command arguments");
    if(command==u"create"||command==u"pack") {
        CreationOptions options;
        std::vector<std::filesystem::path> inputs;
        std::filesystem::path output;
        if(command==u"create") {
            if(args.size()<5)throw UsageError("create FORMAT PRESET OUTPUT SOURCE...");
            auto format=args[1]==u"SquashFS"||args[1]==u"squashfs"?ArchiveFormat::squashfs:
                args[1]==u"DwarFS"||args[1]==u"dwarfs"?ArchiveFormat::dwarfs:
                args[1]==u"EROFS"||args[1]==u"erofs"?ArchiveFormat::erofs:ArchiveFormat::unknown;
            auto preset=number(args[2]);if(preset<1||preset>8)throw std::invalid_argument("invalid compression preset");
            options=CreationOptions::for_preset(format,static_cast<CompressionPreset>(preset));
            output=args[3];for(std::size_t i=4;i<args.size();++i)inputs.emplace_back(args[i]);
        }else{
            if(args.size()!=3&&args.size()!=4)throw UsageError("pack SOURCE OUTPUT [CODEC]");
            inputs.emplace_back(args[1]);output=args[2];if(args.size()==4)options.codec=to_utf8(args[3]);
        }
        ImageCreator(tools,modules).create(inputs,output,options,[](std::string_view line){std::cout<<line<<'\n';});
        return 0;
    }
    if(!((command==u"info"||command==u"verify")&&args.size()==2) &&
       !(command==u"list"&&(args.size()==2||args.size()==3)) &&
       !(command==u"read"&&args.size()==5) && !(command==u"unpack"&&args.size()==3))
        throw UsageError("invalid command arguments");
    ArchiveTree tree(open_archive(std::filesystem::path(args[1]),modules));
    if(command==u"info") {
        auto info=tree.info();std::cout<<format_name(info.format)<<' '<<codec_name(info)<<' '<<info.block_size<<' '<<info.image_size<<'\n';
    } else if(command==u"list") {
        auto directory=args.size()==2?tree.root():tree.find(args[2]);
        if(!directory||directory->info.kind!=FileKind::directory) throw std::runtime_error("directory not found");
        for(const auto& node:tree.children(directory))
            std::cout<<language.kind(node->info.kind)<<'\t'<<node->info.size<<'\t'<<node->name<<'\n';
    } else if(command==u"read") {
        auto node=tree.find(args[2]);
        if(!node) throw std::runtime_error("entry not found");
        auto offset=number(args[3]),count=number(args[4]);
        if(count>(16u<<20)) throw std::out_of_range("count must be between 0 and 16 MiB");
        std::vector<std::byte> data(static_cast<std::size_t>(count));
        auto n=tree.read(node,offset,data);
        constexpr char hex[]="0123456789ABCDEF";
        std::string result(n*2,'\0');
        for(std::size_t i=0;i<n;++i) {
            auto b=std::to_integer<unsigned char>(data[i]);result[2*i]=hex[b>>4];result[2*i+1]=hex[b&15];
        }
        std::cout<<result<<'\n';
    } else if(command==u"verify") {
        auto report=tree.verify();
        if(language.chinese) std::cout<<"已校验 "<<report.files<<" 个文件，共 "<<report.bytes<<" 字节。\n";
        else std::cout<<"Verified "<<report.files<<" files, "<<report.bytes<<" bytes.\n";
    } else if(command==u"unpack") {
        tree.extract(tree.root(),std::filesystem::path(args[2]));
        std::cout<<language.tr("Extracted ","已提取至 ")<<to_utf8(args[2])<<'\n';
    }
    return 0;
}
} // namespace
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setlocale(LC_CTYPE,".UTF-8");
    try {
        std::vector<std::u16string> args;
        static_assert(sizeof(wchar_t)==sizeof(char16_t));
        for(int i=1;i<argc;++i) args.emplace_back(reinterpret_cast<const char16_t*>(argv[i]));
        return cli(std::move(args));
    } catch(const UsageError& error) {std::cerr<<error.what()<<'\n';return 2;} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
#else
int main(int argc,char** argv) {
    std::setlocale(LC_CTYPE,"C.UTF-8");
    try {
        std::vector<std::u16string> args;
        for(int i=1;i<argc;++i) args.push_back(to_utf16(argv[i]));
        return cli(std::move(args));
    } catch(const UsageError& error) {std::cerr<<error.what()<<'\n';return 2;} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
#endif
