// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "archive_reader.hpp"
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace archivefs {
class UnknownArchiveFormat final:public std::runtime_error {public:UnknownArchiveFormat():std::runtime_error("unsupported archive format"){} };

// Load a format decoder from the application directory. The format ABI is
// synchronous: read callbacks are invoked on the calling thread.
std::shared_ptr<const ArchiveBackend> open_decoder_backend(
    std::shared_ptr<const RandomAccessSource> source,
    const std::filesystem::path& module_directory, std::size_t cache_bytes=64u<<20);

} // namespace archivefs
