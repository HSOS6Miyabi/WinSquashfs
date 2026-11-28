// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace archivefs {
inline bool unc_equal(std::wstring_view a,std::wstring_view b)noexcept{
    if(a.size()!=b.size()||a.size()>32767)return false;
#ifdef _WIN32
    return a.empty()||CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL;
#else
    auto lower=[](wchar_t c){return c>=L'A'&&c<=L'Z'?static_cast<wchar_t>(c+32):c;};
    for(std::size_t i=0;i<a.size();++i)if(lower(a[i])!=lower(b[i]))return false;
    return true;
#endif
}
struct RemoteUncPath {
    std::wstring protocol,connection,relative,leaf;
};
inline std::optional<RemoteUncPath> parse_remote_unc(std::wstring_view path){
    if(path.size()>32767||path.size()<3)return {};
    if(path.size()>=8&&unc_equal(path.substr(0,8),L"\\\\?\\UNC\\"))path.remove_prefix(8);
    else if(path.substr(0,2)==L"\\\\")path.remove_prefix(2);
    else return {};
    if(!path.empty()&&path.back()==L'\\')path.remove_suffix(1);
    std::vector<std::wstring_view> parts;
    while(!path.empty()){
        auto slash=path.find(L'\\');auto part=path.substr(0,slash);
        if(part.empty()||part==L"."||part==L".."||part.back()==L'.'||part.back()==L' '||part.size()>255||
           part.find_first_of(L"/:*?\"<>|")!=part.npos||part.find(L'\0')!=part.npos)return {};
        parts.push_back(part);
        if(slash==path.npos)break;
        path.remove_prefix(slash+1);
        if(path.empty())return {};
    }
    if(parts.size()<3||(!unc_equal(parts[0],L"$sfs")&&!unc_equal(parts[0],L"sfs$")&&!unc_equal(parts[0],L"sfs.localhost")))return {};
    std::size_t index=unc_equal(parts[1],L"root")?2:1;
    if(parts.size()<index+2)return {};
    RemoteUncPath result;
    // Only protocols providing a server-side copy mechanism are eligible.
    for(auto protocol:{L"dav",L"s3",L"sftp",L"scp"})
        if(unc_equal(parts[index],protocol)){result.protocol=protocol;break;}
    if(result.protocol.empty())return {};
    result.connection=parts[++index];result.relative=L"/";
    for(++index;index<parts.size();++index){
        if(result.relative!=L"/")result.relative+=L'/';
        result.relative+=parts[index];result.leaf=parts[index];
    }
    return result;
}
inline bool same_remote_connection(const RemoteUncPath& a,const RemoteUncPath& b)noexcept{
    return a.protocol==b.protocol&&unc_equal(a.connection,b.connection);
}
inline std::wstring server_paste_target(const RemoteUncPath& directory,const RemoteUncPath& source){
    if(!same_remote_connection(directory,source)||source.leaf.empty())return {};
    auto result=directory.relative;if(result!=L"/")result+=L'/';result+=source.leaf;
    if(unc_equal(result,source.relative))return {};
    return result;
}
}
