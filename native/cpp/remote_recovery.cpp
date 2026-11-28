// SPDX-License-Identifier: GPL-3.0-only
#include "remote_recovery.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#include "windows_atomic_file.hpp"
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace archivefs {
namespace fs=std::filesystem;
namespace {
constexpr std::size_t limit=256u<<10;
void validate(const RemoteRecovery& r){
    validate_remote_path(r.path);
    if(r.path=="/"||r.identity.empty()||r.identity.size()>65536||r.revision.size()>65536||
       r.size>static_cast<std::uint64_t>(INT64_MAX)||r.base_size>static_cast<std::uint64_t>(INT64_MAX)||
       (r.phase!="ready"&&r.phase!="editing"&&r.phase!="uploading"&&r.phase!="committed"))
        throw std::invalid_argument("Invalid remote recovery record");
}
#ifndef _WIN32
void sync_path(const fs::path& path,int flags){
    auto fd=open(path.c_str(),flags|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)throw std::system_error(errno,std::generic_category(),"Open remote cache");
    struct Close{int fd;~Close(){close(fd);}} close_fd{fd};
    if(fsync(fd))throw std::system_error(errno,std::generic_category(),"Flush remote cache");
}
#endif
}
std::string remote_recovery_identity(const RemoteConnection& c){
    std::ostringstream out;
    out<<std::quoted(std::string(remote_protocol_name(c.protocol)))<<' '<<std::quoted(c.name)<<' '
       <<std::quoted(c.endpoint)<<' '<<std::quoted(c.root)<<' '<<std::quoted(c.username)<<' '
       <<std::quoted(c.bucket)<<' '<<std::quoted(c.region)<<' '<<std::quoted(c.host_key)<<' '
       <<std::quoted(c.shell_root)<<' '<<c.sudo<<' '<<c.implicit_tls;
    return out.str();
}
RemoteRecovery read_remote_recovery(const fs::path& directory){
    auto path=directory/"recovery.v2";
    if(fs::is_symlink(path)||!fs::is_regular_file(path)||fs::file_size(path)>limit)
        throw std::invalid_argument("Missing or invalid remote recovery record");
    std::ifstream in(path,std::ios::binary);std::string header;std::getline(in,header);
    RemoteRecovery r;
    if(header!="ArchiveFS remote cache 2"||
       !(in>>std::quoted(r.identity)>>std::quoted(r.path)>>std::quoted(r.revision)>>r.phase>>r.base_size>>r.size))
        throw std::invalid_argument("Invalid remote recovery record");
    in>>std::ws;if(!in.eof())throw std::invalid_argument("Unexpected remote recovery data");
    validate(r);return r;
}
void sync_remote_cache_file(const fs::path& path){
#ifdef _WIN32
    detail::FileHandle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)detail::file_error(GetLastError(),"Open remote write cache");
    if(!FlushFileBuffers(file.value))detail::file_error(GetLastError(),"Flush remote write cache");
#else
    sync_path(path,O_RDWR);
#endif
}
void write_remote_recovery(const fs::path& directory,const RemoteRecovery& r){
    validate(r);
    std::ostringstream out;
    out<<"ArchiveFS remote cache 2\n"<<std::quoted(r.identity)<<'\n'<<std::quoted(r.path)<<'\n'
       <<std::quoted(r.revision)<<'\n'<<r.phase<<' '<<r.base_size<<' '<<r.size<<'\n';
    auto data=out.str();
    if(data.size()>limit)throw std::length_error("Remote recovery record exceeds limit");
#ifdef _WIN32
    detail::write_text_atomic(directory/"recovery.v2",data,"Save remote recovery record");
#else
    auto temp=directory/"recovery.tmp";
    {
        std::ofstream file(temp,std::ios::binary|std::ios::trunc);
        file.write(data.data(),static_cast<std::streamsize>(data.size()));file.close();
        if(!file)throw RemoteFailure(RemoteError::no_space,"Cannot write recovery record");
    }
    sync_path(temp,O_RDWR);
    fs::rename(temp,directory/"recovery.v2");
    sync_path(directory,O_RDONLY|O_DIRECTORY);
    sync_path(directory.parent_path(),O_RDONLY|O_DIRECTORY);
#endif
}
}
