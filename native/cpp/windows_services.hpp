// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "winfsp_native.hpp"
#include <filesystem>
#include <stop_token>
namespace archivefs {
struct Settings {
    std::filesystem::path root;
    std::u16string alias=u"path";
    bool include_drives=true,start_at_login=false;
    unsigned idle_seconds=60,max_idle_images=8,cache_mib=64;
    std::string language="system",theme="system";
    static Settings load();
    void save() const;
    std::vector<Share> shares()const;
    std::u16string unc_path(const std::filesystem::path& source)const;
};
std::filesystem::path application_directory();
std::filesystem::path settings_directory();
std::vector<std::filesystem::path> recent_images();
void remember_image(const std::filesystem::path& path);
void clear_recent_images();
void open_in_explorer(std::u16string_view path);
void migrate_settings();
void start_agent(std::stop_token stop={});
void stop_agent(std::stop_token stop={});
bool agent_running();
int run_agent();
std::u16string register_unc_alias(const std::filesystem::path& source,std::stop_token stop={});
void open_unc(const std::filesystem::path& source,std::stop_token stop={});
class MountedVolume {
public:
    MountedVolume(const std::filesystem::path& source,const std::filesystem::path& decoders,std::u16string drive={});
    ~MountedVolume();
    MountedVolume(const MountedVolume&)=delete;
    MountedVolume& operator=(const MountedVolume&)=delete;
    std::u16string drive()const;
    const std::filesystem::path& source()const;
    bool mounted()const noexcept;
    void wait(std::stop_token stop={});
    void unmount();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
void eject_volume(std::u16string_view drive);
void serve_share(Share share,const std::filesystem::path& decoders,unsigned idle_seconds,std::stop_token stop);
}
