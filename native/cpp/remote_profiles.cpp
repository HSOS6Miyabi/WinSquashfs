// SPDX-License-Identifier: GPL-3.0-only
#include "windows_services.hpp"
#include "windows_atomic_file.hpp"
#include "remote_control.hpp"
#include "remote_unc_path.hpp"
#include <wincrypt.h>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace archivefs {
namespace {
using Json=nlohmann::json;
std::string hex(std::span<const BYTE> data){
    constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(data.size()*2);
    for(auto c:data){out+=digits[c>>4];out+=digits[c&15];}return out;
}
std::vector<BYTE> unhex(std::string_view s){
    if(s.size()%2||s.size()>2u<<20)throw std::invalid_argument("Invalid encrypted credentials");
    auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::invalid_argument("Invalid encrypted credentials");};
    std::vector<BYTE> data(s.size()/2);for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<BYTE>((digit(s[2*i])<<4)|digit(s[2*i+1]));return data;
}
struct ProtectedBlob{DATA_BLOB data{};~ProtectedBlob(){if(data.pbData){SecureZeroMemory(data.pbData,data.cbData);LocalFree(data.pbData);}}};
std::string protect(const RemoteConnection& c){
    auto plain=Json{{"secret",c.secret},{"session_token",c.session_token}}.dump();
    struct Clear{std::string& s;~Clear(){if(!s.empty())SecureZeroMemory(s.data(),s.size());}} clear{plain};
    DATA_BLOB input{static_cast<DWORD>(plain.size()),reinterpret_cast<BYTE*>(plain.data())};
    ProtectedBlob output;
    if(!CryptProtectData(&input,L"WinArchiveFSPro remote connection",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output.data))
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Encrypt remote credentials");
    return hex(std::span(output.data.pbData,output.data.cbData));
}
void unprotect(RemoteConnection& c,std::string_view value){
    auto encrypted=unhex(value);DATA_BLOB input{static_cast<DWORD>(encrypted.size()),encrypted.data()};ProtectedBlob output;
    if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output.data))
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Decrypt remote credentials");
    auto p=Json::parse(output.data.pbData,output.data.pbData+output.data.cbData);
    c.secret=p.at("secret").get<std::string>();c.session_token=p.value("session_token",std::string{});
}
void validate_all(const std::vector<RemoteConnection>& connections){
    if(connections.size()>256)throw std::invalid_argument("Too many remote connections");
    std::set<std::u16string,WindowsNameLess> names;
    for(auto& c:connections){
        validate_remote_connection(c);auto name=to_utf16(c.name);
        if(encode_windows_name(c.name)!=name||!names.insert(to_utf16(std::string(remote_protocol_name(c.protocol)))+u"/"+name).second)
            throw std::invalid_argument("Invalid or duplicate remote connection name");
    }
}
}
std::vector<RemoteConnection> load_remote_connections(){
    auto file=settings_directory()/L"remote-connections.json";
    if(!std::filesystem::exists(file))return {};
    auto j=Json::parse(detail::read_text(file,8u<<20));
    if(!j.is_array()||j.size()>256)throw std::invalid_argument("Invalid remote connection store");
    std::vector<RemoteConnection> result;
    for(auto& row:j){
        RemoteConnection c;c.name=row.at("name").get<std::string>();c.protocol=parse_remote_protocol(row.at("protocol").get<std::string>());
        c.endpoint=row.at("endpoint").get<std::string>();c.root=row.value("root",c.root);c.username=row.value("username",c.username);
        c.region=row.value("region",c.region);c.bucket=row.value("bucket",c.bucket);c.host_key=row.value("host_key",c.host_key);c.private_key=row.value("private_key",c.private_key);c.shell_root=row.value("shell_root",c.shell_root);c.ca_file=row.value("ca_file",c.ca_file);
        c.read_only=row.value("read_only",c.read_only);c.ssh_helper=row.value("ssh_helper",c.ssh_helper);c.sudo=row.value("sudo",c.sudo);c.partial_put=row.value("partial_put",c.partial_put);c.implicit_tls=row.value("implicit_tls",c.implicit_tls);
        c.cache_mode=row.value("cache_mode",c.cache_mode);c.timeout_seconds=row.value("timeout_seconds",c.timeout_seconds);c.metadata_seconds=row.value("metadata_seconds",c.metadata_seconds);c.cache_mib=row.value("cache_mib",c.cache_mib);c.read_cache_mib=row.value("read_cache_mib",c.read_cache_mib);
        unprotect(c,row.at("credentials").get<std::string>());result.push_back(std::move(c));
    }
    validate_all(result);return result;
}
void save_remote_connections(const std::vector<RemoteConnection>& connections){
    validate_all(connections);auto j=Json::array();
    for(auto& c:connections)j.push_back({{"name",c.name},{"protocol",std::string(remote_protocol_name(c.protocol))},{"endpoint",c.endpoint},{"root",c.root},{"username",c.username},{"region",c.region},{"bucket",c.bucket},{"host_key",c.host_key},{"private_key",c.private_key},{"shell_root",c.shell_root},{"ca_file",c.ca_file},{"read_only",c.read_only},{"ssh_helper",c.ssh_helper},{"sudo",c.sudo},{"partial_put",c.partial_put},{"implicit_tls",c.implicit_tls},{"cache_mode",c.cache_mode},{"timeout_seconds",c.timeout_seconds},{"metadata_seconds",c.metadata_seconds},{"cache_mib",c.cache_mib},{"read_cache_mib",c.read_cache_mib},{"credentials",protect(c)}});
    detail::write_text_atomic(settings_directory()/L"remote-connections.json",j.dump(2)+"\n","Save remote connections");
}
namespace {
// Use names displayed in the mounted namespace, separated by slashes.
std::u16string projected_path(std::string_view connection,std::string_view raw){
    validate_remote_path(raw);
    auto path=u"\\"+to_utf16(connection);
    if(raw=="/")return path;
    raw.remove_prefix(1);
    while(!raw.empty()){
        auto slash=raw.find('/');auto part=raw.substr(0,slash);
        auto name=to_utf16(part);
        path+=u'\\';path+=name;
        if(slash==raw.npos)break;
        raw.remove_prefix(slash+1);
    }
    return path;
}
void control_remote(RemoteCommand command,std::string_view protocol,std::string_view connection,std::string_view from,std::string_view to={},bool replace=false){
    auto profiles=load_remote_connections();auto p=parse_remote_protocol(protocol);
    auto profile=std::find_if(profiles.begin(),profiles.end(),[&](const auto& c){return c.protocol==p&&!WindowsNameLess{}(to_utf16(c.name),to_utf16(connection))&&!WindowsNameLess{}(to_utf16(connection),to_utf16(c.name));});
    if(profile==profiles.end())throw RemoteFailure(RemoteError::missing,"Saved remote connection not found");
    if(profile->read_only)throw RemoteFailure(RemoteError::permission,"Remote connection is read-only");
    auto source=projected_path(profile->name,from);
    auto destination=command==RemoteCommand::copy?projected_path(profile->name,to):std::u16string{};
    auto request=encode_remote_control(command,destination,replace);
    start_agent();
    auto path=u"\\\\$sfs\\"+to_utf16(protocol)+source;
    detail::FileHandle handle(CreateFileW(std::filesystem::path(path).c_str(),GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(handle.value==INVALID_HANDLE_VALUE)detail::file_error(GetLastError(),"Open remote source");
    DWORD transferred=0;
    if(!DeviceIoControl(handle.value,remote_control_code,request.data(),static_cast<DWORD>(request.size()),
        nullptr,0,&transferred,nullptr))detail::file_error(GetLastError(),"Remote filesystem operation");
}
}
void copy_remote_file(std::string_view protocol,std::string_view connection,std::string_view from,std::string_view to,bool replace){
    control_remote(RemoteCommand::copy,protocol,connection,from,to,replace);
}
void recover_remote_file(std::string_view protocol,std::string_view connection,std::string_view path,bool discard){
    control_remote(discard?RemoteCommand::discard:RemoteCommand::retry,protocol,connection,path);
}
void paste_remote_files(const std::filesystem::path& directory,const std::vector<std::filesystem::path>& sources,std::stop_token stop){
    auto destination=parse_remote_unc(directory.wstring());
    if(!destination||sources.empty()||sources.size()>256)throw std::invalid_argument("Invalid remote copy selection");
    start_agent(stop);
    auto attributes=GetFileAttributesW(directory.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES)detail::file_error(GetLastError(),"Open remote destination directory");
    if(!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::invalid_argument("Remote destination is not a directory");
    struct Copy{std::string source,target;};
    std::vector<Copy> copies;
    auto utf8=[](std::wstring_view value){return to_utf8(std::u16string(value.begin(),value.end()));};
    std::vector<std::wstring> destinations;
    for(auto& path:sources){
        if(stop.stop_requested())throw std::runtime_error("Remote copy cancelled");
        auto source=parse_remote_unc(path.wstring());
        if(!source)throw std::invalid_argument("Source is not a remote file");
        auto target=server_paste_target(*destination,*source);
        if(target.empty())throw std::invalid_argument("Server paste requires different paths in the same remote connection");
        auto attrs=GetFileAttributesW(path.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES)detail::file_error(GetLastError(),"Open remote source file");
        if(attrs&FILE_ATTRIBUTE_DIRECTORY)throw RemoteFailure(RemoteError::unsupported,"Server paste accepts regular files");
        if(std::any_of(destinations.begin(),destinations.end(),[&](auto& value){return unc_equal(value,target);}))
            throw RemoteFailure(RemoteError::exists,"Duplicate server paste destination");
        auto target_path=directory/std::filesystem::path(source->leaf);
        auto target_attrs=GetFileAttributesW(target_path.c_str());
        if(target_attrs!=INVALID_FILE_ATTRIBUTES)throw RemoteFailure(RemoteError::exists,"Server paste destination already exists");
        auto error=GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)detail::file_error(error,"Check remote copy destination");
        destinations.push_back(target);copies.push_back({utf8(source->relative),utf8(target)});
    }
    for(auto& copy:copies){
        if(stop.stop_requested())throw std::runtime_error("Remote copy cancelled");
        copy_remote_file(utf8(destination->protocol),utf8(destination->connection),copy.source,copy.target);
    }
}
}
