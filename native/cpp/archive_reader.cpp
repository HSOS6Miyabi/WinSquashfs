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
    if(count>=4&&(std::memcmp(header.data(),"PK\x03\x04",4)==0||
                  std::memcmp(header.data(),"PK\x05\x06",4)==0))return ArchiveFormat::zip;
    if(count>=6&&std::memcmp(header.data(),"7z\xbc\xaf\x27\x1c",6)==0)return ArchiveFormat::seven_zip;
    if(count>=7&&std::memcmp(header.data(),"Rar!\x1a\x07",6)==0)return ArchiveFormat::rar;
    if(count>=262&&std::memcmp(header.data()+257,"ustar",5)==0)return ArchiveFormat::tar;
    if(count>=6&&(std::memcmp(header.data(),"070701",6)==0||std::memcmp(header.data(),"070702",6)==0||
                  std::memcmp(header.data(),"070707",6)==0))return ArchiveFormat::cpio;
    if(count>=4&&std::memcmp(header.data(),"MSCF",4)==0)return ArchiveFormat::cab;
    if(count>=8&&std::memcmp(header.data(),"!<arch>\n",8)==0)return ArchiveFormat::ar;
    return ArchiveFormat::unknown;
}
} // namespace archivefs
