// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "concurrent_cache.hpp"
#include "random_access_source.hpp"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace archivefs {

enum class ArchiveFormat { squashfs, dwarfs, erofs, unknown, zip, seven_zip, rar, tar, cpio, cab, iso9660, ar, lha, compressed_stream };
inline std::string_view archive_format_name(ArchiveFormat format) noexcept {
    switch(format){
    case ArchiveFormat::squashfs:return "SquashFS";
    case ArchiveFormat::dwarfs:return "DwarFS";
    case ArchiveFormat::erofs:return "EROFS";
    case ArchiveFormat::zip:return "ZIP";
    case ArchiveFormat::seven_zip:return "7z";
    case ArchiveFormat::rar:return "RAR";
    case ArchiveFormat::tar:return "TAR";
    case ArchiveFormat::cpio:return "CPIO";
    case ArchiveFormat::cab:return "CAB";
    case ArchiveFormat::iso9660:return "ISO9660";
    case ArchiveFormat::ar:return "AR";
    case ArchiveFormat::lha:return "LHA";
    case ArchiveFormat::compressed_stream:return "Compressed stream";
    default:return "Unknown";
    }
}
inline bool filesystem_image_format(ArchiveFormat f) noexcept {
    return f==ArchiveFormat::squashfs||f==ArchiveFormat::dwarfs||f==ArchiveFormat::erofs;
}
using InodeId = std::uint64_t;
enum class FileKind { regular, directory, symlink, special };
struct FileInfo {
    InodeId inode;
    FileKind kind;
    std::uint64_t size;
    std::int64_t modified;
};
struct DirectoryEntry { std::string name; FileInfo info; };
struct ArchiveInfo { ArchiveFormat format; InodeId root; std::uint64_t image_size; std::uint32_t block_size; std::uint32_t compression = 0; };

// A format adapter owns its decoder state. Concurrent read requests must be safe.
// A successful read_exact must fill the entire requested output.
class ArchiveBackend {
public:
    virtual ~ArchiveBackend() = default;
    virtual ArchiveInfo info() const = 0;
    virtual FileInfo stat(InodeId inode) const = 0;
    virtual std::vector<DirectoryEntry> list(InodeId directory) const = 0;
    virtual void read_exact(InodeId inode, std::uint64_t offset, std::span<std::byte> output) const = 0;
    virtual std::string read_link(InodeId inode) const {
        auto file=stat(inode);
        if(file.kind!=FileKind::symlink||!file.size||file.size>65536)throw std::invalid_argument("invalid symbolic link");
        std::string target(static_cast<std::size_t>(file.size),'\0');
        read_exact(inode,0,std::as_writable_bytes(std::span(target)));
        return target;
    }
    virtual void validate_source() const = 0;
    virtual void validate_path() const { validate_source(); }
    virtual bool source_changed() const noexcept { return false; }
    virtual bool uses_native_cache() const noexcept { return false; }
    virtual std::size_t cache_reservation_bytes() const noexcept { return 0; }
};

ArchiveFormat detect_format(const RandomAccessSource& source);
inline std::size_t recommended_page_size(const ArchiveInfo& info) noexcept {
    if(info.format==ArchiveFormat::erofs)return std::clamp<std::size_t>(info.block_size,64u<<10,1u<<20);
    return info.block_size?std::clamp<std::size_t>(info.block_size,4096,1u<<20):256u<<10;
}

struct PageKey {
    InodeId inode;
    std::uint64_t page;
    bool operator==(PageKey const&) const = default;
};
struct PageHash {
    std::size_t operator()(const PageKey& key) const noexcept {
        auto seed = std::hash<InodeId>{}(key.inode);
        return seed ^ (std::hash<std::uint64_t>{}(key.page) + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) +
                       (seed << 6) + (seed >> 2));
    }
};

class ArchiveReader final {
public:
    using Cache = ConcurrentCache<PageKey, std::byte, PageHash>;
    explicit ArchiveReader(std::shared_ptr<const ArchiveBackend> backend,
                           std::size_t budget = 64u << 20, std::size_t page_size = 256u << 10)
        : backend_(std::move(backend)), cache_(page_budget(backend_,budget)), page_size_(page_size), caching_(page_budget(backend_,budget)!=0) {
        if (!backend_ || page_size_ == 0) throw std::invalid_argument("invalid archive reader configuration");
    }
    ArchiveInfo info() const { backend_->validate_source(); return backend_->info(); }
    void validate_path() const { backend_->validate_path(); }
    bool source_changed() const noexcept { return backend_->source_changed(); }
    FileInfo stat(InodeId id) const { backend_->validate_source(); return backend_->stat(id); }
    std::string read_link(InodeId id) const {
        backend_->validate_source();
        if(backend_->stat(id).kind!=FileKind::symlink)throw std::invalid_argument("not a symbolic link");
        auto target=backend_->read_link(id);
        backend_->validate_source();
        if(target.empty()||target.size()>65536||target.find('\0')!=std::string::npos)
            throw std::runtime_error("invalid symbolic link target");
        return target;
    }
    std::vector<DirectoryEntry> list(InodeId id) const {
        backend_->validate_source();
        if (backend_->stat(id).kind != FileKind::directory) throw std::invalid_argument("not a directory");
        auto entries = backend_->list(id);
        backend_->validate_source();
        return entries;
    }
    std::size_t read_at(InodeId id, std::uint64_t offset, std::span<std::byte> output,
                        std::stop_token stop = {}) const {
        cancelled(stop);
        backend_->validate_source();
        const auto file = backend_->stat(id);
        if (file.kind != FileKind::regular) throw std::invalid_argument("not a regular file");
        if (offset >= file.size || output.empty()) return 0;
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), file.size - offset));
        // Large contiguous reads should be decompressed directly into the
        // caller's buffer. Avoid re-reading compressed extents for each
        // cache page or evicting hot random-read pages with sequential data.
        const bool streaming=length>=(256u<<10)&&length/page_size_>=2;
        if (!caching_||streaming) {
            backend_->read_exact(id, offset, output.first(length));
            backend_->validate_source();
            cancelled(stop);
            return length;
        }
        auto remaining = output.first(length);
        while (!remaining.empty()) {
            cancelled(stop);
            const auto page = offset / page_size_;
            const auto start = page * page_size_;
            const auto within = static_cast<std::size_t>(offset - start);
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(page_size_, file.size - start));
            const auto take = std::min(remaining.size(), count - within);
            auto value = cache_.get(PageKey{id, page}, [&] {
                Cache::Buffer bytes(count);
                backend_->read_exact(id, start, bytes);
                backend_->validate_source();
                return bytes;
            });
            std::copy_n(value->data() + within, take, remaining.data());
            remaining = remaining.subspan(take);
            offset += take;
        }
        backend_->validate_source();
        cancelled(stop);
        return length;
    }
    Cache::Statistics cache_statistics() const { return cache_.statistics(); }
private:
    static std::size_t page_budget(const std::shared_ptr<const ArchiveBackend>& backend,std::size_t budget)noexcept{
        if(!backend||backend->uses_native_cache())return 0;
        return budget-std::min(budget,backend->cache_reservation_bytes());
    }
    static void cancelled(std::stop_token token) {
        if (token.stop_requested()) throw std::runtime_error("read cancelled");
    }
    std::shared_ptr<const ArchiveBackend> backend_;
    mutable Cache cache_;
    std::size_t page_size_;
    bool caching_;
};

} // namespace archivefs
