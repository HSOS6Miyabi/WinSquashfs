// SPDX-License-Identifier: GPL-3.0-only
#include "directory_changes.hpp"
#ifdef _WIN32
#include "archive_tree.hpp"
#include <windows.h>
#include <array>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <cstddef>
#include <cstring>

namespace archivefs {
namespace {
namespace fs=std::filesystem;
std::wstring native_path(const fs::path& path) {
    auto name=path.lexically_normal().wstring();
    if(name.starts_with(L"\\\\?\\"))return name;
    if(name.starts_with(L"\\\\"))return L"\\\\?\\UNC\\"+name.substr(2);
    return L"\\\\?\\"+name;
}
class Monitor {
    struct Subscriber {
        std::wstring name;
        std::weak_ptr<DirectoryChangeState> state;
        bool directory;
    };
public:
    explicit Monitor(const fs::path& path) {
        file_=CreateFileW(native_path(path).c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr);
        if(file_==INVALID_HANDLE_VALUE)return;
        io_=CreateThreadpoolIo(file_,&completed,this,nullptr);
    }
    ~Monitor() {
        {
            std::lock_guard lock(mutex_);
            stopping_=true;
            if(io_&&active_)CancelIoEx(file_,&request_);
        }
        if(io_){WaitForThreadpoolIoCallbacks(io_,FALSE);CloseThreadpoolIo(io_);}
        if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);
    }
    bool subscribe(std::wstring name,const std::shared_ptr<DirectoryChangeState>& state,bool directory) {
        std::lock_guard lock(mutex_);
        if(!io_||failed_)return false;
        std::erase_if(subscribers_,[](auto& sub){return sub.state.expired();});
        subscribers_.push_back({std::move(name),state,directory});
        if(!active_)issue();
        return active_&&!failed_;
    }
private:
    void lost() {
        for(auto& sub:subscribers_)if(auto state=sub.state.lock()){
            state->complete.store(false,std::memory_order_release);
            state->revision.fetch_add(1,std::memory_order_release);
        }
    }
    void issue() {
        request_={};
        StartThreadpoolIo(io_);
        active_=true;
        if(!ReadDirectoryChangesW(file_,buffer_.data(),static_cast<DWORD>(buffer_.size()),FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|
            FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_CREATION,nullptr,&request_,nullptr)){
            if(GetLastError()!=ERROR_IO_PENDING){CancelThreadpoolIo(io_);active_=false;failed_=true;lost();}
        }
    }
    static void CALLBACK completed(PTP_CALLBACK_INSTANCE,void* data,void*,ULONG error,
                                    ULONG_PTR bytes,PTP_IO) noexcept {
        auto& self=*static_cast<Monitor*>(data);
        std::lock_guard lock(self.mutex_);
        self.active_=false;
        if(self.stopping_)return;
        if(error!=ERROR_SUCCESS||!bytes||bytes>self.buffer_.size()){
            self.lost();
            if(error!=ERROR_SUCCESS&&error!=ERROR_NOTIFY_ENUM_DIR){self.failed_=true;return;}
        }else{
            constexpr auto header=offsetof(FILE_NOTIFY_INFORMATION,FileName);
            std::size_t offset=0;
            while(offset<bytes){
                if(bytes-offset<header){self.lost();break;}
                auto* entry=reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(self.buffer_.data()+offset);
                auto length=entry->FileNameLength;
                if(length%sizeof(wchar_t)||length>bytes-offset-header){self.lost();break;}
                auto count=static_cast<int>(length/sizeof(wchar_t));
                for(auto& sub:self.subscribers_){
                    if(sub.directory&&entry->Action==FILE_ACTION_MODIFIED)continue;
                    if(CompareStringOrdinal(entry->FileName,count,sub.name.c_str(),
                        static_cast<int>(sub.name.size()),TRUE)==CSTR_EQUAL)
                        if(auto state=sub.state.lock())state->revision.fetch_add(1,std::memory_order_release);
                }
                auto next=entry->NextEntryOffset;
                if(!next)break;
                if(next<header+length||next>bytes-offset||next%alignof(DWORD)){self.lost();break;}
                offset+=next;
            }
        }
        std::erase_if(self.subscribers_,[](auto& sub){return sub.state.expired();});
        self.issue();
    }
    HANDLE file_=INVALID_HANDLE_VALUE;
    PTP_IO io_=nullptr;
    OVERLAPPED request_{};
    alignas(DWORD) std::array<std::byte,32768> buffer_{};
    std::mutex mutex_;
    std::vector<Subscriber> subscribers_;
    bool active_=false,stopping_=false,failed_=false;
};

std::shared_ptr<Monitor> get_monitor(const fs::path& path) {
    static std::mutex mutex;
    static std::map<std::u16string,std::weak_ptr<Monitor>,WindowsNameLess> monitors;
    std::lock_guard lock(mutex);
    std::erase_if(monitors,[](auto& item){return item.second.expired();});
    auto key=path.lexically_normal().u16string();
    if(auto it=monitors.find(key);it!=monitors.end())if(auto monitor=it->second.lock())return monitor;
    // A bounded number of watched directories prevents unbounded system
    // resource use. Sources without notification support use identity probes.
    if(monitors.size()>=2048)return {};
    auto monitor=std::make_shared<Monitor>(path);
    monitors[key]=monitor;
    return monitor;
}
}
DirectoryChanges::DirectoryChanges():state_(std::make_shared<DirectoryChangeState>()) {}
void DirectoryChanges::subscribe(const fs::path& file) {
    auto item=file.lexically_normal();
    auto root=item.root_path();
    // std::filesystem treats a UNC server as root_name; the share is the
    // first relative component and is itself the remote volume root.
    if(item.wstring().starts_with(L"\\\\")){
        auto rel=item.relative_path();
        if(rel.begin()!=rel.end())root/=*rel.begin();
    }
    bool directory=false;
    while(item!=root&&item.has_relative_path()&&!item.filename().empty()){
        auto parent=item.parent_path();
        if(parent.empty()||parent==item){state_->complete=false;return;}
        auto monitor=get_monitor(parent);
        if(!monitor||!monitor->subscribe(item.filename().wstring(),state_,directory)){
            state_->complete.store(false,std::memory_order_release);
        }else subscriptions_.push_back(std::move(monitor));
        item=std::move(parent);directory=true;
    }
}
}
#endif
