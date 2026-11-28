// SPDX-License-Identifier: GPL-3.0-only
#include "decoder_backend.hpp"
#ifdef ARCHIVEFS_WITH_DWARFS
#include "dwarfs_backend.hpp"
#endif
#include <cstring>
#include <limits>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace archivefs {
namespace {
class Module {
public:
    explicit Module(const std::filesystem::path& path) {
#ifdef _WIN32
        handle_ = LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
        handle_ = dlopen(std::filesystem::absolute(path).c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (!handle_) throw std::runtime_error("cannot load archive format library");
    }
    Module(Module const&) = delete;
    Module& operator=(Module const&) = delete;
    ~Module() {
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
    }
    template<class Fn> Fn optional_symbol(const char* name) const {
#ifdef _WIN32
        auto proc = GetProcAddress(handle_, name);
#else
        auto proc = dlsym(handle_, name);
#endif
        if (!proc) return nullptr;
        Fn function;
        static_assert(sizeof(function) == sizeof(proc));
        std::memcpy(&function, &proc, sizeof(function));
        return function;
    }
    template<class Fn> Fn symbol(const char* name) const {
        auto function=optional_symbol<Fn>(name);
        if(!function) throw std::runtime_error(std::string("missing decoder symbol: ")+name);
        return function;
    }
private:
#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
};

std::filesystem::path module_name(ArchiveFormat format) {
    const char* base = format == ArchiveFormat::squashfs ? "winsqfs-native" :
                       format == ArchiveFormat::dwarfs ? "winsqfs-dwarfs" : "winsqfs-erofs";
#ifdef _WIN32
    return std::string(base) + ".dll";
#else
    return std::string("lib") + base + ".so";
#endif
}

struct ReadScope;
thread_local ReadScope* current_read = nullptr;
struct ReadScope {
    explicit ReadScope(const RandomAccessSource& source)
        : source(source), previous(std::exchange(current_read, this)) {}
    ~ReadScope() { current_read = previous; }
    ReadScope(ReadScope const&) = delete;
    ReadScope& operator=(ReadScope const&) = delete;
    void check(int result) {
        if (failure) std::rethrow_exception(failure);
        if (result < 0) throw std::runtime_error("archive decoder returned error " + std::to_string(result));
        if (session) { session->finish(); session.reset(); }
    }
    void read(std::uint64_t offset, std::span<std::byte> output) {
        if (!session) session = source.begin_read();
        session->read_exact(offset, output);
    }
    const RandomAccessSource& source;
    ReadScope* previous;
    std::exception_ptr failure;
    std::unique_ptr<ReadSession> session;
};
using SourceRead = int(*)(std::uint64_t, void*, std::size_t);
int read_source(std::uint64_t offset, void* output, std::size_t size) noexcept {
    if (!current_read || (!output && size)) return -1;
    try {
        current_read->read(offset, std::span(static_cast<std::byte*>(output), size));
        return 0;
    } catch (...) {
        current_read->failure = std::current_exception();
        return -1;
    }
}

using Visit = int(*)(std::uint64_t, const char*, std::size_t, std::uint32_t, std::uint64_t, std::uint64_t);
FileKind mode_kind(std::uint32_t mode) noexcept {
    switch (mode & 0170000u) {
    case 0040000u: return FileKind::directory;
    case 0100000u: return FileKind::regular;
    case 0120000u: return FileKind::symlink;
    default: return FileKind::special;
    }
}
FileKind squash_kind(std::uint32_t mode) noexcept {
    switch (mode) {
    case 1: case 8: return FileKind::directory;
    case 2: case 9: return FileKind::regular;
    case 3: case 10: return FileKind::symlink;
    default: return FileKind::special;
    }
}
std::string checked_name(const char* name, std::size_t length) {
    if (!name || !length) throw std::runtime_error("empty archive entry name");
    std::string result(name, length);
    if (result == "." || result == ".." || result.find('/') != std::string::npos ||
        result.find('\0') != std::string::npos) throw std::runtime_error("invalid archive entry name");
    return result;
}
std::int64_t timestamp(std::uint64_t seconds) noexcept {
    return static_cast<std::int64_t>(std::min<std::uint64_t>(seconds, 253402300799ULL));
}
struct Listing {
    std::vector<DirectoryEntry> entries;
    std::exception_ptr error;
};
thread_local Listing* current_list = nullptr;
class ListScope {
public:
    explicit ListScope(Listing& listing) : previous_(std::exchange(current_list, &listing)) {}
    ~ListScope() { current_list = previous_; }
    ListScope(ListScope const&) = delete;
    ListScope& operator=(ListScope const&) = delete;
private:
    Listing* previous_;
};
int visit_entry(std::uint64_t inode, const char* name, std::size_t length, std::uint32_t mode,
                std::uint64_t size, std::uint64_t time) noexcept {
    if (!current_list) return -1;
    try {
        current_list->entries.push_back({checked_name(name, length), {inode, mode_kind(mode), size, timestamp(time)}});
        return 0;
    } catch (...) { current_list->error = std::current_exception(); return -1; }
}

struct SquashApi {
    using Open = int(*)(std::uint64_t, SourceRead, void**);
    using Close = void(*)(void*);
    using Root = const void*(*)(void*);
    using Node = const void*(*)(const void*);
    using Name = const char*(*)(const void*);
    using Node32 = std::uint32_t(*)(const void*);
    using Node64 = std::uint64_t(*)(const void*);
    using Info32 = std::uint32_t(*)(void*);
    using Info64 = std::uint64_t(*)(void*);
    using Read = int(*)(void*, const void*, std::uint64_t, void*, std::uint32_t);
    using CreateContext = void*(*)(void*);
    explicit SquashApi(const Module& module)
        : open(module.symbol<Open>("ws_open_source")), close(module.symbol<Close>("ws_close")),
          root(module.symbol<Root>("ws_root")), child(module.symbol<Node>("ws_child")), next(module.symbol<Node>("ws_next")),
          name(module.symbol<Name>("ws_name")), type(module.symbol<Node32>("ws_type")),
          mtime(module.symbol<Node32>("ws_mtime")), inode(module.symbol<Node32>("ws_inode")),
          size(module.symbol<Node64>("ws_size")), image_size(module.symbol<Info64>("ws_image_size")),
          block_size(module.symbol<Info32>("ws_block_size")), compression(module.symbol<Info32>("ws_compression")), read(module.symbol<Read>("ws_read")),
          create_context(module.optional_symbol<CreateContext>("ws_read_context_create")),
          destroy_context(module.optional_symbol<Close>("ws_read_context_destroy")) {}
    Open open; Close close; Root root; Node child, next; Name name; Node32 type, mtime, inode;
    Node64 size; Info64 image_size; Info32 block_size, compression; Read read;
    CreateContext create_context; Close destroy_context;
};
struct ExtraApi {
    using Open = int(*)(std::uint64_t, SourceRead, void**, std::uint64_t*, std::uint32_t*, std::uint64_t*);
    using Close = void(*)(void*);
    using List = int(*)(void*, std::uint64_t, Visit);
    using Read = int(*)(void*, std::uint64_t, std::uint64_t, void*, std::uint32_t);
    explicit ExtraApi(const Module& module) : open(module.symbol<Open>("wa_open")),
        close(module.symbol<Close>("wa_close")), list(module.symbol<List>("wa_list")), read(module.symbol<Read>("wa_read")) {}
    Open open; Close close; List list; Read read;
};

class Backend final : public ArchiveBackend {
public:
    Backend(std::shared_ptr<const RandomAccessSource> source, ArchiveFormat format,
            const std::filesystem::path& directory)
        : source_(std::move(source)), module_(directory / module_name(format)), info_{format, 0, source_->size(), 0} {
        ReadScope scope(*source_);
        try {
            if (format == ArchiveFormat::squashfs) {
                squash_ = std::make_unique<SquashApi>(module_);
                const auto result = squash_->open(source_->size(), read_source, &handle_);
                scope.check(result);
                if (result != 0 || !handle_) throw std::runtime_error("cannot open SquashFS image");
                auto root = squash_->root(handle_);
                if (!root) throw std::runtime_error("missing SquashFS root");
                auto root_info = squash_info(root);
                if (root_info.kind != FileKind::directory) throw std::runtime_error("invalid SquashFS root");
                info_.root = root_info.inode;
                info_.image_size = squash_->image_size(handle_);
                info_.block_size = squash_->block_size(handle_);
                info_.compression = squash_->compression(handle_);
                nodes_.emplace(root_info.inode, root);
                metadata_.emplace(root_info.inode, root_info);
                // Keep the original metadata handle and allocate private read
                // contexts on demand. Bound scratch-buffer usage independently
                // of the page cache.
                if(squash_->create_context&&squash_->destroy_context) {
                    const auto maximum=std::clamp(std::thread::hardware_concurrency(),2u,8u);
                    contexts_.reserve(maximum);
                    free_contexts_.reserve(maximum);
                    for(unsigned i=0;i<maximum;++i) {
                        auto ctx=squash_->create_context(handle_);
                        if(!ctx) break;
                        contexts_.push_back(ctx);free_contexts_.push_back(ctx);
                    }
                }
            } else {
                extra_ = std::make_unique<ExtraApi>(module_);
                std::uint64_t mtime = 0;
                const auto result = extra_->open(source_->size(), read_source, &handle_, &info_.root, &info_.block_size, &mtime);
                scope.check(result);
                if (result != 0 || !handle_) throw std::runtime_error("cannot open archive image");
                metadata_.emplace(info_.root, FileInfo{info_.root, FileKind::directory, 0, timestamp(mtime)});
            }
            source_->validate();
        } catch (...) { close(); throw; }
    }
    ~Backend() override { close(); }
    ArchiveInfo info() const override { return info_; }
    FileInfo stat(InodeId id) const override {
        std::shared_lock lock(mutex_);
        return known(id);
    }
    std::vector<DirectoryEntry> list(InodeId id) const override {
        std::lock_guard lock(mutex_);
        if (known(id).kind != FileKind::directory) throw std::invalid_argument("not a directory");
        auto cached = directories_.find(id);
        if (cached != directories_.end()) return cached->second;
        ReadScope scope(*source_);
        Listing listing;
        std::vector<std::pair<InodeId, const void*>> nodes;
        if (squash_) {
            std::unordered_set<const void*> seen;
            for (auto node = squash_->child(nodes_.at(id)); node; node = squash_->next(node)) {
                if (!seen.insert(node).second) throw std::runtime_error("directory entry cycle");
                auto name = squash_->name(node);
                if (!name) throw std::runtime_error("missing archive entry name");
                auto item = squash_info(node);
                listing.entries.push_back({checked_name(name, std::strlen(name)), item});
                nodes.emplace_back(item.inode, node);
            }
        } else {
            ListScope visit(listing);
            std::lock_guard decoder_lock(decoder_mutex_);
            const auto result = extra_->list(handle_, id, visit_entry);
            if (listing.error) std::rethrow_exception(listing.error);
            scope.check(result);
            if (result != 0) throw std::runtime_error("archive directory read failed");
        }
        source_->validate();
        std::unordered_set<std::string> names;
        std::unordered_set<InodeId> child_directories;
        std::unordered_map<InodeId, FileInfo> children_metadata;
        for (const auto& child : listing.entries) {
            if (!names.insert(child.name).second) throw std::runtime_error("duplicate directory entry");
            auto [local, inserted] = children_metadata.try_emplace(child.info.inode, child.info);
            if (!inserted && (local->second.kind != child.info.kind || local->second.size != child.info.size))
                throw std::runtime_error("inconsistent hardlink metadata");
            auto existing = metadata_.find(child.info.inode);
            if (child.info.kind == FileKind::directory &&
                (existing != metadata_.end() || !child_directories.insert(child.info.inode).second))
                throw std::runtime_error("invalid archive directory hierarchy");
            if (existing != metadata_.end() && (existing->second.kind != child.info.kind || existing->second.size != child.info.size))
                throw std::runtime_error("inconsistent hardlink metadata");
        }
        for (const auto& child : listing.entries) metadata_.try_emplace(child.info.inode, child.info);
        for (const auto& node : nodes) nodes_.try_emplace(node.first, node.second);
        return directories_.emplace(id, std::move(listing.entries)).first->second;
    }
    void read_exact(InodeId id, std::uint64_t offset, std::span<std::byte> output) const override {
        std::shared_lock lock(mutex_);
        auto file = known(id);
        if ((file.kind != FileKind::regular && file.kind != FileKind::symlink) || offset > file.size || output.size() > file.size - offset)
            throw std::out_of_range("invalid archive file read");
        auto node=squash_?nodes_.at(id):nullptr;
        lock.unlock();
        if(contexts_.empty()) {
            std::lock_guard decoder_lock(decoder_mutex_);
            read_data(handle_,node,id,offset,output);
        } else {
            void* context;
            {
                std::unique_lock context_lock(context_mutex_);
                context_cv_.wait(context_lock,[&]{return !free_contexts_.empty();});
                context=free_contexts_.back();free_contexts_.pop_back();
            }
            struct Lease {const Backend& owner;void* context;~Lease(){
                {std::lock_guard context_lock(owner.context_mutex_);owner.free_contexts_.push_back(context);}
                owner.context_cv_.notify_one();
            }} lease{*this,context};
            read_data(context,node,id,offset,output);
        }
    }
    void validate_source() const override { source_->validate(); }
    void validate_path() const override { source_->validate_path(); }
    bool source_changed() const noexcept override { return source_->source_changed(); }
private:
    void read_data(void* handle,const void* node,InodeId id,std::uint64_t offset,std::span<std::byte> output) const {
        ReadScope scope(*source_);
        while (!output.empty()) {
            const auto request = static_cast<std::uint32_t>(std::min<std::size_t>(output.size(), std::numeric_limits<int>::max()));
            const auto result = squash_ ? squash_->read(handle, node, offset, output.data(), request) :
                                          extra_->read(handle, id, offset, output.data(), request);
            scope.check(result);
            if (static_cast<std::uint32_t>(result) != request) throw std::runtime_error("short archive file read");
            offset += request;
            output = output.subspan(request);
        }
    }
    void close() noexcept {
        if(squash_&&squash_->destroy_context) for(auto context:contexts_) squash_->destroy_context(context);
        contexts_.clear();free_contexts_.clear();
        if (handle_) {
            ReadScope scope(*source_);
            if (squash_) squash_->close(handle_);
            else if (extra_) extra_->close(handle_);
            handle_ = nullptr;
        }
    }
    FileInfo squash_info(const void* node) const {
        return {squash_->inode(node), squash_kind(squash_->type(node)), squash_->size(node), squash_->mtime(node)};
    }
    FileInfo known(InodeId id) const {
        auto it = metadata_.find(id);
        if (it == metadata_.end()) throw std::out_of_range("unknown archive inode");
        return it->second;
    }
    std::shared_ptr<const RandomAccessSource> source_;
    Module module_;
    std::unique_ptr<SquashApi> squash_;
    std::unique_ptr<ExtraApi> extra_;
    void* handle_ = nullptr;
    ArchiveInfo info_;
    mutable std::shared_mutex mutex_;
    mutable std::mutex decoder_mutex_,context_mutex_;
    mutable std::condition_variable context_cv_;
    std::vector<void*> contexts_;
    mutable std::vector<void*> free_contexts_;
    mutable std::unordered_map<InodeId, const void*> nodes_;
    mutable std::unordered_map<InodeId, FileInfo> metadata_;
    mutable std::unordered_map<InodeId, std::vector<DirectoryEntry>> directories_;
};
} // namespace

std::shared_ptr<const ArchiveBackend> open_decoder_backend(
    std::shared_ptr<const RandomAccessSource> source, const std::filesystem::path& module_directory, std::size_t cache_bytes) {
    if (!source) throw std::invalid_argument("missing archive source");
    auto format = detect_format(*source);
    if (format == ArchiveFormat::unknown) throw UnknownArchiveFormat();
#ifdef ARCHIVEFS_WITH_DWARFS
    if(format==ArchiveFormat::dwarfs) return open_dwarfs_backend(std::move(source),cache_bytes);
#else
    (void)cache_bytes;
#endif
    return std::make_shared<Backend>(std::move(source), format, module_directory);
}
} // namespace archivefs
