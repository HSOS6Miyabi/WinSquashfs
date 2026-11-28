// SPDX-License-Identifier: GPL-3.0-only
#include "image_creation.hpp"
#include "archive_tree.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <chrono>
#include <clocale>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <thread>
using namespace archivefs;
namespace fs=std::filesystem;
void require(bool c,const char* message){if(!c)throw std::runtime_error(message);}
template<class F>void throws(F&& f){bool result=false;try{f();}catch(...){result=true;}require(result,"expected error");}
struct Temp {fs::path p;Temp(){std::random_device rand;p=fs::temp_directory_path()/("archivefs-creation-"+std::to_string(rand())+"-"+std::to_string(rand()));require(fs::create_directory(p),"create temp");}~Temp(){std::error_code ec;fs::remove_all(p,ec);}};
int main(int argc,char** argv){try{
    require(argc==3,"missing test tool paths");
#ifdef _WIN32
    std::setlocale(LC_ALL,".UTF-8");
#else
    std::setlocale(LC_ALL,"C.UTF-8");
#endif
    Temp temp;auto root=temp.p/"sources";fs::create_directory(root);auto folder=root/"folder";fs::create_directory(folder);auto input=root/"data.bin";auto name=fs::path(u"文件 名称.txt");
    std::vector<char> bytes(8193);for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<char>((i*17+10)%251);{std::ofstream f(input,std::ios::binary);f.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));}{std::ofstream f(folder/name,std::ios::binary);f<<"Unicode";}
    std::vector<std::byte> tar;write_tar({input,folder},[&](const void* ptr,std::size_t n){auto p=static_cast<const std::byte*>(ptr);tar.insert(tar.end(),p,p+n);});require(tar.size()%512==0,"TAR alignment");
    auto ar=archive_read_new();require(ar!=nullptr,"create reader");struct Reader{struct archive* ar;~Reader(){archive_read_free(ar);}}reader{ar};archive_read_support_format_tar(ar);archive_read_support_filter_none(ar);require(archive_read_open_memory(ar,tar.data(),tar.size())==ARCHIVE_OK,"open TAR");std::set<std::string> names;struct archive_entry* entry=nullptr;
    for(;;){int rc=archive_read_next_header(ar,&entry);if(rc==ARCHIVE_EOF)break;require(rc==ARCHIVE_OK,"read TAR header");auto n=std::string(archive_entry_pathname_utf8(entry));names.insert(n);if(n=="data.bin"){std::vector<char> actual(bytes.size());std::size_t p=0;while(p<actual.size()){auto got=archive_read_data(ar,actual.data()+p,actual.size()-p);require(got>0,"TAR short read");p+=static_cast<std::size_t>(got);}require(actual==bytes,"TAR byte mismatch");}}
    require(names.contains("data.bin")&&names.contains("folder/"+to_utf8(name.u16string())),"TAR names");
    auto tool=fs::path(argv[2]);auto tools=temp.p/"tools";fs::create_directory(tools);for(auto n:{"mksquashfs","mkdwarfs","mkfs.erofs"}){
#ifdef _WIN32
        fs::copy_file(tool,tools/(std::string(n)+".exe"));
#else
        fs::copy_file(tool,tools/n);
#endif
    }
    ImageCreator creator(tools,fs::path(argv[1]));
    for(auto format:{ArchiveFormat::squashfs,ArchiveFormat::dwarfs,ArchiveFormat::erofs})for(unsigned level=1;level<=8;++level){auto settings=CreationOptions::for_preset(format,static_cast<CompressionPreset>(level));require(!settings.arguments().empty(),"preset arguments");auto output=temp.p/(std::to_string(static_cast<int>(format))+"-"+std::to_string(level)+".image");creator.create({input,folder},output,settings,[](std::string_view line){std::cerr<<line<<'\n';});require(detect_format(*open_file_source(output))==format,"created image magic");if(format==ArchiveFormat::erofs){auto source=open_file_source(output);std::vector<char> data(bytes.size());source->read_exact(1028,std::as_writable_bytes(std::span(data)));require(data==bytes,"EROFS TAR transport corrupted binary data");}}
    auto options=CreationOptions::for_preset(ArchiveFormat::squashfs,CompressionPreset::balanced);throws([&]{creator.create({root},root/"nested.img",options);});throws([&]{creator.create({input},input,options);});std::stop_source cancelled;cancelled.request_stop();throws([&]{creator.create({input},temp.p/"cancelled.img",options,{},cancelled.get_token());});require(!fs::exists(temp.p/"cancelled.img"),"cancel output");
    auto batch=temp.p/"batch";fs::create_directory(batch);
    auto outputs=creator.create_batch({input,folder},batch,options);require(outputs.size()==2,"two separate archives");
    require(outputs[0].filename()=="data.bin.squashfs"&&outputs[1].filename()=="folder.squashfs","batch output names");
    for(auto const& output:outputs)require(detect_format(*open_file_source(output))==ArchiveFormat::squashfs,"batch output format");
    throws([&]{creator.create_batch({input,folder},batch,options);});
    auto collision=temp.p/"collision";fs::create_directory(collision);throws([&]{creator.create_batch({input,input},collision,options);});require(fs::is_empty(collision),"duplicate names checked before creation");
    throws([&]{creator.create_batch({input},collision,options,{},cancelled.get_token());});require(fs::is_empty(collision),"cancelled batch has no output");
    std::set<std::string> logs;run_process(tool,{fs::path("echo").native()},[&](std::string_view l){logs.emplace(l);},{},[&](const auto& write){write(bytes.data(),bytes.size());});require(logs.contains("input=8193")&&logs.contains("done"),"pipe transport");throws([&]{run_process(tool,{fs::path("fail").native()});});
    // The child closes its input before consuming a large write. This must
    // return an exception, without SIGPIPE killing the parent.
    throws([&]{run_process(tool,{fs::path("fail").native()},{},{},[&](const auto& write){std::vector<char> big(1u<<20);write(big.data(),big.size());});});
    for(const auto& e:fs::directory_iterator(temp.p))require(!e.path().filename().string().starts_with(".archivefs-"),"staging leak");
    std::cout<<"PASS TAR Unicode and binary stream, 24 format presets, output validation, cancellation, process failure\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
