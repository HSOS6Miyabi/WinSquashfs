// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "archive_reader.hpp"
#include "process.hpp"
#include <filesystem>
#include <optional>

namespace archivefs {
enum class CompressionPreset : unsigned { store=1, fastest, fast, balanced, high, very_high, maximum, extreme };
struct CreationOptions {
    ArchiveFormat format=ArchiveFormat::squashfs;
    CompressionPreset preset=CompressionPreset::balanced;
    std::string codec="zstd";
    std::optional<int> level;
    std::uint32_t block_size=131072, workers=0;
    std::optional<std::uint32_t> memory_mib, timestamp;
    bool no_fragments=false, always_fragments=false, no_duplicates=false, no_sparse=false;
    bool no_xattrs=false, all_root=false, no_exports=false, no_data_compression=false;
    bool no_inode_compression=false, no_fragment_compression=false, keep_directory=false;
    bool lz4_high_compression=false, fragments=false, deduplicate=false, extreme=false;
    std::optional<int> gzip_window;
    std::string xz_dictionary, xz_filters, lzo_algorithm;
    std::optional<std::filesystem::path> exclude_file;
    static CreationOptions for_preset(ArchiveFormat format,CompressionPreset preset);
    std::vector<std::filesystem::path::string_type> arguments() const;
};
class ImageCreator {
public:
    ImageCreator(std::filesystem::path tools,std::filesystem::path decoders):tools_(std::move(tools)),decoders_(std::move(decoders)){}
    void create(const std::vector<std::filesystem::path>& sources,const std::filesystem::path& destination,
                CreationOptions options,LogSink log={},std::stop_token stop={}) const;
private:
    std::filesystem::path tools_,decoders_;
};
// Writes a POSIX pax-restricted TAR byte stream without staging file contents.
// A single directory contributes its children; multiple sources keep their names.
void write_tar(const std::vector<std::filesystem::path>& sources,
    const std::function<void(const void*,std::size_t)>& write,std::stop_token stop={});
}
