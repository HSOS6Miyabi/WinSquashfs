// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <chrono>
#include <filesystem>
namespace archivefs {
inline auto file_time_to_system(std::filesystem::file_time_type value) {
#ifdef _MSC_VER
    return std::chrono::clock_cast<std::chrono::system_clock>(value);
#else
    return std::filesystem::file_time_type::clock::to_sys(value);
#endif
}
}
