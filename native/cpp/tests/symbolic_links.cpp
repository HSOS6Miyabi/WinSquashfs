// SPDX-License-Identifier: GPL-3.0-only
#include "projection.hpp"
#include <array>
#include <iostream>
#include <map>
#include <cstring>
using namespace archivefs;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
class Backend final:public ArchiveBackend{
public:
    struct Record{FileInfo info;std::string data;};
    std::map<InodeId,Record> records;
    std::map<InodeId,std::vector<DirectoryEntry>> directories;
    Backend(){
        auto add=[&](InodeId parent,InodeId id,std::string name,FileKind kind,std::string data={}){
            FileInfo info{id,kind,data.size(),0};records.emplace(id,Record{info,std::move(data)});
            if(parent)directories[parent].push_back({std::move(name),info});
        };
        add(0,1,"",FileKind::directory);
        add(1,2,"folder",FileKind::directory);
        add(1,3,"Data",FileKind::regular,"UPPER");
        add(1,4,"data",FileKind::regular,"lower");
        add(2,5,"中文.txt",FileKind::regular,"hello");
        add(1,6,"absolute",FileKind::symlink,"/folder/中文.txt");
        add(2,7,"relative",FileKind::symlink,"../data");
        add(1,8,"dir-link",FileKind::symlink,"folder");
        add(1,9,"cycle1",FileKind::symlink,"cycle2");
        add(1,10,"cycle2",FileKind::symlink,"cycle1");
        add(1,11,"outside",FileKind::symlink,"../Windows/system.ini");
        add(1,12,"dangling",FileKind::symlink,"/missing");
        add(1,13,"pipe",FileKind::special);
        add(1,14,"case-missing",FileKind::symlink,"DATA");
        add(1,15,"trailing-slash",FileKind::symlink,"data/");
        add(1,16,"directory-parent",FileKind::symlink,"folder/..");
        add(1,17,"link-chain",FileKind::symlink,"dir-link/relative");
    }
    ArchiveInfo info()const override{return {ArchiveFormat::squashfs,1,4096,4096};}
    FileInfo stat(InodeId id)const override{return records.at(id).info;}
    std::vector<DirectoryEntry> list(InodeId id)const override{return directories.at(id);}
    void read_exact(InodeId id,std::uint64_t offset,std::span<std::byte> output)const override{
        auto& data=records.at(id).data;
        check(offset<=data.size()&&output.size()<=data.size()-offset,"read bounds");
        if(!output.empty())std::memcpy(output.data(),data.data()+offset,output.size());
    }
    void validate_source()const override{}
};
}
int main(){try{
    auto tree=std::make_shared<ArchiveTree>(std::make_shared<ArchiveReader>(std::make_shared<Backend>()));
    auto absolute=tree->find(u"absolute");
    check(absolute&&tree->read_link(absolute)=="/folder/中文.txt","read link target");
    check(tree->resolve_link(absolute)->info.inode==5,"absolute link confined to archive");
    check(tree->resolve_link(tree->find(u"folder/relative"))->info.inode==4,"relative link uses case-sensitive original names");
    check(tree->resolve_link(tree->find(u"link-chain"))->info.inode==4,"link chain");
    check(tree->find(u"dir-link/中文.txt")->info.inode==5,"directory link traversal");
    check(tree->resolve_link(tree->find(u"directory-parent"))==tree->root(),"internal parent traversal");
    for(auto name:{u"cycle1",u"cycle2",u"outside",u"dangling",u"case-missing",u"trailing-slash"})
        check(!tree->resolve_link(tree->find(name)),"unsafe or invalid link resolved");
    auto disk=mount_projection(tree);
    auto root=disk->open(u"\\");auto listing=disk->list(root);
    check(listing.size()==14,"all root entries remain visible");
    auto alias=disk->open(u"\\absolute");
    std::array<std::byte,5> data{};
    check(alias&&alias->info.size==5&&!alias->info.unavailable&&disk->read(alias,0,data)==5,"projected symbolic link");
    check(std::memcmp(data.data(),"hello",5)==0,"symbolic link file content");
    auto directory=disk->open(u"\\dir-link");
    check(directory&&directory->info.directory&&disk->list(directory).size()==2,"projected linked directory");
    check(disk->open(u"\\dir-link\\中文.txt")->entry->info.inode==5,"open through linked directory");
    for(auto name:{u"\\cycle1",u"\\outside",u"\\dangling",u"\\pipe"}){
        auto file=disk->open(name);
        check(file&&file->info.unavailable,"unavailable object is visible");
        bool denied=false;try{disk->read(file,0,data);}catch(const std::invalid_argument&){denied=true;}
        check(denied,"unavailable object has no data stream");
    }
    check(tree->verify().files==3,"verify does not recursively traverse link aliases");
    std::cout<<"PASS symbolic links, original filename case, confined resolution, cycles and special objects\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
