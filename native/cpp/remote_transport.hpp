// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace archivefs {
enum class RemoteProtocol { dav, s3, ftp, ftps, sftp, scp };
enum class RemoteError { missing, exists, permission, conflict, unsupported, timeout, offline, protocol, not_empty, no_space };
class RemoteFailure final:public std::runtime_error {
public:
    RemoteFailure(RemoteError code,std::string message):std::runtime_error(std::move(message)),code_(code){}
    RemoteError code()const noexcept{return code_;}
private:RemoteError code_;
};
struct RemoteConnection {
    std::string name;
    RemoteProtocol protocol=RemoteProtocol::dav;
    std::string endpoint,root="/",username,secret,session_token;
    std::string region="us-east-1",bucket,host_key,private_key;
    std::string shell_root;
    bool read_only=false,ssh_helper=true,sudo=false,partial_put=false;
    bool implicit_tls=false;
    unsigned timeout_seconds=60,metadata_seconds=5,cache_mib=512;
};
struct RemoteStat {
    std::string name;
    bool directory=false;
    std::uint64_t size=0;
    std::int64_t modified=0;
    std::string revision;
};
using RemoteInput=std::function<std::size_t(std::uint64_t,std::span<std::byte>)>;
using RemoteOutput=std::function<void(std::span<const std::byte>)>;
class RemoteTransport {
public:
    virtual ~RemoteTransport()=default;
    virtual RemoteStat stat(std::string_view path)=0;
    virtual std::vector<RemoteStat> list(std::string_view path)=0;
    virtual void read(std::string_view path,std::uint64_t offset,std::span<std::byte> output,std::string_view revision={})=0;
    // Return false only when no mutation was attempted or the server
    // definitively rejected an unsupported method before applying it.
    virtual bool write_at(std::string_view,std::uint64_t,std::span<const std::byte>,std::string_view){return false;}
    virtual bool resize(std::string_view,std::uint64_t){return false;}
    virtual void replace(std::string_view path,std::uint64_t size,const RemoteInput& input,
                         std::string_view revision={},bool exclusive=false)=0;
    virtual void mkdir(std::string_view path)=0;
    virtual void remove(std::string_view path,bool directory)=0;
    virtual void rename(std::string_view from,std::string_view to,bool replace)=0;
    virtual bool copy(std::string_view,std::string_view,bool){return false;}
};
std::string_view remote_protocol_name(RemoteProtocol protocol)noexcept;
RemoteProtocol parse_remote_protocol(std::string_view name);
void validate_remote_connection(const RemoteConnection& connection);
std::string remote_join(std::string_view parent,std::string_view component);
std::string remote_parent(std::string_view path);
std::string remote_name(std::string_view path);
void validate_remote_path(std::string_view path);
std::string remote_url_path(std::string_view path);
std::string remote_shell_quote(std::string_view value);
std::shared_ptr<RemoteTransport> make_remote_transport(RemoteConnection connection);
std::shared_ptr<RemoteTransport> make_http_ftp_transport(RemoteConnection connection);
}
