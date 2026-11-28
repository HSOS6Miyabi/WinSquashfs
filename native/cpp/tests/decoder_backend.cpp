// SPDX-License-Identifier: GPL-3.0-only
#include "../decoder_backend.hpp"
#include "../archive_tree.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace archivefs;
static void require(bool condition) { if (!condition) throw std::runtime_error("decoder integration test failed"); }
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("archivefs-decoder-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { require(std::filesystem::create_directory(path)); }
    ~TemporaryDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
static std::byte sample(std::size_t n, unsigned seed) {
    return std::byte(static_cast<unsigned char>((n * 17 + seed) % 251));
}
int main(int argc,char** argv) {
    // Archive names can contain arbitrary Unix byte strings.
    for(std::uint32_t cp=0;cp<=0x10ffff;cp+=97){
        if(cp>=0xd800&&cp<=0xdfff)continue;
        std::u16string u;
        if(cp<=0xffff)u.push_back(static_cast<char16_t>(cp));
        else{auto v=cp-0x10000;u.push_back(static_cast<char16_t>(0xd800+(v>>10)));u.push_back(static_cast<char16_t>(0xdc00+(v&0x3ff)));}
        if(to_utf16(to_utf8(u))!=u)throw std::runtime_error("Unicode round trip");
    }
    for(auto bytes:{std::string("\x80"),std::string("\xc0\xaf"),std::string("\xed\xa0\x80"),std::string("\xf4\x90\x80\x80"),std::string("\xe2\x82")}){
        bool rejected=false;try{(void)to_utf16(bytes);}catch(const std::invalid_argument&){rejected=true;}
        if(!rejected||encode_windows_name(bytes).empty())throw std::runtime_error("invalid UTF-8 filename handling");
    }

    require(argc==2);
    require(encode_windows_name("AUX.txt")==u"%0041UX.txt");
    require(encode_windows_name("a%. ")==u"a%0025%002E%0020");
    require(encode_windows_name("\\file")==u"%005Cfile");
    require(encode_windows_name(std::string(250,'a')).size()==215);
    require(to_utf8(to_utf16("文件😀.txt"))=="文件😀.txt");
    TemporaryDirectory dir;
    auto run = [&](ArchiveFormat format, unsigned seed) {
        auto path=dir.path / (std::to_string(seed)+".img");
        std::vector<std::byte> bytes(1028+8193);
        for (std::size_t i=1028;i<bytes.size();++i) bytes[i]=sample(i-1028,seed);
        if(format==ArchiveFormat::squashfs) {
            bytes[0]=std::byte{'h'};bytes[1]=std::byte{'s'};bytes[2]=std::byte{'q'};bytes[3]=std::byte{'s'};
        } else if(format==ArchiveFormat::dwarfs) {
            const char magic[]="DWARFS";
            for(unsigned i=0;i<6;++i) bytes[i]=std::byte(magic[i]);
        } else {
            bytes[1024]=std::byte{0xe2};bytes[1025]=std::byte{0xe1};bytes[1026]=std::byte{0xf5};bytes[1027]=std::byte{0xe0};
        }
        {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));require(static_cast<bool>(out));}
        std::shared_ptr<const RandomAccessSource> source=open_file_source(path);
        auto reader=std::make_shared<ArchiveReader>(open_decoder_backend(source,argv[1]),1<<20,256);
        auto& archive=*reader;
        require(archive.info().format==format && archive.info().root==1);
        auto root=archive.list(1);
        require(root.size()==2 && root.front().name=="data.bin" && root.front().info.size==8193);
        require(archive.list(3).front().info.inode==4);
        std::vector<std::jthread> workers;
        for(unsigned n=0;n<8;++n) workers.emplace_back([&,n] {
            for(unsigned i=0;i<40;++i) {
                std::array<std::byte,257> data{};
                auto offset=(i*103+n*131)%7936;
                require(archive.read_at(2,offset,data)==data.size());
                for(std::size_t k=0;k<data.size();++k) require(data[k]==sample(offset+k,seed));
            }
        });
        workers.clear();
        require(archive.cache_statistics().hits>0);
        std::array<std::byte,23> tail{};
        require(archive.read_at(4,15,tail)==5);
        for(unsigned i=0;i<5;++i) require(tail[i]==sample(15+i,seed));
        ArchiveTree tree(reader);
        auto found=tree.find(u"\\FOLDER\\SMALL.TXT");
        require(found&&found->info.size==20);
        require(!tree.find(u"folder/../data.bin"));
        auto report=tree.verify();
        require(report.files==2&&report.bytes==8213);
        auto output=dir.path/(std::to_string(seed)+"-extracted");
        tree.extract(tree.root(),output);
        require(std::filesystem::file_size(output/"data.bin")==8193);
        require(std::filesystem::file_size(output/"folder"/"small.txt")==20);
        std::ifstream file(output/"data.bin",std::ios::binary);
        for(std::size_t i=0;i<8193;++i) require(static_cast<unsigned char>(file.get())==std::to_integer<unsigned char>(sample(i,seed)));
        require(file.get()==std::char_traits<char>::eof());
        auto output_one=dir.path/(std::to_string(seed)+"-small.txt");
        tree.extract(found,output_one);
        require(std::filesystem::file_size(output_one)==20);
    };
    std::jthread a(run,ArchiveFormat::squashfs,10), b(run,ArchiveFormat::dwarfs,20), c(run,ArchiveFormat::erofs,30);
    a.join();b.join();c.join();
    std::cout<<"PASS three decoder ABIs, callback isolation, directory metadata and concurrent random reads\n";
}
