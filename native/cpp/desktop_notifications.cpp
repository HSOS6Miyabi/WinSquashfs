// SPDX-License-Identifier: GPL-3.0-only
#include "desktop_notifications.hpp"
#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <vector>

namespace archivefs {
namespace {
using namespace std::chrono_literals;
using namespace winrt::Windows::UI::Notifications;
using winrt::Windows::Data::Xml::Dom::XmlDocument;
constexpr wchar_t app_id[]=L"WinArchiveFSPro.Desktop";
constexpr wchar_t group[]=L"ArchiveOpen";
std::wstring wide(std::u16string_view s){return {s.begin(),s.end()};}
std::wstring xml_escape(std::wstring_view value){
    std::wstring result;
    for(auto ch:value){
        if(ch==L'&')result+=L"&amp;";
        else if(ch==L'<')result+=L"&lt;";
        else if(ch==L'>')result+=L"&gt;";
        else if(ch==L'"')result+=L"&quot;";
        else if(ch==L'\'')result+=L"&apos;";
        else if(ch>=32||ch==L'\n'||ch==L'\r'||ch==L'\t')result+=ch;
    }
    return result;
}
void register_shortcut(const std::filesystem::path& directory){
    PWSTR raw=nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_Programs,0,nullptr,&raw));
    struct Free{PWSTR value;~Free(){CoTaskMemFree(value);}} cleanup{raw};
    auto folder=std::filesystem::path(raw)/L"WinArchiveFSPro";
    std::filesystem::create_directories(folder);
    auto path=folder/L"WinArchiveFSPro.lnk";
    auto link=winrt::create_instance<IShellLinkW>(CLSID_ShellLink);
    auto persist=link.as<IPersistFile>();
    // Retain the existing shortcut's options when it is already present.
    if(std::filesystem::exists(path))persist->Load(path.c_str(),STGM_READWRITE);
    auto target=directory/L"WinArchiveFSPro.exe";
    winrt::check_hresult(link->SetPath(target.c_str()));
    winrt::check_hresult(link->SetWorkingDirectory(directory.c_str()));
    auto store=link.as<IPropertyStore>();
    PROPVARIANT prop{};
    winrt::check_hresult(InitPropVariantFromString(app_id,&prop));
    struct Clear{PROPVARIANT* p;~Clear(){PropVariantClear(p);}} clear{&prop};
    winrt::check_hresult(store->SetValue(PKEY_AppUserModel_ID,prop));
    winrt::check_hresult(store->Commit());
    winrt::check_hresult(persist->Save(path.c_str(),TRUE));
}
NotificationData progress_data(const OpenProgress& progress,bool chinese,std::uint32_t sequence){
    auto size=progress.size.load(std::memory_order_relaxed);
    auto offset=std::min(size,progress.offset.load(std::memory_order_relaxed));
    NotificationData data;
    data.SequenceNumber(sequence);
    auto values=data.Values();
    if(size){
        // The seek position is a measured byte offset, not a prediction of elapsed opening time.
        auto percent=static_cast<unsigned>(100.0L*offset/size);
        values.Insert(L"progressValue",winrt::hstring(std::to_wstring(percent/100)+L"."+std::to_wstring((percent%100)/10)+std::to_wstring(percent%10)));
        values.Insert(L"progressText",winrt::hstring(std::to_wstring(percent)+L"%"));
        values.Insert(L"progressStatus",chinese?L"正在读取镜像元数据":L"Reading archive metadata");
    }else{
        values.Insert(L"progressValue",L"indeterminate");
        values.Insert(L"progressText",L"");
        values.Insert(L"progressStatus",chinese?L"正在连接数据源":L"Connecting to source");
    }
    return data;
}
ToastNotification progress_toast(const OpenProgress& progress,bool chinese,std::wstring_view tag){
    auto title=chinese?L"正在打开镜像":L"Opening archive";
    auto subtitle=chinese?L"文件读取位置":L"File read position";
    auto xml=std::wstring(L"<toast><visual><binding template=\"ToastGeneric\"><text>")+title+L"</text><text>"+
        xml_escape(wide(progress.label))+L"</text><progress title=\""+subtitle+
        L"\" value=\"{progressValue}\" valueStringOverride=\"{progressText}\" status=\"{progressStatus}\"/>"
        L"</binding></visual><audio silent=\"true\"/></toast>";
    XmlDocument doc;doc.LoadXml(xml);
    ToastNotification toast(doc);toast.Tag(winrt::hstring(tag));toast.Group(group);
    toast.Data(progress_data(progress,chinese,1));
    return toast;
}
}
struct DesktopNotifications::State {
    struct Job {
        std::shared_ptr<OpenProgress> progress;
        std::wstring tag;
        ToastNotification toast{nullptr};
        std::uint32_t sequence=1;
        std::uint64_t previous_size=0,previous_offset=0;
        bool shown=false;
    };
    std::filesystem::path directory;
    bool chinese;
    DesktopNotificationObserver observer;
    void report(DesktopNotificationEvent event,const OpenProgress* progress=nullptr) noexcept {
        if(observer)try{observer(event,progress);}catch(...){}
    }
    std::mutex mutex;
    std::condition_variable_any wake;
    std::vector<std::shared_ptr<OpenProgress>> pending;
    std::map<std::u16string,std::u16string> errors;
    std::jthread thread;
    State(std::filesystem::path dir,bool zh,DesktopNotificationObserver callback):directory(std::move(dir)),chinese(zh),observer(std::move(callback)){
        thread=std::jthread([this](std::stop_token stop){run(stop);});
    }
    ~State(){thread.request_stop();wake.notify_all();if(thread.joinable())thread.join();}
    void run(std::stop_token stop)noexcept{
        try{
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
            register_shortcut(directory);
            auto notifier=ToastNotificationManager::CreateToastNotifier(app_id);
            std::vector<Job> jobs;
            std::map<std::u16string,std::chrono::steady_clock::time_point> last_error;
            std::uint32_t serial=0;
            auto remove=[&](Job& j){
                if(j.shown)try{
                    notifier.Hide(j.toast);
                    ToastNotificationManager::History().Remove(winrt::hstring(j.tag),group,app_id);
                    report(DesktopNotificationEvent::removed,j.progress.get());
                }catch(...){report(DesktopNotificationEvent::failed,j.progress.get());}
            };
            while(!stop.stop_requested()){
                std::vector<std::shared_ptr<OpenProgress>> next;
                std::map<std::u16string,std::u16string> messages;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock,stop,250ms,[&]{return !pending.empty()||!errors.empty();});
                    next.swap(pending);messages.swap(errors);
                }
                auto now=std::chrono::steady_clock::now();
                for(auto& p:next){
                    if(p->done.load(std::memory_order_acquire))continue;
                    if(jobs.size()>=128)continue;
                    wchar_t tag[17]{};
                    swprintf_s(tag,L"%08X%08X",static_cast<unsigned>(GetCurrentProcessId()),static_cast<unsigned>(++serial));
                    Job job;job.progress=std::move(p);job.tag=tag;jobs.push_back(std::move(job));
                }
                for(auto& [title,message]:messages){
                    auto previous=last_error.find(title);
                    if(previous!=last_error.end()&&now-previous->second<30s)continue;
                    last_error[title]=now;
                    try{
                        auto xml=L"<toast><visual><binding template=\"ToastGeneric\"><text>"+xml_escape(wide(title))+
                            L"</text><text>"+xml_escape(wide(message))+L"</text></binding></visual></toast>";
                        XmlDocument doc;doc.LoadXml(xml);notifier.Show(ToastNotification(doc));
                    }catch(...){}
                }
                for(auto it=jobs.begin();it!=jobs.end();){
                    auto& j=*it;
                    if(j.progress->done.load(std::memory_order_acquire)){remove(j);it=jobs.erase(it);continue;}
                    if(now-j.progress->started>=1500ms){
                        try{
                            auto size=j.progress->size.load(std::memory_order_relaxed);
                            auto offset=j.progress->offset.load(std::memory_order_relaxed);
                            if(!j.shown){
                                j.toast=progress_toast(*j.progress,chinese,j.tag);notifier.Show(j.toast);j.shown=true;
                                report(DesktopNotificationEvent::shown,j.progress.get());
                            }else if(size!=j.previous_size||offset!=j.previous_offset){
                                auto data=progress_data(*j.progress,chinese,++j.sequence);
                                auto result=notifier.Update(data,winrt::hstring(j.tag),group);
                                if(result==NotificationUpdateResult::NotificationNotFound){
                                    it=jobs.erase(it);continue;
                                }
                                if(result!=NotificationUpdateResult::Succeeded)
                                    throw std::runtime_error("Cannot update archive progress notification");
                                report(DesktopNotificationEvent::updated,j.progress.get());
                            }
                            j.previous_size=size;j.previous_offset=offset;
                        }catch(...){report(DesktopNotificationEvent::failed,j.progress.get());remove(j);it=jobs.erase(it);continue;}
                    }
                    ++it;
                }
            }
            for(auto& j:jobs)remove(j);
        }catch(...){
            report(DesktopNotificationEvent::failed);
            // Notifications must never prevent the filesystem agent from operating.
        }
    }
};
DesktopNotifications::DesktopNotifications(std::filesystem::path directory,bool chinese,DesktopNotificationObserver observer):state_(std::make_unique<State>(std::move(directory),chinese,std::move(observer))){}
DesktopNotifications::~DesktopNotifications()=default;
void DesktopNotifications::opening(std::shared_ptr<OpenProgress> progress){
    std::lock_guard lock(state_->mutex);
    std::erase_if(state_->pending,[](auto const& p){return p->done.load(std::memory_order_acquire);});
    if(state_->pending.size()<128)state_->pending.push_back(std::move(progress));
    state_->wake.notify_all();
}
void DesktopNotifications::error(std::u16string title,std::u16string message){
    std::lock_guard lock(state_->mutex);
    if(state_->errors.size()<32)state_->errors.insert_or_assign(std::move(title),std::move(message));
    state_->wake.notify_all();
}
}
