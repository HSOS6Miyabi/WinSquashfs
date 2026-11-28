// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archivefs {
enum class DeviceError { unavailable, offline, permission, missing, timeout, protocol, unsupported };
class DeviceFailure:public std::runtime_error {
public:
    DeviceFailure(DeviceError code,std::string message):std::runtime_error(std::move(message)),code_(code){}
    DeviceError code()const noexcept{return code_;}
private:
    DeviceError code_;
};
struct RemoteDevice {std::string id,name,state;};
struct RemoteObject {
    std::string id,name;
    bool directory=false;
    std::uint64_t size=0;
    std::int64_t modified=0;
    std::string revision;
};
class DeviceProvider {
public:
    virtual ~DeviceProvider()=default;
    virtual std::vector<RemoteDevice> devices()=0;
    virtual std::string root_id()const=0;
    virtual std::vector<RemoteObject> children(std::string_view device,std::string_view parent)=0;
    virtual RemoteObject stat(std::string_view device,std::string_view object)=0;
    virtual void read_exact(std::string_view device,std::string_view object,std::uint64_t offset,std::span<std::byte> output)=0;
};
struct AdbOptions {
    bool root=false;
    std::uint16_t port=5037;
    std::chrono::milliseconds connect_timeout{2000},io_timeout{15000};
};
std::shared_ptr<DeviceProvider> make_adb_provider(AdbOptions options={});
#ifdef _WIN32
std::shared_ptr<DeviceProvider> make_mtp_provider();
int run_mtp_worker();
#endif
std::string adb_shell_quote(std::string_view value);
}
