// SPDX-License-Identifier: GPL-3.0-only
#include "archive_reader.hpp"
#include <array>
#include <cstring>

namespace archivefs {
ArchiveFormat detect_format(const RandomAccessSource& source) {
    std::array<std::byte, 1028> header{};
    auto count = static_cast<std::size_t>(std::min<std::uint64_t>(header.size(), source.size()));
    source.read_exact(0, std::span(header).first(count));
    if (count >= 4 && std::memcmp(header.data(), "hsqs", 4) == 0) return ArchiveFormat::squashfs;
    if (count >= 6 && std::memcmp(header.data(), "DWARFS", 6) == 0) return ArchiveFormat::dwarfs;
    if (count >= 1028 && header[1024] == std::byte{0xe2} && header[1025] == std::byte{0xe1} &&
        header[1026] == std::byte{0xf5} && header[1027] == std::byte{0xe0}) return ArchiveFormat::erofs;
    return ArchiveFormat::unknown;
}
} // namespace archivefs
