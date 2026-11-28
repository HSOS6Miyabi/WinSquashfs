// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "archive_reader.hpp"
namespace archivefs {
std::shared_ptr<const ArchiveBackend> open_dwarfs_backend(std::shared_ptr<const RandomAccessSource> source);
}
