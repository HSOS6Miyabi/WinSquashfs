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
#include <vector>

namespace archivefs {

enum class ArchiveFormat { squashfs, dwarfs, erofs, unknown };
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
    virtual void validate_source() const = 0;
    virtual void validate_path() const { validate_source(); }
};

ArchiveFormat detect_format(const RandomAccessSource& source);

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
        : backend_(std::move(backend)), cache_(budget), page_size_(page_size), caching_(budget != 0) {
        if (!backend_ || page_size_ == 0) throw std::invalid_argument("invalid archive reader configuration");
    }
    ArchiveInfo info() const { backend_->validate_source(); return backend_->info(); }
    void validate_path() const { backend_->validate_path(); }
    FileInfo stat(InodeId id) const { backend_->validate_source(); return backend_->stat(id); }
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
        if (!caching_) {
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
    static void cancelled(std::stop_token token) {
        if (token.stop_requested()) throw std::runtime_error("read cancelled");
    }
    std::shared_ptr<const ArchiveBackend> backend_;
    mutable Cache cache_;
    std::size_t page_size_;
    bool caching_;
};

} // namespace archivefs
