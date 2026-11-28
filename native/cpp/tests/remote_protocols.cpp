// SPDX-License-Identifier: GPL-3.0-only
#include "remote_projection.hpp"
#include <array>
#include <cstring>
#include <iostream>
using namespace archivefs;
namespace {
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
}
int main(int argc,char** argv){
    try{
        if(argc!=4)return 2;
        RemoteConnection c;c.name="test";c.endpoint=argv[1];c.root="/dav";c.cache_mib=8;c.read_cache_mib=0;c.metadata_seconds=0;
        if(std::string_view(argv[3])=="writeback")c.cache_mode="writeback";
        auto p=remote_projection(RemoteProtocol::dav,{c},argv[2]);
        auto n=namespace_projection();n->replace_routes({{u"dav",p}});
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
