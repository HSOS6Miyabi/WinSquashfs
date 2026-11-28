// SPDX-License-Identifier: GPL-3.0-only
#include "desktop_notifications.hpp"
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace archivefs;
using namespace std::chrono_literals;
using namespace winrt::Windows::UI::Notifications;
namespace {
template<class F> void wait_for(F predicate,const char* message){
    const auto deadline=std::chrono::steady_clock::now()+15s;
    while(std::chrono::steady_clock::now()<deadline){
        if(predicate())return;
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error(message);
}
bool in_history(const std::wstring& label){
    for(const auto& item:ToastNotificationManager::History().GetHistory(L"WinArchiveFSPro.Desktop")){
        const auto xml=item.Content().GetXml();
        if(item.Group()==L"ArchiveOpen"&&std::wstring_view(xml).find(label)!=std::wstring_view::npos&&
           std::wstring_view(xml).find(L"<progress")!=std::wstring_view::npos)return true;
    }
    return false;
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2){std::cerr<<"Usage: desktop_notifications_live_tests APP_DIRECTORY\n";return 2;}
    try{
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
        const auto directory=std::filesystem::absolute(argv[1]);
        if(!std::filesystem::is_regular_file(directory/L"WinArchiveFSPro.exe"))
            throw std::runtime_error("Missing GUI application");
        const auto label=L"ArchiveFS notification test "+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        auto progress=std::make_shared<OpenProgress>(std::u16string(label.begin(),label.end()));
        std::atomic<bool> shown{false},updated{false},removed{false},failed{false};
        DesktopNotifications notifications(directory,false,[&](DesktopNotificationEvent event,const OpenProgress* p){
            if(event==DesktopNotificationEvent::failed){failed=true;return;}
            if(p!=progress.get())return;
            if(event==DesktopNotificationEvent::shown)shown=true;
            if(event==DesktopNotificationEvent::updated&&p->offset.load()>=750)updated=true;
            if(event==DesktopNotificationEvent::removed)removed=true;
        });
        progress->size=1000;
        progress->advance(250);
        notifications.opening(progress);
        wait_for([&]{return failed.load()||(shown.load()&&in_history(label));},"Progress notification did not appear in Windows history");
        if(failed)throw std::runtime_error("Windows notification creation failed");
        progress->advance(750);
        wait_for([&]{return failed.load()||updated.load();},"Windows did not accept the progress update");
        if(failed)throw std::runtime_error("Windows notification update failed");
        progress->done=true;
        wait_for([&]{return failed.load()||(removed.load()&&!in_history(label));},"Progress notification was not removed");
        if(failed)throw std::runtime_error("Windows notification removal failed");
        std::cout<<"PASS Windows progress notification creation, update and removal\n";
        return 0;
    }catch(const winrt::hresult_error& e){
        std::cerr<<winrt::to_string(e.message())<<"\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";}
    return 1;
}
