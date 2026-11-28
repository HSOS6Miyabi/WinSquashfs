// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#include <winsvc.h>
#endif

namespace archivefs {
inline bool ascii_host_equal(std::u16string_view a,std::u16string_view b)noexcept{
    if(a.size()!=b.size())return false;
    auto lower=[](char16_t c){return c>=u'A'&&c<=u'Z'?static_cast<char16_t>(c+32):c;};
    for(std::size_t i=0;i<a.size();++i)if(lower(a[i])!=lower(b[i]))return false;
    return true;
}
inline bool unc_filter_unqualified_policy()noexcept{
#ifdef _WIN32
    DWORD policy=0,size=sizeof(policy);
    return RegGetValueW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Services\\ArchiveFsUncFilter\\Parameters",
        L"IncludeUnqualified",RRF_RT_REG_DWORD,nullptr,&policy,&size)==ERROR_SUCCESS&&policy==1;
#else
    return false;
#endif
}
inline bool reserved_archive_host(std::u16string_view host,bool unqualified)noexcept{
    if(ascii_host_equal(host,u"sfs$")||ascii_host_equal(host,u"sfs.localhost"))return true;
    return host.size()>4&&ascii_host_equal(host.substr(0,4),u"sfs.")&&
        (unqualified||(host.size()>14&&ascii_host_equal(host.substr(host.size()-10),u".localhost")));
}
inline bool reserved_archive_host(std::u16string_view host)noexcept{
    if(reserved_archive_host(host,false))return true;
    return host.size()>4&&ascii_host_equal(host.substr(0,4),u"sfs.")&&unc_filter_unqualified_policy();
}
#ifdef _WIN32
inline bool unc_filter_running()noexcept{
    auto manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if(!manager)return false;
    auto service=OpenServiceW(manager,L"ArchiveFsUncFilter",SERVICE_QUERY_STATUS);
    SERVICE_STATUS status{};
    auto running=service&&QueryServiceStatus(service,&status)&&status.dwCurrentState==SERVICE_RUNNING;
    if(service)CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return running;
}
#endif
}
