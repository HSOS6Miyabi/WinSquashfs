// SPDX-License-Identifier: GPL-3.0-only
#include "random_access_source.hpp"
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <functional>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace archivefs {
namespace {
#ifdef _WIN32
using RawHandle = HANDLE;
const RawHandle invalid_handle = INVALID_HANDLE_VALUE;
[[noreturn]] void fail(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}
void close_handle(RawHandle h) noexcept { if(h) CloseHandle(h); }
#else
using RawHandle = int;
constexpr RawHandle invalid_handle = -1;
[[noreturn]] void fail(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
void close_handle(RawHandle h) noexcept { ::close(h); }
#endif

class Handle {
public:
    explicit Handle(RawHandle h = invalid_handle) : value_(h) {}
    Handle(Handle const&) = delete;
    Handle& operator=(Handle const&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, invalid_handle)) {}
    ~Handle() { close(); }
    void close() noexcept { auto previous=std::exchange(value_,invalid_handle);if(previous!=invalid_handle)close_handle(previous); }
    RawHandle get() const noexcept { return value_; }
private:
    RawHandle value_;
};

struct Stamp {
    std::uint64_t size = 0;
    std::uint64_t volume = 0, id_low = 0, id_high = 0;
    std::uint64_t modified = 0, changed = 0;
    bool identity_available = false;
    bool extended_identity = false;
    bool matches(const Stamp& other) const noexcept {
#ifndef _WIN32
        return size==other.size&&volume==other.volume&&id_low==other.id_low&&modified==other.modified&&changed==other.changed;
#else
        return size==other.size && (!identity_available||!other.identity_available||extended_identity!=other.extended_identity||
            (volume==other.volume&&id_low==other.id_low&&id_high==other.id_high)) &&
            (!modified||!other.modified||modified==other.modified) &&
            (!changed||!other.changed||changed==other.changed);
#endif
    }
};

Handle open_handle(const std::filesystem::path& path, SourcePolicy policy) {
#ifdef _WIN32
    const auto share = policy == SourcePolicy::protected_handle ? FILE_SHARE_READ :
        FILE_SHARE_READ | FILE_SHARE_DELETE;
    auto name=path.lexically_normal().wstring();
    if(!name.starts_with(L"\\\\?\\")){if(name.starts_with(L"\\\\"))name=L"\\\\?\\UNC\\"+name.substr(2);else name=L"\\\\?\\"+name;}
    auto handle = CreateFileW(name.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle == invalid_handle) fail("CreateFileW");
#else
    (void)policy;
    auto handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (handle == invalid_handle) fail("open");
#endif
    return Handle(handle);
}

Stamp stamp(RawHandle h) {
    Stamp s;
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};
    const bool legacy = GetFileInformationByHandle(h, &info) != FALSE;
    FILE_STANDARD_INFO standard{};
    const bool standard_ok = GetFileInformationByHandleEx(h, FileStandardInfo, &standard, sizeof(standard)) != FALSE;
    if (!legacy && !standard_ok) {
        LARGE_INTEGER size{};if(!GetFileSizeEx(h,&size)||size.QuadPart<0) fail("GetFileSizeEx");
        s.size=static_cast<std::uint64_t>(size.QuadPart);
    }
    if (standard_ok) {
        if (standard.DeletePending) throw std::runtime_error("source is being deleted");
        if (standard.Directory || standard.EndOfFile.QuadPart < 0) throw std::runtime_error("source is not a regular file");
        s.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    } else if(legacy) {
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) throw std::runtime_error("source is a directory");
        s.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    }
    if (legacy) {
        s.volume = info.dwVolumeSerialNumber;
        s.id_low = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        s.identity_available = s.id_low != 0;
        s.modified = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
            info.ftLastWriteTime.dwLowDateTime;
    }
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic))) {
        s.modified = static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart);
        s.changed = static_cast<std::uint64_t>(basic.ChangeTime.QuadPart);
    }
    FILE_ID_INFO id{};
    if (GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id))) {
        s.volume = id.VolumeSerialNumber;
        s.id_low = s.id_high = 0;
        for (unsigned i = 0; i < 8; ++i) {
            s.id_low |= static_cast<std::uint64_t>(id.FileId.Identifier[i]) << (8 * i);
            s.id_high |= static_cast<std::uint64_t>(id.FileId.Identifier[i + 8]) << (8 * i);
        }
        s.identity_available = (s.id_low | s.id_high) != 0;
        s.extended_identity = s.identity_available;
    }
#else
    struct stat info{};
    if (::fstat(h, &info) != 0) fail("fstat");
    if (!S_ISREG(info.st_mode) || info.st_size < 0) throw std::runtime_error("source is not a regular file");
    s.size = static_cast<std::uint64_t>(info.st_size);
    s.volume = static_cast<std::uint64_t>(info.st_dev);
    s.id_low = static_cast<std::uint64_t>(info.st_ino);
    s.identity_available = true;
    s.modified = static_cast<std::uint64_t>(info.st_mtim.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(info.st_mtim.tv_nsec);
    s.changed = static_cast<std::uint64_t>(info.st_ctim.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(info.st_ctim.tv_nsec);
#endif
    return s;
}

void positioned_read(RawHandle h, std::uint64_t offset, std::span<std::byte> buffer) {
#ifdef _WIN32
    if (buffer.empty()) return;
    auto event_handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event_handle) fail("CreateEventW");
    Handle event(event_handle);
    while (!buffer.empty()) {
        OVERLAPPED request{};
        request.Offset = static_cast<DWORD>(offset);
        request.OffsetHigh = static_cast<DWORD>(offset >> 32);
        request.hEvent = event.get();
        if (!ResetEvent(event.get())) fail("ResetEvent");
        const auto count = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), 1u << 30));
        DWORD transferred = 0;
        if (!ReadFile(h, buffer.data(), count, nullptr, &request) && GetLastError() != ERROR_IO_PENDING)
            fail("ReadFile");
        if (!GetOverlappedResult(h, &request, &transferred, TRUE)) fail("GetOverlappedResult");
        if (!transferred || transferred > count) throw std::runtime_error("unexpected end of source");
        offset += transferred;
        buffer = buffer.subspan(transferred);
    }
#else
    while (!buffer.empty()) {
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            throw std::out_of_range("source offset exceeds off_t");
        const auto count = std::min<std::size_t>(buffer.size(), 1u << 30);
        const auto result = ::pread(h, buffer.data(), count, static_cast<off_t>(offset));
        if (result < 0) { if (errno == EINTR) continue; fail("pread"); }
        if (!result) throw std::runtime_error("unexpected end of source");
        const auto transferred = static_cast<std::size_t>(result);
        offset += transferred;
        buffer = buffer.subspan(transferred);
    }
#endif
}


#ifdef _WIN32
class Oplock {
public:
    explicit Oplock(std::shared_ptr<Handle> handle):file_(std::move(handle)),signal_(CreateEventW(nullptr,TRUE,FALSE,nullptr)),stop_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
        if(!signal_.get()||!stop_.get()) fail("CreateEventW");
        input_.StructureVersion=REQUEST_OPLOCK_CURRENT_VERSION;input_.StructureLength=sizeof(input_);
        input_.RequestedOplockLevel=OPLOCK_LEVEL_CACHE_READ|OPLOCK_LEVEL_CACHE_HANDLE;
        input_.Flags=REQUEST_OPLOCK_INPUT_FLAG_REQUEST;request_.hEvent=signal_.get();
        DWORD returned=0;
        auto ok=DeviceIoControl(file_->get(),FSCTL_REQUEST_OPLOCK,&input_,sizeof(input_),&output_,sizeof(output_),&returned,&request_);
        pending_=!ok&&GetLastError()==ERROR_IO_PENDING;
    }
    Oplock(Oplock const&)=delete;
    Oplock& operator=(Oplock const&)=delete;
    ~Oplock(){
        if(worker_.joinable()){SetEvent(stop_.get());worker_.join();}
        else if(pending_&&file_){CancelIoEx(file_->get(),&request_);WaitForSingleObject(signal_.get(),INFINITE);}
    }
    bool pending()const noexcept{return pending_;}
    void request_stop()const noexcept{SetEvent(stop_.get());}
    bool broken()const noexcept{return WaitForSingleObject(signal_.get(),0)==WAIT_OBJECT_0;}
    void join(){if(worker_.joinable())worker_.join();}
    void monitor(std::function<void()> invalidate,std::shared_mutex& io){
        worker_=std::thread([this,invalidate=std::move(invalidate),&io]{
            HANDLE events[]{signal_.get(),stop_.get()};
            auto result=WaitForMultipleObjects(2,events,FALSE,INFINITE);
            if(result!=WAIT_OBJECT_0){CancelIoEx(file_->get(),&request_);WaitForSingleObject(signal_.get(),INFINITE);}
            // Prevent new reads, cancel outstanding I/O and wait for read
            // operations to leave the handle. Closing the handle acknowledges
            // the oplock break even when a read session still exists.
            invalidate();
            CancelIoEx(file_->get(),nullptr);
            {std::unique_lock lock(io);file_->close();}
            file_.reset();
        });
    }
private:
    std::shared_ptr<Handle> file_;Handle signal_,stop_;
    REQUEST_OPLOCK_INPUT_BUFFER input_{};REQUEST_OPLOCK_OUTPUT_BUFFER output_{};OVERLAPPED request_{};
    bool pending_=false;std::thread worker_;
};
#endif

class FileSource final : public RandomAccessSource, public std::enable_shared_from_this<FileSource> {
    class Session final : public ReadSession {
    public:
        explicit Session(std::shared_ptr<const FileSource> owner) : owner_(std::move(owner)) {
            owner_->ensure_valid();
            try {
                retained_=owner_->retained_handle();
                if (retained_) {
#ifdef _WIN32
                    std::shared_lock lock(owner_->retained_io_);
                    owner_->ensure_valid();
#endif
                    handle_ = retained_->get();
                } else {
                    temporary_ = std::make_unique<Handle>(open_handle(owner_->path_, owner_->policy_));
                    handle_ = temporary_->get();
                }
                owner_->check_for_read(handle_,static_cast<bool>(retained_));
            } catch (...) { owner_->invalidate(); throw; }
        }
        void read_exact(std::uint64_t offset, std::span<std::byte> output) const override {
            if (finished_) throw std::logic_error("read session has ended");
            if (offset > owner_->size() || output.size() > owner_->size() - offset)
                throw std::out_of_range("source read outside file");
            owner_->ensure_valid();
            try {
#ifdef _WIN32
                std::shared_lock lock(owner_->retained_io_,std::defer_lock);
                if(retained_)lock.lock();
                owner_->ensure_valid();
#endif
                positioned_read(handle_, offset, output);
                owner_->ensure_valid();
            } catch (...) { owner_->invalidate(); throw; }
        }
        void finish() override {
            if (finished_) return;
            finished_ = true;
            try {
                owner_->check_for_read(handle_,static_cast<bool>(retained_));
                if (temporary_) {
                    // Check the named file, not only the handle that may now
                    // refer to a file that has been renamed or removed.
                    auto current = open_handle(owner_->path_, owner_->policy_);
                    owner_->check(current.get());
                }
                owner_->ensure_valid();
            } catch (...) { owner_->invalidate(); temporary_.reset(); throw; }
            temporary_.reset();retained_.reset();
        }
    private:
        std::shared_ptr<const FileSource> owner_;
        std::unique_ptr<Handle> temporary_;
        std::shared_ptr<Handle> retained_;
        RawHandle handle_ = invalid_handle;
        bool finished_ = false;
    };
public:
    FileSource(std::filesystem::path path, SourcePolicy policy)
        : path_(std::filesystem::absolute(std::move(path))), policy_(policy) {
        auto h = std::make_shared<Handle>(open_handle(path_, policy_));
        original_ = stamp(h->get());
        if (policy_ == SourcePolicy::protected_handle) retained_ = h;
#ifdef _WIN32
        else {
            // SMB redirectors may not support application-requested oplocks.
            // If the request is rejected, use short-lived read handles.
            auto path_root=path_.root_path();
            if(GetDriveTypeW(path_root.c_str())!=DRIVE_REMOTE){
                auto oplock=std::make_unique<Oplock>(h);
                if(oplock->pending()){retained_=h;oplock_=std::move(oplock);}
            }
        }
#endif
    }
    ~FileSource() override {
#ifdef _WIN32
        if(oplock_){oplock_->request_stop();oplock_->join();oplock_.reset();}
#endif
    }
    void start_monitor(){
#ifdef _WIN32
        if(oplock_)oplock_->monitor([this]{invalidate();},retained_io_);
#endif
    }
    std::uint64_t size() const noexcept override { return original_.size; }
    void validate() const override {
        ensure_valid();
        try {
            if (auto retained=retained_handle()) {
#ifdef _WIN32
                {std::shared_lock lock(retained_io_);ensure_valid();check_for_read(retained->get(),true);}
#else
                check_for_read(retained->get(),true);
#endif
#ifdef _WIN32
                if(policy_==SourcePolicy::transient_handle){auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();auto previous=last_probe_.load();if(now-previous>=500&&last_probe_.compare_exchange_strong(previous,now)){auto h=open_handle(path_,policy_);check(h.get());}}
#endif
            }
            else { auto h = open_handle(path_, policy_); check(h.get()); }
            ensure_valid();
        } catch (...) { invalidate(); throw; }
    }
    std::unique_ptr<ReadSession> begin_read() const override {
        return std::make_unique<Session>(shared_from_this());
    }
private:
    void invalidate() const noexcept {
        if(invalid_.exchange(true,std::memory_order_acq_rel))return;
#ifdef _WIN32
        if(oplock_)oplock_->request_stop();
        if(policy_==SourcePolicy::transient_handle){std::lock_guard lock(retained_mutex_);retained_.reset();}
#endif
    }
    void ensure_valid() const {
        if (invalid_.load(std::memory_order_acquire)
#ifdef _WIN32
            ||(oplock_&&oplock_->broken())
#endif
        ) throw std::runtime_error("source generation is invalid");
    }
    std::shared_ptr<Handle> retained_handle() const {
        std::lock_guard lock(retained_mutex_);return retained_;
    }
    void check(RawHandle handle) const {
        if (!original_.matches(stamp(handle))) throw std::runtime_error("source was modified or replaced");
    }
    void check_for_read(RawHandle handle,bool retained) const {
#ifdef _WIN32
        // FILE_SHARE_READ on the retained Windows handle excludes writers and
        // delete/rename access. No metadata query is needed on that fast path.
        if (retained) {ensure_valid();return;}
#else
        (void)retained;
#endif
        check(handle);
    }
    const std::filesystem::path path_;
    const SourcePolicy policy_;
    Stamp original_{};
    mutable std::mutex retained_mutex_;
    mutable std::shared_ptr<Handle> retained_;
#ifdef _WIN32
    mutable std::shared_mutex retained_io_;
    mutable std::atomic<std::int64_t> last_probe_{0};
    std::unique_ptr<Oplock> oplock_;
#endif
    mutable std::atomic<bool> invalid_{false};
};
} // namespace

std::shared_ptr<RandomAccessSource> open_file_source(std::filesystem::path path, SourcePolicy policy) {
    auto source=std::make_shared<FileSource>(std::move(path), policy);source->start_monitor();return source;
}
} // namespace archivefs
