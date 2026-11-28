// SPDX-License-Identifier: GPL-3.0-only
#include "remote_transport.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace archivefs;
namespace {
std::string env(const char* key){auto value=std::getenv(key);return value?value:"";}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
RemoteInput bytes(std::string data){return [data=std::move(data)](std::uint64_t offset,std::span<std::byte> output){auto n=std::min(output.size(),data.size()-static_cast<std::size_t>(offset));std::memcpy(output.data(),data.data()+static_cast<std::size_t>(offset),n);return n;};}
std::string read(RemoteTransport& t,std::string_view path){
    auto info=t.stat(path);std::string value(static_cast<std::size_t>(info.size),'\0');
    t.read(path,0,std::as_writable_bytes(std::span(value)),info.revision);return value;
}
}
int main(int argc,char** argv){
    try{
        if(argc!=2)return 2;
        RemoteConnection c;c.name="test";c.protocol=parse_remote_protocol(argv[1]);c.endpoint=env("REMOTE_ENDPOINT");c.root=env("REMOTE_ROOT");c.username=env("REMOTE_USER");c.secret=env("REMOTE_SECRET");c.host_key=env("REMOTE_HOST_KEY");c.private_key=env("REMOTE_PRIVATE_KEY");c.bucket=env("REMOTE_BUCKET");c.ca_file=env("REMOTE_CA_FILE");c.shell_root=env("REMOTE_SHELL_ROOT");c.sudo=env("REMOTE_SUDO")=="1";c.ssh_helper=env("REMOTE_DISABLE_HELPER")!="1";
        if(c.protocol==RemoteProtocol::sftp||c.protocol==RemoteProtocol::scp)
            check(probe_ssh_host_key(c)==c.host_key,"SSH host key");
        auto t=make_remote_transport(c);
        check(t->list("/").empty(),"remote test directory not empty");
        t->mkdir("/test-dir");
        t->replace("/test-dir/data",10,bytes("0123456789"),{},true);
        check(t->stat("/test-dir/data").size==10,"remote stat");
        std::array<std::byte,4> output{};t->read("/test-dir/data",3,output);
        check(!std::memcmp(output.data(),"3456",4),"remote positioned read");
        std::string payload="AB";
        bool direct=t->write_at("/test-dir/data",2,std::as_bytes(std::span(payload)),t->stat("/test-dir/data").revision);
        if(c.protocol==RemoteProtocol::sftp||c.protocol==RemoteProtocol::scp)check(direct,"SSH native random write");
        if(!direct)t->replace("/test-dir/data",10,bytes("01AB456789"),t->stat("/test-dir/data").revision);
        check(read(*t,"/test-dir/data")=="01AB456789","remote written data");
        if(c.protocol==RemoteProtocol::ftp||c.protocol==RemoteProtocol::ftps){
            check(t->write_at("/test-dir/data",10,std::as_bytes(std::span(payload)),t->stat("/test-dir/data").revision),"FTP direct append");
            check(read(*t,"/test-dir/data")=="01AB456789AB","FTP appended bytes");
        }
        if(c.protocol==RemoteProtocol::sftp||c.protocol==RemoteProtocol::scp||c.protocol==RemoteProtocol::s3){
            check(t->copy("/test-dir/data","/test-dir/copy",false),"server-side copy");
            check(read(*t,"/test-dir/copy")==read(*t,"/test-dir/data"),"copied bytes");
            t->remove("/test-dir/copy",false);
        }
        t->rename("/test-dir/data","/test-dir/moved",false);
        check(!read(*t,"/test-dir/moved").empty(),"renamed content");
        t->remove("/test-dir/moved",false);t->remove("/test-dir",true);
        check(t->list("/").empty(),"remote deletion");
        if(c.protocol==RemoteProtocol::s3){
            constexpr std::uint64_t length=(64ULL<<20)+913;
            auto input=[](std::uint64_t offset,std::span<std::byte> out){
                for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<std::byte>((offset+i)%251);
                return out.size();
            };
            t->mkdir("/source");
            t->mkdir("/source/empty");
            t->mkdir("/source/nested");
            t->replace("/source/nested/file +%.bin",length,input,{},true);
            auto original=t->stat("/source/nested/file +%.bin");
            check(original.size==length&&original.revision.find('-')!=std::string::npos,"multipart upload");
            t->rename("/source","/destination",false);
            check(t->list("/").size()==1&&t->stat("/destination/empty").directory,"directory rename including empty marker");
            auto info=t->stat("/destination/nested/file +%.bin");check(info.size==length,"multipart copy size");
            for(auto offset:std::array<std::uint64_t,3>{0,(16ULL<<20)-100,length-100}){
                std::array<std::byte,100> out{},expected{};input(offset,expected);
                t->read("/destination/nested/file +%.bin",offset,out,info.revision);check(out==expected,"multipart content");
            }
            t->remove("/destination/nested/file +%.bin",false);t->remove("/destination/nested",true);
            t->remove("/destination/empty",true);t->remove("/destination",true);
            check(t->list("/").empty(),"multipart rename cleanup");
        }
        std::cout<<"PASS actual "<<argv[1]<<" server operations\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
