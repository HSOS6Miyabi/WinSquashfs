// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace archivefs {
using LogSink = std::function<void(std::string_view)>;
using ProcessInput = std::function<void(const std::function<void(const void*,std::size_t)>&)>;
// Execute a named binary directly without a command shell. Drain stdout and
// stderr concurrently and cancel the entire child process group on request.
void run_process(const std::filesystem::path& executable,
                 const std::vector<std::filesystem::path::string_type>& arguments,
                 LogSink log = {}, std::stop_token stop = {}, ProcessInput input = {});
}
