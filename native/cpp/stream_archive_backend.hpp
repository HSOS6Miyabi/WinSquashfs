// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "archive_reader.hpp"

namespace archivefs {
std::shared_ptr<const ArchiveBackend> open_stream_archive_backend(
    std::shared_ptr<const RandomAccessSource> source, ArchiveFormat hint,std::size_t cache_bytes);
}
