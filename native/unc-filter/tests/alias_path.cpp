// SPDX-License-Identifier: GPL-3.0-only
#include "../alias_path.h"
#include "../../cpp/unc_alias_policy.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Text=std::vector<ArchiveFsChar>;
Text units(std::u16string_view s){return {s.begin(),s.end()};}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void mapped(std::u16string_view path,std::u16string_view expected,bool unqualified=true){
    auto input=units(path),target=units(expected);
    ArchiveFsAlias alias{};
    require(ArchiveFsParseAlias(input.data(),input.size(),unqualified,&alias)==ArchiveFsAliasMatched,"matching alias");
    require(alias.target_length==target.size(),"target length");
    std::vector<uint16_t> output(alias.target_length+2,0xdead);
    ArchiveFsMakeAlias(input.data(),input.size(),&alias,output.data()+1);
    require(output.front()==0xdead&&output.back()==0xdead,"output bounds");
    require(std::equal(target.begin(),target.end(),output.begin()+1),"target path");
    ArchiveFsAlias second{};
    require(ArchiveFsParseAlias(target.data(),target.size(),unqualified,&second)==ArchiveFsAliasUnrelated,"no recursive alias");
}
void rejected(std::u16string_view path,ArchiveFsAliasResult result,bool unqualified=true){
    auto input=units(path);ArchiveFsAlias alias{};
    require(ArchiveFsParseAlias(input.data(),input.size(),unqualified,&alias)==result,"rejected alias");
}
}
int main(){
    try{
        mapped(u"\\sfs.NAS\\backup\\a.tar.gz\\目录\\文件.bin",u"\\Device\\Mup\\sfs$\\UNC\\NAS\\backup\\a.tar.gz\\目录\\文件.bin");
        mapped(u"\\SFS.192.168.1.10\\share$",u"\\Device\\Mup\\sfs$\\UNC\\192.168.1.10\\share$");
        mapped(u"\\sfs.nas.example.localhost\\share\\",u"\\Device\\Mup\\sfs$\\UNC\\nas.example\\share\\",false);
        mapped(u"\\Device\\Mup\\SFS.nas.localhost\\share\\😀.zip",u"\\Device\\Mup\\sfs$\\UNC\\nas\\share\\😀.zip",false);
        mapped(u"\\sfs.fe80--1.ipv6-literal.net\\data",u"\\Device\\Mup\\sfs$\\UNC\\fe80--1.ipv6-literal.net\\data");
        for(auto name:{u"\\sfs$\\UNC\\nas\\share",u"\\sfs.localhost\\C",u"\\nas\\share",u"\\;LanmanRedirector\\;Z:123\\sfs.nas\\share",u"\\sfs-nas\\share"})
            rejected(name,ArchiveFsAliasUnrelated);
        rejected(u"\\sfs.nas\\share",ArchiveFsAliasUnrelated,false);
        for(auto name:{u"\\sfs.\\share",u"\\sfs.nas",u"\\sfs.nas\\",u"\\sfs..nas\\share",u"\\sfs.sfs.nas\\share",u"\\sfs.nas\\..\\file",u"\\sfs.nas\\share\\.. \\file",u"\\sfs.nas\\share\\\\file",u"\\sfs.nas\\share\\a:b",u"\\sfs.-nas\\share",u"\\sfs.nas\\share\\a."})
            rejected(name,ArchiveFsAliasInvalid);
        require(archivefs::reserved_archive_host(u"SFS.NAS",true),"unqualified alias reservation");
        require(!archivefs::reserved_archive_host(u"SFS.NAS",false),"ordinary DNS hostname when disabled");
        require(archivefs::reserved_archive_host(u"sfs.nas.localhost",false),"qualified alias reservation");
        require(archivefs::reserved_archive_host(u"sfs$",false),"fixed UNC namespace");
        require(!archivefs::reserved_archive_host(u"nas.localhost",true),"unrelated hostname");
        auto embedded=units(u"\\sfs.nas\\share\\file");embedded[15]=0;
        ArchiveFsAlias alias{};
        require(ArchiveFsParseAlias(embedded.data(),embedded.size(),1,&alias)==ArchiveFsAliasInvalid,"embedded NUL rejected");
        std::mt19937 rng(2026);
        for(unsigned i=0;i<100000;++i){
            Text input=units(u"\\sfs.");
            for(unsigned j=0,n=rng()%300;j<n;++j)input.push_back(static_cast<uint16_t>(rng()));
            auto result=ArchiveFsParseAlias(input.data(),input.size(),1,&alias);
            if(result==ArchiveFsAliasMatched){
                require(alias.target_length<=32767&&alias.host_begin+alias.host_length<=input.size()&&alias.share_begin<input.size(),"valid ranges");
                std::vector<uint16_t> target(alias.target_length+2,0xbeef);
                ArchiveFsMakeAlias(input.data(),input.size(),&alias,target.data()+1);
                require(target.front()==0xbeef&&target.back()==0xbeef,"fuzz bounds");
            }
        }
        std::cout<<"PASS UNC alias path validation, UTF-16, reserved namespaces and 100000 randomized inputs\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
