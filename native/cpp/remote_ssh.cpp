// SPDX-License-Identifier: GPL-3.0-only
#include "remote_transport.hpp"
#include <libssh2.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <utility>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace archivefs {
namespace {
[[noreturn]] void fail(RemoteError c,std::string m){throw RemoteFailure(c,std::move(m));}
#ifdef _WIN32
using Socket=SOCKET;
constexpr Socket invalid_socket=INVALID_SOCKET;
void close_socket(Socket s){closesocket(s);}
bool nonblock(Socket s,bool enabled){u_long value=enabled?1u:0u;return ioctlsocket(s,FIONBIO,&value)==0;}
bool pending(){auto e=WSAGetLastError();return e==WSAEWOULDBLOCK||e==WSAEINPROGRESS||e==WSAEINTR;}
void socket_runtime(){struct Init{Init(){WSADATA d{};if(WSAStartup(MAKEWORD(2,2),&d))fail(RemoteError::offline,"Winsock initialization failed");}~Init(){WSACleanup();}};static Init init;}
#else
using Socket=int;
constexpr Socket invalid_socket=-1;
void close_socket(Socket s){::close(s);}
bool nonblock(Socket s,bool enabled){auto flags=fcntl(s,F_GETFL,0);return flags>=0&&fcntl(s,F_SETFL,enabled?flags|O_NONBLOCK:flags&~O_NONBLOCK)==0;}
bool pending(){return errno==EINPROGRESS||errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR;}
void socket_runtime(){}
#endif
struct Endpoint{std::string host,port="22";};
Endpoint endpoint(std::string_view value){
    Endpoint e;
    if(value.starts_with('[')){
        auto end=value.find(']');if(end==value.npos||end<2)throw std::invalid_argument("Invalid SSH IPv6 host");
        e.host=value.substr(1,end-1);
        if(end+1<value.size()){if(value[end+1]!=':')throw std::invalid_argument("Invalid SSH port");e.port=value.substr(end+2);}
    }else{
        auto pos=value.rfind(':');
        if(pos!=value.npos){if(value.find(':')!=pos)throw std::invalid_argument("Bracket IPv6 hosts");e.host=value.substr(0,pos);e.port=value.substr(pos+1);}
        else e.host=value;
    }
    unsigned port=0;auto r=std::from_chars(e.port.data(),e.port.data()+e.port.size(),port);
    if(e.host.empty()||e.host.find_first_of(" \t\r\n/\\@?#")!=e.host.npos||r.ec!=std::errc{}||r.ptr!=e.port.data()+e.port.size()||!port||port>65535)
        throw std::invalid_argument("Invalid SSH endpoint");
    return e;
}
struct SocketOwner{
    Socket value=invalid_socket;
    ~SocketOwner(){if(value!=invalid_socket)close_socket(value);}
    SocketOwner()=default;SocketOwner(const SocketOwner&)=delete;
};
void connect_socket(SocketOwner& owner,const Endpoint& e,unsigned timeout){
    socket_runtime();
    addrinfo hints{};hints.ai_socktype=SOCK_STREAM;hints.ai_family=AF_UNSPEC;addrinfo* raw=nullptr;
    if(getaddrinfo(e.host.c_str(),e.port.c_str(),&hints,&raw))fail(RemoteError::offline,"Cannot resolve SSH host");
    std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> addresses(raw,freeaddrinfo);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(std::min(timeout,30u));
    for(auto a=raw;a;a=a->ai_next){
        SocketOwner candidate;candidate.value=socket(a->ai_family,a->ai_socktype,a->ai_protocol);
        if(candidate.value==invalid_socket||!nonblock(candidate.value,true))continue;
        auto result=::connect(candidate.value,a->ai_addr,static_cast<int>(a->ai_addrlen));
        if(result!=0){
            if(!pending())continue;
            auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now());
            if(remaining.count()<=0)break;
            fd_set write;FD_ZERO(&write);
#ifndef _WIN32
            if(candidate.value>=FD_SETSIZE)continue;
#endif
            FD_SET(candidate.value,&write);
            timeval tv{static_cast<long>(remaining.count()/1000),static_cast<long>((remaining.count()%1000)*1000)};
#ifdef _WIN32
            if(select(0,nullptr,&write,nullptr,&tv)<=0)continue;
            int error=0,length=sizeof(error);
            if(getsockopt(candidate.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length)||error)continue;
#else
            if(select(candidate.value+1,nullptr,&write,nullptr,&tv)<=0)continue;
            int error=0;socklen_t length=sizeof(error);
            if(getsockopt(candidate.value,SOL_SOCKET,SO_ERROR,&error,&length)||error)continue;
#endif
        }
        if(!nonblock(candidate.value,false))continue;
        owner.value=std::exchange(candidate.value,invalid_socket);return;
    }
    fail(RemoteError::offline,"Cannot connect to SSH host");
}
std::string fingerprint(LIBSSH2_SESSION* session){
    auto p=libssh2_hostkey_hash(session,LIBSSH2_HOSTKEY_HASH_SHA256);
    if(!p)fail(RemoteError::protocol,"SSH server did not provide a host key");
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out="SHA256:";
    unsigned bits=0,value=0;
    for(unsigned i=0;i<32;++i){value=(value<<8)|static_cast<unsigned char>(p[i]);bits+=8;while(bits>=6){bits-=6;out+=alphabet[(value>>bits)&63];}}
    if(bits)out+=alphabet[(value<<(6-bits))&63];
    return out;
}
struct SessionDeleter{void operator()(LIBSSH2_SESSION* s)const noexcept{if(s){libssh2_session_disconnect(s,"ArchiveFS disconnect");libssh2_session_free(s);}}};
struct ChannelDeleter{void operator()(LIBSSH2_CHANNEL* c)const noexcept{if(c){libssh2_channel_close(c);libssh2_channel_free(c);}}};
using Channel=std::unique_ptr<LIBSSH2_CHANNEL,ChannelDeleter>;
class Session{
public:
    explicit Session(const RemoteConnection& c,bool authenticate=true){
        static const struct Runtime{Runtime(){if(libssh2_init(0))fail(RemoteError::offline,"libssh2 initialization failed");}~Runtime(){libssh2_exit();}} runtime;
        (void)runtime;
        connect_socket(socket_,endpoint(c.endpoint),c.timeout_seconds);
        session_.reset(libssh2_session_init());if(!session_)throw std::bad_alloc();
        libssh2_session_set_blocking(session_.get(),1);
        libssh2_session_set_timeout(session_.get(),static_cast<long>(c.timeout_seconds)*1000L);
        check(libssh2_session_handshake(session_.get(),socket_.value),"SSH handshake failed");
        host_key_=fingerprint(session_.get());
        if(!authenticate)return;
        if(c.host_key!=host_key_)fail(RemoteError::permission,"SSH host key differs from the pinned fingerprint");
        int rc=0;
        if(!c.private_key.empty()){
            auto path=std::filesystem::path(std::u8string(c.private_key.begin(),c.private_key.end()));
            auto size=std::filesystem::file_size(path);
            if(size>1u<<20)throw std::invalid_argument("SSH private key exceeds size limit");
            std::ifstream in(path,std::ios::binary);std::string key(static_cast<std::size_t>(size),'\0');
            if(!in.read(key.data(),static_cast<std::streamsize>(key.size())))fail(RemoteError::permission,"Cannot read SSH private key");
            struct Clear{std::string& data;~Clear(){volatile char* p=data.data();for(std::size_t i=0;i<data.size();++i)p[i]=0;}} clear{key};
            rc=libssh2_userauth_publickey_frommemory(session_.get(),c.username.c_str(),c.username.size(),
                nullptr,0,key.data(),key.size(),c.secret.empty()?nullptr:c.secret.c_str());
        }else rc=libssh2_userauth_password_ex(session_.get(),c.username.c_str(),static_cast<unsigned>(c.username.size()),
                c.secret.c_str(),static_cast<unsigned>(c.secret.size()),nullptr);
        if(rc)fail(RemoteError::permission,"SSH authentication failed");
    }
    const std::string& host_key()const noexcept{return host_key_;}
    Channel channel(){
        Channel c(libssh2_channel_open_session(session_.get()));
        if(!c)fail(RemoteError::offline,"Cannot open SSH channel");
        check(libssh2_channel_handle_extended_data2(c.get(),LIBSSH2_CHANNEL_EXTENDED_DATA_IGNORE),"Cannot configure SSH channel");
        return c;
    }
    static void check(int result,const char* message){
        if(result<0)fail(result==LIBSSH2_ERROR_TIMEOUT||result==LIBSSH2_ERROR_SOCKET_TIMEOUT?RemoteError::timeout:RemoteError::protocol,message);
    }
private:
    SocketOwner socket_;
    std::unique_ptr<LIBSSH2_SESSION,SessionDeleter> session_;
    std::string host_key_;
};
void write_all(LIBSSH2_CHANNEL* channel,std::span<const std::byte> data){
    while(!data.empty()){
        auto count=libssh2_channel_write_ex(channel,0,reinterpret_cast<const char*>(data.data()),std::min<std::size_t>(data.size(),1u<<20));
        if(count<=0)fail(count==LIBSSH2_ERROR_TIMEOUT?RemoteError::timeout:RemoteError::offline,"SSH channel write failed");
        data=data.subspan(static_cast<std::size_t>(count));
    }
}
void write_all(LIBSSH2_CHANNEL* c,std::string_view s){write_all(c,std::as_bytes(std::span(s)));}
void read_all(LIBSSH2_CHANNEL* channel,std::span<std::byte> data){
    while(!data.empty()){
        auto count=libssh2_channel_read_ex(channel,0,reinterpret_cast<char*>(data.data()),data.size());
        if(count<=0)fail(count==LIBSSH2_ERROR_TIMEOUT?RemoteError::timeout:RemoteError::offline,"SSH channel closed during transfer");
        data=data.subspan(static_cast<std::size_t>(count));
    }
}
using Bytes=std::vector<std::byte>;
void u32(Bytes& data,std::uint32_t value){for(int i=3;i>=0;--i)data.push_back(static_cast<std::byte>((value>>(i*8))&255));}
void u64(Bytes& data,std::uint64_t value){u32(data,static_cast<std::uint32_t>(value>>32));u32(data,static_cast<std::uint32_t>(value));}
void string(Bytes& data,std::span<const std::byte> s){if(s.size()>UINT32_MAX)throw std::length_error("SFTP string too long");u32(data,static_cast<std::uint32_t>(s.size()));data.insert(data.end(),s.begin(),s.end());}
void string(Bytes& data,std::string_view s){string(data,std::as_bytes(std::span(s)));}
class Parser{
public:
    explicit Parser(std::span<const std::byte> data):data_(data){}
    unsigned byte(){need(1);auto x=std::to_integer<unsigned>(data_[0]);data_=data_.subspan(1);return x;}
    std::uint32_t u32(){std::uint32_t x=0;for(int i=0;i<4;++i)x=(x<<8)|byte();return x;}
    std::uint64_t u64(){auto hi=u32();return (static_cast<std::uint64_t>(hi)<<32)|u32();}
    std::span<const std::byte> bytes(){auto n=u32();need(n);auto s=data_.first(n);data_=data_.subspan(n);return s;}
    std::string string(){auto s=bytes();return {reinterpret_cast<const char*>(s.data()),s.size()};}
    bool empty()const{return data_.empty();}
private:
    std::span<const std::byte> data_;
    void need(std::size_t n){if(n>data_.size())fail(RemoteError::protocol,"Truncated SFTP packet");}
};
constexpr unsigned fxp_init=1,fxp_version=2,fxp_open=3,fxp_close=4,fxp_read=5,fxp_write=6,fxp_lstat=7,fxp_fsetstat=10,
    fxp_opendir=11,fxp_readdir=12,fxp_remove=13,fxp_mkdir=14,fxp_rmdir=15,fxp_rename=18,
    fxp_status=101,fxp_handle=102,fxp_data=103,fxp_name=104,fxp_attrs=105,fxp_extended=200;
constexpr unsigned fx_read=1,fx_write=2,fx_create=8,fx_truncate=16,fx_exclusive=32;
constexpr unsigned fx_ok=0,fx_eof=1,fx_missing=2,fx_denied=3,fx_unsupported=8;
[[noreturn]] void sftp_error(unsigned code){
    switch(code){case fx_missing:fail(RemoteError::missing,"SFTP path not found");case fx_denied:fail(RemoteError::permission,"SFTP access denied");
    case fx_unsupported:fail(RemoteError::unsupported,"SFTP operation unsupported");case 11:fail(RemoteError::exists,"SFTP path already exists");case 14:fail(RemoteError::no_space,"SFTP storage is full");
    case 18:fail(RemoteError::not_empty,"SFTP directory is not empty");default:fail(RemoteError::protocol,"SFTP server returned status "+std::to_string(code));}
}
struct Attributes{
    std::uint64_t size=0;std::uint32_t mode=0,mtime=0;
};
Attributes attributes(Parser& p){
    Attributes a;auto flags=p.u32();
    if(flags&1)a.size=p.u64();
    if(flags&2){(void)p.u32();(void)p.u32();}
    if(flags&4)a.mode=p.u32();
    if(flags&8){(void)p.u32();a.mtime=p.u32();}
    if(flags&0x80000000u){auto n=p.u32();if(n>4096)fail(RemoteError::protocol,"Too many SFTP attribute extensions");for(unsigned i=0;i<n;++i){(void)p.bytes();(void)p.bytes();}}
    if(flags&~0x8000000fu)fail(RemoteError::protocol,"Unexpected SFTP attributes");
    return a;
}
std::optional<RemoteStat> metadata(std::string name,const Attributes& a){
    auto type=a.mode&0170000u;
    if(type!=0040000u&&type!=0100000u)return {};
    bool directory=type==0040000u;
    return RemoteStat{std::move(name),directory,directory?0:a.size,a.mtime,std::to_string(a.size)+":"+std::to_string(a.mtime)};
}
class Sftp{
public:
    explicit Sftp(Session& session):channel_(session.channel()){
        Session::check(libssh2_channel_subsystem(channel_.get(),"sftp"),"Cannot start SFTP subsystem");
        Bytes init{static_cast<std::byte>(fxp_init)};u32(init,3);send(init);
        auto packet=receive();Parser p(packet);
        if(p.byte()!=fxp_version||p.u32()!=3)fail(RemoteError::unsupported,"SFTP server does not support version 3");
        while(!p.empty()){auto name=p.string(),value=p.string();extensions_.insert_or_assign(std::move(name),std::move(value));}
    }
    bool supports(std::string_view name)const{return extensions_.contains(name);}
    RemoteStat stat(std::string_view path){
        Bytes args;string(args,path);auto response=call(fxp_lstat,args);Parser p(response);
        auto type=p.byte();if(type==fxp_status)sftp_error(p.u32());if(type!=fxp_attrs)fail(RemoteError::protocol,"Invalid SFTP stat response");
        auto value=metadata(remote_name(path),attributes(p));if(!value)fail(RemoteError::unsupported,"SFTP object is not a regular file or directory");return *value;
    }
    struct Handle{
        Sftp* owner;Bytes value;
        Handle(Sftp* s,Bytes h):owner(s),value(std::move(h)){}
        Handle(const Handle&)=delete;
        ~Handle(){if(owner)try{close();}catch(...){}}
        void close(){if(owner){Bytes args;string(args,value);auto* s=std::exchange(owner,nullptr);s->status(fxp_close,args);}}
    };
    Handle open(std::string_view path,unsigned flags){
        Bytes args;string(args,path);u32(args,flags);u32(args,4);u32(args,0666);
        return Handle(this,handle(fxp_open,args));
    }
    std::vector<RemoteStat> list(std::string_view path){
        Bytes args;string(args,path);Handle dir(this,handle(fxp_opendir,args));std::vector<RemoteStat> result;
        for(;;){
            Bytes request;string(request,dir.value);auto response=call(fxp_readdir,request);Parser p(response);
            auto type=p.byte();if(type==fxp_status){auto code=p.u32();if(code==fx_eof)break;sftp_error(code);}
            if(type!=fxp_name)fail(RemoteError::protocol,"Invalid SFTP directory response");
            auto count=p.u32();if(!count||count>500000-result.size())fail(RemoteError::protocol,"SFTP directory exceeds limit");
            for(unsigned i=0;i<count;++i){auto name=p.string();(void)p.string();auto a=attributes(p);
                if(name=="."||name=="..")continue;
                try{(void)remote_join("/",name);}catch(const std::invalid_argument&){continue;}
                if(auto entry=metadata(std::move(name),a))result.push_back(std::move(*entry));}
        }
        dir.close();return result;
    }
    void read(Handle& h,std::uint64_t offset,std::span<std::byte> data){
        while(!data.empty()){
            auto count=static_cast<std::uint32_t>(std::min<std::size_t>(data.size(),64u<<10));
            Bytes args;string(args,h.value);u64(args,offset);u32(args,count);
            auto response=call(fxp_read,args);Parser p(response);auto type=p.byte();
            if(type==fxp_status){auto code=p.u32();if(code==fx_eof)fail(RemoteError::protocol,"Short SFTP read");sftp_error(code);}
            if(type!=fxp_data)fail(RemoteError::protocol,"Invalid SFTP read response");
            auto bytes=p.bytes();if(bytes.empty()||bytes.size()>count)fail(RemoteError::protocol,"Invalid SFTP data length");
            std::copy(bytes.begin(),bytes.end(),data.begin());offset+=bytes.size();data=data.subspan(bytes.size());
        }
    }
    void write(Handle& h,std::uint64_t offset,std::span<const std::byte> data){
        while(!data.empty()){auto count=std::min<std::size_t>(data.size(),64u<<10);Bytes args;string(args,h.value);u64(args,offset);string(args,data.first(count));status(fxp_write,args);offset+=count;data=data.subspan(count);}
    }
    void resize(Handle& h,std::uint64_t size){Bytes args;string(args,h.value);u32(args,1);u64(args,size);status(fxp_fsetstat,args);}
    void mkdir(std::string_view path){Bytes args;string(args,path);u32(args,4);u32(args,0777);status(fxp_mkdir,args);}
    void remove(std::string_view path,bool directory){Bytes args;string(args,path);status(directory?fxp_rmdir:fxp_remove,args);}
    void rename(std::string_view from,std::string_view to,bool overwrite){
        Bytes args;
        if(overwrite){if(!supports("posix-rename@openssh.com"))fail(RemoteError::unsupported,"Server lacks atomic overwrite rename");string(args,"posix-rename@openssh.com");}
        string(args,from);string(args,to);status(overwrite?fxp_extended:fxp_rename,args);
    }
    void copy(std::string_view from,std::string_view to,std::uint64_t length,bool overwrite){
        auto src=open(from,fx_read),dst=open(to,fx_write|fx_create|(overwrite?fx_truncate:fx_exclusive));
        if(length){Bytes args;string(args,"copy-data");string(args,src.value);u64(args,0);u64(args,length);string(args,dst.value);u64(args,0);status(fxp_extended,args);}
        dst.close();src.close();
    }
private:
    Channel channel_;std::uint32_t sequence_=0;std::map<std::string,std::string,std::less<>> extensions_;
    void send(std::span<const std::byte> packet){Bytes header;u32(header,static_cast<std::uint32_t>(packet.size()));write_all(channel_.get(),header);write_all(channel_.get(),packet);}
    Bytes receive(){std::array<std::byte,4> header{};read_all(channel_.get(),header);Parser p(header);auto n=p.u32();if(!n||n>4u<<20)fail(RemoteError::protocol,"Invalid SFTP packet size");Bytes packet(n);read_all(channel_.get(),packet);return packet;}
    Bytes call(unsigned type,const Bytes& args){
        Bytes packet{static_cast<std::byte>(type)};auto id=++sequence_;u32(packet,id);packet.insert(packet.end(),args.begin(),args.end());send(packet);
        auto reply=receive();Parser p(reply);auto response_type=p.byte();if(p.u32()!=id)fail(RemoteError::protocol,"SFTP request ID mismatch");
        reply.erase(reply.begin(),reply.begin()+5);reply.insert(reply.begin(),static_cast<std::byte>(response_type));return reply;
    }
    void status(unsigned type,const Bytes& args){auto response=call(type,args);Parser p(response);if(p.byte()!=fxp_status)fail(RemoteError::protocol,"Expected SFTP status");auto code=p.u32();if(code!=fx_ok)sftp_error(code);}
    Bytes handle(unsigned type,const Bytes& args){auto response=call(type,args);Parser p(response);auto kind=p.byte();if(kind==fxp_status)sftp_error(p.u32());if(kind!=fxp_handle)fail(RemoteError::protocol,"Expected SFTP handle");auto h=p.bytes();if(h.empty()||h.size()>4096)fail(RemoteError::protocol,"Invalid SFTP handle");return {h.begin(),h.end()};}
};
std::string physical(std::string_view root,std::string_view path){validate_remote_path(path);return root=="/"?std::string(path):path=="/"?std::string(root):std::string(root)+std::string(path);}
class SshTransport final:public RemoteTransport{
public:
    explicit SshTransport(RemoteConnection c):c_(std::move(c)){}
    RemoteStat stat(std::string_view path)override{
        std::lock_guard lock(mutex_);ensure();
        if(path=="/")return {"/",true,0,0,{}};
        return stat_locked(path);
    }
    std::vector<RemoteStat> list(std::string_view path)override{
        std::lock_guard lock(mutex_);ensure();
        if(sftp_)return sftp_->list(physical(c_.root,path));
        auto directory=shell_path(path),quoted=remote_shell_quote(directory);
        auto body=command("find "+quoted+" -mindepth 1 -maxdepth 1 -print0",16u<<20);
        std::vector<RemoteStat> entries;std::string_view remaining(body);
        while(!remaining.empty()){
            auto end=remaining.find('\0');if(end==remaining.npos)fail(RemoteError::protocol,"Invalid SSH directory listing");
            auto name=remaining.substr(0,end);auto prefix=directory=="/"?"/":directory+"/";
            if(name.starts_with(prefix)){auto part=name.substr(prefix.size());try{auto child=remote_join(path,part);entries.push_back(stat_locked(child));}catch(const std::invalid_argument&){}catch(const RemoteFailure& e){if(e.code()!=RemoteError::unsupported&&e.code()!=RemoteError::missing)throw;}}
            remaining.remove_prefix(end+1);if(entries.size()>500000)fail(RemoteError::protocol,"SSH directory exceeds entry limit");
        }
        return entries;
    }
    void read(std::string_view path,std::uint64_t offset,std::span<std::byte> out,std::string_view revision)override{
        if(out.empty())return;
        std::lock_guard lock(mutex_);ensure();check_revision(path,revision);
        if(sftp_){auto handle=sftp_->open(physical(c_.root,path),fx_read);sftp_->read(handle,offset,out);handle.close();return;}
        auto p=remote_shell_quote(shell_path(path));
        auto cmd="dd if="+p+" bs=1 skip="+std::to_string(offset)+" count="+std::to_string(out.size())+" 2>/dev/null";
        if(byte_flags())cmd="dd if="+p+" bs=65536 iflag=skip_bytes,count_bytes skip="+std::to_string(offset)+" count="+std::to_string(out.size())+" 2>/dev/null";
        auto data=command(cmd,out.size());
        if(data.size()!=out.size())fail(RemoteError::protocol,"Short SSH read");
        std::memcpy(out.data(),data.data(),data.size());
    }
    bool write_at(std::string_view path,std::uint64_t offset,std::span<const std::byte> data,std::string_view revision)override{
        writable(path);if(data.empty())return true;
        std::lock_guard lock(mutex_);ensure();check_revision(path,revision);
        if(sftp_){auto handle=sftp_->open(physical(c_.root,path),fx_write);sftp_->write(handle,offset,data);handle.close();return true;}
        auto p=remote_shell_quote(shell_path(path));
        auto cmd="dd of="+p+" bs=1 seek="+std::to_string(offset)+" conv=notrunc 2>/dev/null";
        if(byte_flags())cmd="dd of="+p+" bs=65536 oflag=seek_bytes seek="+std::to_string(offset)+" conv=notrunc 2>/dev/null";
        (void)command(cmd,65536,data.size(),span_input(data));return true;
    }
    bool resize(std::string_view path,std::uint64_t size)override{
        writable(path);std::lock_guard lock(mutex_);ensure();
        if(sftp_){auto handle=sftp_->open(physical(c_.root,path),fx_write);sftp_->resize(handle,size);handle.close();return true;}
        if(command("command -v truncate || true",4096).empty())return false;
        (void)command("truncate -s "+std::to_string(size)+" -- "+remote_shell_quote(shell_path(path)),65536);return true;
    }
    void replace(std::string_view path,std::uint64_t size,const RemoteInput& input,std::string_view revision,bool exclusive)override{
        writable(path);std::lock_guard lock(mutex_);ensure();check_revision(path,revision);
        if(sftp_){
            auto handle=sftp_->open(physical(c_.root,path),fx_write|fx_create|(exclusive?fx_exclusive:fx_truncate));
            std::array<std::byte,64u<<10> bytes{};std::uint64_t offset=0;
            while(offset<size){auto n=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),size-offset));auto got=input(offset,std::span(bytes).first(n));if(!got||got>n)fail(RemoteError::protocol,"Short upload source");sftp_->write(handle,offset,std::span(bytes).first(got));offset+=got;}
            handle.close();return;
        }
        auto target=shell_path(path);
        if(exclusive){
            try{(void)stat_locked(path);fail(RemoteError::exists,"SCP destination already exists");}catch(const RemoteFailure& e){if(e.code()!=RemoteError::missing)throw;}
            (void)command("set -C; : > "+remote_shell_quote(target),65536);
        }
        // The legacy SCP sink runs under the same SSH identity (or the
        // explicitly configured non-interactive sudo identity).
        auto channel=session_->channel();auto cmd=wrap("scp -t "+remote_shell_quote(target));
        Session::check(libssh2_channel_exec(channel.get(),cmd.c_str()),"Cannot start SCP receiver");
        scp_ack(channel.get());
        write_all(channel.get(),"C0600 "+std::to_string(size)+" "+remote_name(target)+"\n");scp_ack(channel.get());
        std::array<std::byte,64u<<10> bytes{};std::uint64_t offset=0;
        while(offset<size){auto n=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),size-offset));auto got=input(offset,std::span(bytes).first(n));if(!got||got>n)fail(RemoteError::protocol,"Short SCP upload source");write_all(channel.get(),std::span(bytes).first(got));offset+=got;}
        std::array<std::byte,1> end{};write_all(channel.get(),end);scp_ack(channel.get());
        finish(channel.get());
    }
    void mkdir(std::string_view path)override{
        writable(path);std::lock_guard lock(mutex_);ensure();if(sftp_)sftp_->mkdir(physical(c_.root,path));else (void)command("mkdir -- "+remote_shell_quote(shell_path(path)),65536);
    }
    void remove(std::string_view path,bool directory)override{
        writable(path);std::lock_guard lock(mutex_);ensure();if(sftp_)sftp_->remove(physical(c_.root,path),directory);else (void)command(std::string(directory?"rmdir -- ":"rm -- ")+remote_shell_quote(shell_path(path)),65536);
    }
    void rename(std::string_view from,std::string_view to,bool overwrite)override{
        writable(from);writable(to);std::lock_guard lock(mutex_);ensure();
        if(sftp_&&(!overwrite||sftp_->supports("posix-rename@openssh.com"))){sftp_->rename(physical(c_.root,from),physical(c_.root,to),overwrite);return;}
        if(sftp_&&!c_.ssh_helper)fail(RemoteError::unsupported,"Server does not support overwrite rename");
        auto a=remote_shell_quote(shell_path(from)),b=remote_shell_quote(shell_path(to));
        (void)command((overwrite?std::string{}:"test ! -e "+b+" && ")+"mv -f -- "+a+" "+b,65536);
    }
    bool copy(std::string_view from,std::string_view to,bool overwrite)override{
        writable(to);std::lock_guard lock(mutex_);ensure();auto info=stat_locked(from);
        if(info.directory)return false;
        if(from==to)throw std::invalid_argument("Source and destination are identical");
        if(sftp_&&sftp_->supports("copy-data")){
            auto temporary=remote_join(remote_parent(to),".archivefs-copy-"+std::to_string(std::random_device{}())+"-"+std::to_string(std::random_device{}()));
            auto path=physical(c_.root,temporary);
            bool copied=false;
            try{sftp_->copy(physical(c_.root,from),path,info.size,false);copied=true;}
            catch(const RemoteFailure& e){
                try{sftp_->remove(path,false);}catch(...){}
                if(e.code()!=RemoteError::unsupported)throw;
            }
            if(copied){
                try{sftp_->rename(path,physical(c_.root,to),overwrite);}
                catch(...){try{sftp_->remove(path,false);}catch(...){}throw;}
                return true;
            }
        }
        if(sftp_&&!c_.ssh_helper)return false;
        auto a=remote_shell_quote(shell_path(from)),b=remote_shell_quote(shell_path(to));
        (void)command((overwrite?std::string{}:"test ! -e "+b+" && ")+"cp -p -- "+a+" "+b,65536);return true;
    }
private:
    RemoteConnection c_;std::mutex mutex_;std::unique_ptr<Session> session_;std::unique_ptr<Sftp> sftp_;
    std::optional<bool> byte_flags_;
    void ensure(){if(!session_){auto s=std::make_unique<Session>(c_);std::unique_ptr<Sftp> ftp;if(c_.protocol==RemoteProtocol::sftp)ftp=std::make_unique<Sftp>(*s);session_=std::move(s);sftp_=std::move(ftp);}}
    void writable(std::string_view path){validate_remote_path(path);if(c_.read_only||path=="/")fail(RemoteError::permission,"Remote path is read-only");}
    std::string shell_path(std::string_view path)const{return physical(c_.shell_root.empty()?c_.root:c_.shell_root,path);}
    std::string wrap(std::string_view command)const{
        if(c_.protocol==RemoteProtocol::sftp&&!c_.ssh_helper)fail(RemoteError::unsupported,"SSH helper is disabled");
        return std::string(c_.sudo?"sudo -n -- sh -c ":"sh -c ")+remote_shell_quote(command);
    }
    static RemoteInput span_input(std::span<const std::byte> bytes){return [bytes](std::uint64_t offset,std::span<std::byte> out){auto n=std::min(out.size(),bytes.size()-static_cast<std::size_t>(offset));std::copy_n(bytes.data()+static_cast<std::size_t>(offset),n,out.data());return n;};}
    static void finish(LIBSSH2_CHANNEL* channel){
        Session::check(libssh2_channel_send_eof(channel),"SSH EOF failed");
        Session::check(libssh2_channel_wait_eof(channel),"SSH remote EOF failed");
        Session::check(libssh2_channel_close(channel),"SSH channel close failed");
        if(libssh2_channel_get_exit_status(channel)!=0)fail(RemoteError::protocol,"Remote SSH command failed");
    }
    static void scp_ack(LIBSSH2_CHANNEL* channel){
        std::array<std::byte,1> byte{};read_all(channel,byte);
        if(byte[0]!=std::byte{})fail(RemoteError::protocol,"Remote SCP receiver rejected transfer");
    }
    std::string command(std::string_view script,std::size_t limit,std::uint64_t length=0,const RemoteInput& input={}){
        auto channel=session_->channel();auto cmd=wrap(script);
        Session::check(libssh2_channel_exec(channel.get(),cmd.c_str()),"Cannot execute SSH helper");
        std::array<std::byte,65536> bytes{};std::uint64_t offset=0;
        while(offset<length){auto n=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),length-offset));auto got=input(offset,std::span(bytes).first(n));if(!got||got>n)fail(RemoteError::protocol,"Short SSH upload");write_all(channel.get(),std::span(bytes).first(got));offset+=got;}
        Session::check(libssh2_channel_send_eof(channel.get()),"SSH EOF failed");
        std::string result;
        for(;;){auto n=libssh2_channel_read_ex(channel.get(),0,reinterpret_cast<char*>(bytes.data()),bytes.size());
            if(n<0)fail(n==LIBSSH2_ERROR_TIMEOUT?RemoteError::timeout:RemoteError::offline,"SSH command read failed");
            if(!n){if(libssh2_channel_eof(channel.get()))break;fail(RemoteError::offline,"SSH channel did not advance");}
            if(static_cast<std::size_t>(n)>limit-result.size())fail(RemoteError::protocol,"SSH response exceeds limit");
            result.append(reinterpret_cast<const char*>(bytes.data()),static_cast<std::size_t>(n));}
        Session::check(libssh2_channel_close(channel.get()),"SSH command close failed");
        if(libssh2_channel_get_exit_status(channel.get())!=0)fail(RemoteError::protocol,"Remote SSH command failed");
        return result;
    }
    bool byte_flags(){
        if(!byte_flags_){auto result=command("if dd if=/dev/zero of=/dev/null bs=65536 iflag=skip_bytes,count_bytes skip=1 count=1 oflag=seek_bytes seek=1 2>/dev/null; then printf yes; else printf no; fi",16);byte_flags_=result=="yes";}
        return *byte_flags_;
    }
    RemoteStat stat_locked(std::string_view path){
        if(sftp_)return sftp_->stat(physical(c_.root,path));
        auto p=remote_shell_quote(shell_path(path));
        auto result=command("if [ -L "+p+" ]; then printf special; elif [ ! -e "+p+" ]; then printf missing; else stat -c '%f %s %Y' -- "+p+"; fi",4096);
        if(result=="missing")fail(RemoteError::missing,"SSH path not found");
        if(result=="special")fail(RemoteError::unsupported,"SSH symbolic link is not projected");
        std::istringstream in(result);Attributes a;in>>std::hex>>a.mode>>std::dec>>a.size>>a.mtime;
        if(!in)fail(RemoteError::protocol,"Invalid SSH stat response");
        auto info=metadata(remote_name(path),a);if(!info)fail(RemoteError::unsupported,"Unsupported SSH object type");return *info;
    }
    void check_revision(std::string_view path,std::string_view revision){if(!revision.empty()&&stat_locked(path).revision!=revision)fail(RemoteError::conflict,"SSH file changed");}
};
}
std::shared_ptr<RemoteTransport> make_ssh_transport(RemoteConnection connection){validate_remote_connection(connection);return std::make_shared<SshTransport>(std::move(connection));}
std::string probe_ssh_host_key(const RemoteConnection& connection){Session session(connection,false);return session.host_key();}
}
