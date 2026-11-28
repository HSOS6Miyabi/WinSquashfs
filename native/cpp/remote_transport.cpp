// SPDX-License-Identifier: GPL-3.0-only
#include "remote_transport.hpp"
#include <algorithm>
#include <cctype>
#include <limits>

namespace archivefs {
namespace {
bool control(std::string_view s){return std::any_of(s.begin(),s.end(),[](unsigned char c){return c<32||c==127;});}
}
std::string_view remote_protocol_name(RemoteProtocol p)noexcept{
    switch(p){case RemoteProtocol::dav:return "dav";case RemoteProtocol::s3:return "s3";
    case RemoteProtocol::ftp:return "ftp";case RemoteProtocol::ftps:return "ftps";
    case RemoteProtocol::sftp:return "sftp";case RemoteProtocol::scp:return "scp";}
    return "";
}
RemoteProtocol parse_remote_protocol(std::string_view n){
    for(auto p:{RemoteProtocol::dav,RemoteProtocol::s3,RemoteProtocol::ftp,RemoteProtocol::ftps,RemoteProtocol::sftp,RemoteProtocol::scp})
        if(n==remote_protocol_name(p))return p;
    throw std::invalid_argument("Unknown remote filesystem protocol");
}
void validate_remote_path(std::string_view path){
    if(path.empty()||path.front()!='/'||path.size()>32768||path.find('\0')!=path.npos||control(path)||
       path.find('\\')!=path.npos)throw std::invalid_argument("Invalid remote path");
    if(path=="/")return;
    path.remove_prefix(1);
    while(!path.empty()){
        auto pos=path.find('/');
        auto part=path.substr(0,pos);
        if(part.empty()||part=="."||part=="..")throw std::invalid_argument("Invalid remote path component");
        if(pos==path.npos)break;
        path.remove_prefix(pos+1);
        if(path.empty())throw std::invalid_argument("Unexpected trailing slash");
    }
}
std::string remote_join(std::string_view parent,std::string_view component){
    validate_remote_path(parent);
    if(component.empty()||component.find('/')!=component.npos||component=="."||component=="..")
        throw std::invalid_argument("Invalid remote name");
    auto path=std::string(parent);if(path!="/")path+='/';path+=component;validate_remote_path(path);return path;
}
std::string remote_parent(std::string_view path){
    validate_remote_path(path);auto i=path.rfind('/');return i==0?"/":std::string(path.substr(0,i));
}
std::string remote_name(std::string_view path){
    validate_remote_path(path);return path=="/"?"/":std::string(path.substr(path.rfind('/')+1));
}
std::string remote_url_path(std::string_view path){
    constexpr char hex[]="0123456789ABCDEF";
    std::string out;out.reserve(path.size());
    for(unsigned char c:path){
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~'||c=='/')out+=static_cast<char>(c);
        else{out+='%';out+=hex[c>>4];out+=hex[c&15];}
    }
    return out;
}
std::string remote_shell_quote(std::string_view value){
    if(value.find('\0')!=value.npos)throw std::invalid_argument("NUL in SSH argument");
    std::string out="'";for(auto c:value){if(c=='\'')out+="'\\''";else out+=c;}out+='\'';return out;
}
void validate_remote_connection(const RemoteConnection& c){
    if(c.name.empty()||c.name.size()>120||c.name=="."||c.name==".."||control(c.name)||
       c.name.find_first_of("\\/:*?\"<>|")!=c.name.npos||c.name.back()=='.'||c.name.back()==' ')
        throw std::invalid_argument("Invalid connection name");
    if(c.endpoint.empty()||c.endpoint.size()>4096||control(c.endpoint)||c.endpoint.find_first_of("@?#")!=c.endpoint.npos)
        throw std::invalid_argument("Endpoint must not include credentials, query or fragment");
    validate_remote_path(c.root);
    if(!c.shell_root.empty())validate_remote_path(c.shell_root);
    if(c.timeout_seconds<1||c.timeout_seconds>3600||c.metadata_seconds>3600||c.cache_mib>32768||c.read_cache_mib>1024)
        throw std::invalid_argument("Invalid connection cache or timeout");
    if(c.cache_mode!="auto"&&c.cache_mode!="writeback"&&c.cache_mode!="direct")throw std::invalid_argument("Invalid remote cache mode");
    if(c.username.size()>4096||c.secret.size()>65536||c.session_token.size()>65536||
       control(c.username)||c.secret.find('\0')!=c.secret.npos||control(c.session_token))
        throw std::invalid_argument("Invalid remote credentials");
    const auto web=c.protocol==RemoteProtocol::dav||c.protocol==RemoteProtocol::s3;
    const auto ftp=c.protocol==RemoteProtocol::ftp||c.protocol==RemoteProtocol::ftps;
    if(web&&!(c.endpoint.starts_with("https://")||c.endpoint.starts_with("http://")))
        throw std::invalid_argument("Use an HTTP or HTTPS endpoint");
    if(ftp&&!(c.endpoint.starts_with("ftp://")||c.endpoint.starts_with("ftps://")))
        throw std::invalid_argument("Use an FTP or FTPS endpoint");
    if(c.protocol==RemoteProtocol::ftp&&!c.endpoint.starts_with("ftp://"))
        throw std::invalid_argument("FTP requires an ftp:// endpoint");
    if(c.protocol==RemoteProtocol::s3&&(c.bucket.empty()||c.bucket.size()>255||
        c.bucket.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-")!=c.bucket.npos||
        c.region.empty()||c.region.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-")!=c.region.npos))
        throw std::invalid_argument("Invalid S3 bucket or region");
    if(!web&&!ftp&&(c.endpoint.find_first_of("/\\")!=c.endpoint.npos||c.username.empty()||c.host_key.empty()))
        throw std::invalid_argument("SSH requires host[:port], username and a pinned host key");
    if(c.sudo&&c.protocol!=RemoteProtocol::scp)throw std::invalid_argument("sudo is available only for SCP");
}
std::shared_ptr<RemoteTransport> make_remote_transport(RemoteConnection connection){
    validate_remote_connection(connection);
    if(connection.protocol==RemoteProtocol::sftp||connection.protocol==RemoteProtocol::scp)
        return make_ssh_transport(std::move(connection));
    return make_http_ftp_transport(std::move(connection));
}
}
