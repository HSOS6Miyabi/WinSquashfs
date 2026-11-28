// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "remote_transport.hpp"
namespace archivefs {
struct RemoteRecovery {
    std::string identity,path,revision,phase;
    std::uint64_t base_size=0,size=0;
};
std::string remote_recovery_identity(const RemoteConnection& connection);
RemoteRecovery read_remote_recovery(const std::filesystem::path& directory);
void write_remote_recovery(const std::filesystem::path& directory,const RemoteRecovery& record);
void sync_remote_cache_file(const std::filesystem::path& path);
}
