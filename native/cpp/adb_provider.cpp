// SPDX-License-Identifier: GPL-3.0-only
#include "device_provider.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace archivefs {
namespace {
using Clock=std::chrono::steady_clock;
[[noreturn]] void protocol(std::string text){throw DeviceFailure(DeviceError::protocol,std::move(text));}
#ifdef _WIN32
using Socket=SOCKET;
constexpr Socket invalid=INVALID_SOCKET;
int socket_error(){return WSAGetLastError();}
bool pending(int e){return e==WSAEWOULDBLOCK||e==WSAEINPROGRESS||e==WSAEINTR;}
void close_socket(Socket s){closesocket(s);}
void init_socket(){struct Runtime{Runtime(){WSADATA d{};if(WSAStartup(MAKEWORD(2,2),&d))throw DeviceFailure(DeviceError::unavailable,"Winsock initialization failed");}~Runtime(){WSACleanup();}};static Runtime runtime;}
#else
using Socket=int;
constexpr Socket invalid=-1;
int socket_error(){return errno;}
bool pending(int e){return e==EAGAIN||e==EWOULDBLOCK||e==EINPROGRESS||e==EINTR;}
void close_socket(Socket s){::close(s);}
void init_socket(){}
#endif
class Connection {
public:
    explicit Connection(const AdbOptions& o){
        init_socket();
#ifdef _WIN32
        socket_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
#else
        socket_=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP);
#endif
        if(socket_==invalid)throw DeviceFailure(DeviceError::unavailable,"Cannot create ADB socket");
        try{
#ifdef _WIN32
            u_long nonblocking=1;if(ioctlsocket(socket_,FIONBIO,&nonblocking))protocol("Cannot configure ADB socket");
#else
            int flags=fcntl(socket_,F_GETFL,0);if(flags<0||fcntl(socket_,F_SETFL,flags|O_NONBLOCK)<0)protocol("Cannot configure ADB socket");
#endif
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(o.port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            auto end=Clock::now()+o.connect_timeout;
            if(::connect(socket_,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){
                if(!pending(socket_error()))unavailable(o.port);
                wait(true,end);
                int error=0;
#ifdef _WIN32
                int length=sizeof(error);auto status=getsockopt(socket_,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length);
#else
                socklen_t length=sizeof(error);auto status=getsockopt(socket_,SOL_SOCKET,SO_ERROR,&error,&length);
#endif
                if(status||error)unavailable(o.port);
            }
            deadline_=Clock::now()+o.io_timeout;
        }catch(...){close_socket(socket_);socket_=invalid;throw;}
    }
    ~Connection(){if(socket_!=invalid)close_socket(socket_);}
    Connection(Connection const&)=delete;
    void read(std::span<std::byte> data){transfer(data.data(),data.size(),false);}
    void write(std::string_view data){transfer(const_cast<char*>(data.data()),data.size(),true);}
    std::string read_string(std::size_t length){std::string s(length,'\0');read(std::as_writable_bytes(std::span(s)));return s;}
    std::string message(){auto hex=read_string(4);unsigned length=0;auto r=std::from_chars(hex.data(),hex.data()+4,length,16);if(r.ec!=std::errc{}||r.ptr!=hex.data()+4)protocol("Invalid ADB message length");return read_string(length);}
    void service(std::string_view name){
        if(name.empty()||name.size()>65535||name.find('\0')!=name.npos)protocol("Invalid ADB service");
        constexpr char digits[]="0123456789ABCDEF";std::array<char,4> length{};auto n=name.size();
        for(int i=3;i>=0;--i){length[static_cast<std::size_t>(i)]=digits[n&15];n>>=4;}
        write(std::string_view(length.data(),length.size()));write(name);
        auto status=read_string(4);
        if(status=="FAIL"){
            auto error=message();auto code=DeviceError::protocol;
            if(error.find("unauthorized")!=error.npos||error.find("permission")!=error.npos)code=DeviceError::permission;
            else if(error.find("offline")!=error.npos||error.find("not found")!=error.npos||error.find("no devices")!=error.npos)code=DeviceError::offline;
            throw DeviceFailure(code,"ADB: "+error);
        }
        if(status!="OKAY")protocol("Invalid ADB service response");
    }
private:
    Socket socket_=invalid;
    Clock::time_point deadline_;
    [[noreturn]] static void unavailable(unsigned port){throw DeviceFailure(DeviceError::unavailable,"ADB Server is not listening on 127.0.0.1:"+std::to_string(port));}
    void wait(bool writing,Clock::time_point end){
        for(;;){
            auto left=std::chrono::duration_cast<std::chrono::milliseconds>(end-Clock::now());
            if(left.count()<=0)throw DeviceFailure(DeviceError::timeout,"ADB connection or request timed out");
#ifdef _WIN32
            fd_set set,errors;FD_ZERO(&set);FD_ZERO(&errors);FD_SET(socket_,&set);FD_SET(socket_,&errors);
            timeval tv{static_cast<long>(left.count()/1000),static_cast<long>((left.count()%1000)*1000)};
            int ready=select(0,writing?nullptr:&set,writing?&set:nullptr,&errors,&tv);
#else
            pollfd descriptor{socket_,static_cast<short>(writing?POLLOUT:POLLIN),0};
            int ready=poll(&descriptor,1,static_cast<int>(std::min<std::int64_t>(left.count(),std::numeric_limits<int>::max())));
#endif
            if(ready>0)return;
            if(!ready)throw DeviceFailure(DeviceError::timeout,"ADB connection or request timed out");
            if(!pending(socket_error()))throw DeviceFailure(DeviceError::offline,"ADB socket failed");
        }
    }
    void transfer(void* raw,std::size_t size,bool writing){
        auto data=static_cast<char*>(raw);
        while(size){
            wait(writing,deadline_);
            auto count=static_cast<int>(std::min<std::size_t>(size,1u<<20));
#ifdef _WIN32
            auto n=writing?send(socket_,data,count,0):recv(socket_,data,count,0);
#else
            auto n=writing?send(socket_,data,count,MSG_NOSIGNAL):recv(socket_,data,count,0);
#endif
            if(n<0&&pending(socket_error()))continue;
            if(n<=0)throw DeviceFailure(DeviceError::offline,"ADB connection closed unexpectedly");
            data+=n;size-=static_cast<std::size_t>(n);
        }
    }
};
std::uint32_t little32(std::span<const std::byte> s){
    std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::to_integer<std::uint32_t>(s[i])<<(8*i);return n;
}
std::string path_join(std::string_view base,std::string_view child){
    if(child.empty()||child=="."||child==".."||child.find('/')!=child.npos||child.find('\0')!=child.npos)protocol("Invalid device directory entry");
    auto path=std::string(base);if(path.empty()||path.back()!='/')path+='/';path+=child;return path;
}
void check_path(std::string_view path){
    if(path.empty()||path.front()!='/'||path.find('\0')!=path.npos)throw std::invalid_argument("Invalid Android path");
}
std::string name_of(std::string_view path){if(path=="/")return "/";return std::string(path.substr(path.find_last_of('/')+1));}
template<class T>T integer(std::string_view text,int radix=10){T n{};auto r=std::from_chars(text.data(),text.data()+text.size(),n,radix);if(r.ec!=std::errc{}||r.ptr!=text.data()+text.size())protocol("Invalid Android file metadata");return n;}
std::vector<std::string_view> fields(std::string_view line,char sep){
    std::vector<std::string_view> out;
    while(true){auto pos=line.find(sep);out.push_back(line.substr(0,pos));if(pos==line.npos)break;line.remove_prefix(pos+1);}
    return out;
}
RemoteObject parse_stat(std::string_view id,std::string_view name,std::string_view metadata){
    while(!metadata.empty()&&(metadata.back()=='\r'||metadata.back()=='\n'))metadata.remove_suffix(1);
    auto parts=fields(metadata,' ');
    if(parts.size()!=5)protocol("Invalid Android stat response");
    auto mode=integer<unsigned>(parts[0],16)&0170000u;
    if(mode!=0040000u&&mode!=0100000u)throw DeviceFailure(DeviceError::unsupported,"Android object is not a regular file or directory");
    return {std::string(id),std::string(name),mode==0040000u,mode==0040000u?0:integer<std::uint64_t>(parts[1]),integer<std::int64_t>(parts[2]),std::string(parts[3])+" "+std::string(parts[4])};
}
class Adb final:public DeviceProvider {
public:
    explicit Adb(AdbOptions options):options_(options){if(!options.port||options.connect_timeout.count()<=0||options.io_timeout.count()<=0)throw std::invalid_argument("Invalid ADB endpoint or timeout");}
    std::string root_id()const override{return "/";}
    std::vector<RemoteDevice> devices()override{
        Connection c(options_);c.service("host:devices-l");auto data=c.message();
        std::vector<RemoteDevice> result;
        for(auto line:fields(data,'\n')){
            if(line.empty())continue;
            auto split=line.find('\t');if(split==line.npos)protocol("Invalid ADB device list");
            auto id=line.substr(0,split);auto rest=line.substr(split+1);auto end=rest.find_first_of(" \r");auto state=rest.substr(0,end);
            if(id.empty()||state.empty()||id.find_first_of("\r\n")!=id.npos||id.find('\0')!=id.npos)protocol("Invalid ADB device record");
            std::string name(id);
            auto model=rest.find(" model:");if(model!=rest.npos){auto value=rest.substr(model+7);auto stop=value.find_first_of(" \r");value=value.substr(0,stop);if(!value.empty())name=std::string(value)+" ("+std::string(id)+")";}
            result.push_back({std::string(id),std::move(name),std::string(state)});
        }
        return result;
    }
    RemoteObject stat(std::string_view device,std::string_view object)override{
        check_path(object);auto cmd="stat -L -c '%f %s %Y %d %i' "+adb_shell_quote(object);
        return parse_stat(object,name_of(object),shell(device,cmd,4096));
    }
    std::vector<RemoteObject> children(std::string_view device,std::string_view parent)override{
        check_path(parent);
        // NUL-delimited names and numeric metadata preserve all filename bytes.
        const auto command="p="+adb_shell_quote(parent)+"; [ -d \"$p\" ] && [ -r \"$p\" ] && [ -x \"$p\" ] || exit 77; "
            "for f in \"$p\"/* \"$p\"/.[!.]* \"$p\"/..?*; do "
            "[ -e \"$f\" ] || continue; "
            "m=$(stat -L -c '%f %s %Y %d %i' \"$f\" 2>/dev/null) || continue; "
            "printf '%s\\000%s\\000' \"$" "{f##*/}\" \"$m\"; done";
        auto data=shell(device,command,32u<<20);
        auto parts=fields(data,'\0');if(parts.empty()||!parts.back().empty()||parts.size()%2!=1)protocol("Incomplete Android directory record");
        std::vector<RemoteObject> result;result.reserve(parts.size()/2);
        for(std::size_t i=0;i+1<parts.size();i+=2){
            auto name=parts[i];try{result.push_back(parse_stat(path_join(parent,name),name,parts[i+1]));}
            catch(DeviceFailure const& e){if(e.code()!=DeviceError::unsupported)throw;}
        }
        return result;
    }
    void read_exact(std::string_view device,std::string_view object,std::uint64_t offset,std::span<std::byte> output)override{
        check_path(object);
        while(!output.empty()){
            auto size=std::min<std::size_t>(output.size(),8u<<20);
            if(offset>std::numeric_limits<std::uint64_t>::max()-size)throw std::out_of_range("Android read offset");
            auto cmd="dd if="+adb_shell_quote(object)+" bs=262144 iflag=skip_bytes,count_bytes skip="+std::to_string(offset)+" count="+std::to_string(size)+" status=none";
            auto data=shell(device,cmd,size);
            if(data.size()!=size)throw DeviceFailure(DeviceError::offline,"Android file ended before the requested byte range");
            std::memcpy(output.data(),data.data(),size);offset+=size;output=output.subspan(size);
        }
    }
private:
    AdbOptions options_;
    std::string shell(std::string_view device,std::string command,std::size_t maximum){
        if(device.empty()||device.find_first_of("\r\n")!=device.npos||device.find('\0')!=device.npos)throw std::invalid_argument("Invalid ADB serial");
        if(options_.root)command="su -c "+adb_shell_quote("[ \"$(id -u)\" = 0 ] || exit 77; "+command);
        Connection c(options_);c.service("host:transport:"+std::string(device));c.service("shell,v2,raw:"+command);
        c.write(std::string_view("\x04\0\0\0\0",5));
        std::string out,error;
        for(;;){
            std::array<std::byte,5> header{};c.read(header);auto id=std::to_integer<unsigned>(header[0]);auto length=little32(std::span(header).subspan(1));
            if(length>(16u<<20))protocol("ADB shell packet too large");
            if(id==1){if(length>maximum-out.size())protocol("ADB output exceeds expected size");out+=c.read_string(length);}
            else if(id==2){auto value=c.read_string(length);if(error.size()<4096)error+=value.substr(0,4096-error.size());}
            else if(id==3){
                if(length!=1)protocol("Invalid ADB exit status");
                auto code=static_cast<unsigned char>(c.read_string(1)[0]);
                if(code){auto kind=code==77||options_.root||error.find("Permission denied")!=error.npos?DeviceError::permission:error.find("No such file")!=error.npos?DeviceError::missing:DeviceError::protocol;throw DeviceFailure(kind,options_.root?"Android Root operation failed: "+error:"Android operation failed: "+error);}
                return out;
            }else protocol("Unexpected ADB shell-v2 packet");
        }
    }
};
}
std::string adb_shell_quote(std::string_view value){
    if(value.find('\0')!=value.npos)throw std::invalid_argument("NUL in shell argument");
    std::string out="'";for(char c:value){if(c=='\'')out+="'\\''";else out+=c;}out+='\'';
    return out;
}
std::shared_ptr<DeviceProvider> make_adb_provider(AdbOptions options){return std::make_shared<Adb>(options);}
}
