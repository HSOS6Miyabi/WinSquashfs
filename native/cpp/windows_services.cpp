// SPDX-License-Identifier: GPL-3.0-only
#include "windows_services.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <array>
#include <cwctype>
#include <utility>
#include <thread>
#include <map>
#include <set>
namespace archivefs {
namespace fs=std::filesystem;
using namespace std::chrono_literals;
namespace {
std::wstring wide(std::u16string_view s){return {s.begin(),s.end()};}
[[noreturn]] void fail(const char* action){throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),action);}
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE v=nullptr):value(v){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& v)noexcept:value(std::exchange(v.value,nullptr)){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    explicit operator bool()const noexcept{return value&&value!=INVALID_HANDLE_VALUE;}
};
std::wstring sid(){HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))fail("OpenProcessToken");Handle token(raw);DWORD size=0;GetTokenInformation(raw,TokenUser,nullptr,0,&size);if(!size)fail("GetTokenInformation");std::vector<std::byte> bytes(size);if(!GetTokenInformation(raw,TokenUser,bytes.data(),size,&size))fail("GetTokenInformation");LPWSTR text=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text))fail("ConvertSidToStringSidW");std::wstring result(text);LocalFree(text);return result;}
struct UserSecurity {
    PSECURITY_DESCRIPTOR descriptor=nullptr;SECURITY_ATTRIBUTES attributes{};
    UserSecurity(){auto s=L"D:P(A;;GA;;;"+sid()+L")";if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(s.c_str(),SDDL_REVISION_1,&descriptor,nullptr))fail("security descriptor");attributes.nLength=sizeof(attributes);attributes.lpSecurityDescriptor=descriptor;}
    ~UserSecurity(){LocalFree(descriptor);}
};
std::wstring agent_name(std::wstring_view suffix){return L"Local\\WinArchiveFSPro.Unc."+sid()+L"."+std::wstring(suffix);}
std::wstring eject_name(std::u16string_view drive,std::wstring_view suffix){
    auto d=wide(drive);while(!d.empty()&&(d.back()==L'\\'||d.back()==L'/'))d.pop_back();if(d.size()!=2||d[1]!=L':'||!((d[0]>=L'A'&&d[0]<=L'Z')||(d[0]>=L'a'&&d[0]<=L'z')))throw std::invalid_argument("expected drive letter, e.g. X:");
    DWORD session=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&session))fail("ProcessIdToSessionId");return L"Local\\WinArchiveFSPro.Eject."+sid()+L"."+std::to_wstring(session)+L"."+static_cast<wchar_t>(towupper(d[0]))+L"."+std::wstring(suffix);
}
fs::path known_folder(REFKNOWNFOLDERID folder){PWSTR p=nullptr;if(FAILED(SHGetKnownFolderPath(folder,0,nullptr,&p)))throw std::runtime_error("cannot resolve user folder");fs::path result(p);CoTaskMemFree(p);return result;}
void cancelled(std::stop_token s){if(s.stop_requested())throw std::runtime_error("operation cancelled");}
void validate(const Settings& s){
    auto valid_alias=[](std::u16string_view alias){return !alias.empty()&&alias.size()<=64&&alias.find_first_of(u"\\/:*?\"<>| ")==std::u16string_view::npos&&alias!=u"."&&alias!=u".."&&alias.back()!=u'.'&&
        !std::any_of(alias.begin(),alias.end(),[](char16_t c){return c<32;});};
    std::set<std::u16string,WindowsNameLess> aliases{u"UNC"};
    if(s.root.empty()||!s.root.is_absolute()||!valid_alias(s.alias)||!aliases.insert(s.alias).second||
       s.idle_seconds>86400||s.max_idle_images>1024||s.cache_mib>4096||s.extra_shares.size()>64)throw std::invalid_argument("invalid UNC settings");
    for(auto const& extra:s.extra_shares)
        if(extra.root.empty()||!extra.root.is_absolute()||!valid_alias(extra.name)||!aliases.insert(extra.name).second)
            throw std::invalid_argument("invalid or duplicate directory alias");
    if(s.language!="system"&&s.language!="en-US"&&s.language!="zh-CN")throw std::invalid_argument("invalid language");
    if(s.theme!="system"&&s.theme!="light"&&s.theme!="dark")throw std::invalid_argument("invalid theme");
}
void autostart(bool enable){HKEY key=nullptr;auto rc=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);if(rc!=ERROR_SUCCESS)throw std::system_error(static_cast<int>(rc),std::system_category(),"login startup registry");struct Key{HKEY h;~Key(){RegCloseKey(h);}}guard{key};if(enable){auto value=L"\""+(application_directory()/L"WinArchiveFSPro.Unc.exe").wstring()+L"\"";rc=RegSetValueExW(key,L"WinArchiveFSProUNC",0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));}else{rc=RegDeleteValueW(key,L"WinArchiveFSProUNC");if(rc==ERROR_FILE_NOT_FOUND)rc=ERROR_SUCCESS;}if(rc!=ERROR_SUCCESS)throw std::system_error(static_cast<int>(rc),std::system_category(),"login startup registry");}
bool instance_stopped(){auto name=agent_name(L"Instance");Handle h(OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,name.c_str()));if(!h){if(GetLastError()==ERROR_FILE_NOT_FOUND)return true;fail("OpenMutexW");}auto wait=WaitForSingleObject(h.value,0);if(wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED){ReleaseMutex(h.value);return true;}if(wait==WAIT_TIMEOUT)return false;fail("WaitForSingleObject");}
}
fs::path application_directory(){std::wstring name(512,L'\0');for(;;){auto n=GetModuleFileNameW(nullptr,name.data(),static_cast<DWORD>(name.size()));if(!n)fail("GetModuleFileNameW");if(n<name.size()){name.resize(n);return fs::path(name).parent_path();}if(name.size()>=32768)throw std::length_error("module path too long");name.resize(name.size()*2);}}
fs::path settings_directory(){return known_folder(FOLDERID_LocalAppData)/L"WinArchiveFSPro";}
namespace {
void save_recent(const std::vector<fs::path>& paths){
    fs::create_directories(settings_directory());
    auto path=settings_directory()/L"recent.json";
    auto temp=path;temp+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    struct Cleanup{fs::path p;~Cleanup(){std::error_code ec;fs::remove(p,ec);}}cleanup{temp};
    auto json=nlohmann::json::array();for(auto& p:paths)json.push_back(to_utf8(p.u16string()));
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.exceptions(std::ios::failbit|std::ios::badbit);out<<json.dump(2);out.close();}
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))fail("save recent images");
}
}
std::vector<fs::path> recent_images(){try{auto path=settings_directory()/L"recent.json";if(!fs::exists(path))path=settings_directory().parent_path()/L"WinSquashfs"/L"recent.json";if(!fs::exists(path)||fs::file_size(path)>1u<<20)return {};std::ifstream input(path,std::ios::binary);auto json=nlohmann::json::parse(input);std::vector<fs::path> paths;for(auto& value:json){if(paths.size()==15)break;paths.emplace_back(to_utf16(value.get<std::string>()));}return paths;}catch(...){return {};}}
void remember_image(const fs::path& path){try{auto paths=recent_images();auto full=fs::absolute(path).lexically_normal();auto name=full.u16string();std::erase_if(paths,[&](auto& p){auto other=p.u16string();WindowsNameLess less;return !less(name,other)&&!less(other,name);});paths.insert(paths.begin(),full);if(paths.size()>15)paths.resize(15);save_recent(paths);}catch(...){}}
void clear_recent_images(){save_recent({});}
Settings Settings::load(){Settings s;s.root=known_folder(FOLDERID_Profile);auto path=settings_directory()/L"unc.json";if(!fs::exists(path))path=settings_directory().parent_path()/L"WinSquashfs"/L"unc.json";if(fs::exists(path)){std::ifstream in(path,std::ios::binary);auto j=nlohmann::json::parse(in);s.root=fs::u8path(j.value("Root",to_utf8(s.root.u16string())));s.alias=to_utf16(j.value("Alias",std::string("path")));s.include_drives=j.value("IncludeDrives",true);s.start_at_login=j.value("StartAtLogin",false);s.idle_seconds=j.value("IdleSeconds",60u);s.max_idle_images=j.value("MaxIdleImages",8u);s.cache_mib=j.value("CacheMiB",64u);s.language=j.value("Language",std::string("system"));s.theme=j.value("Theme",std::string("system"));for(auto const& item:j.value("ExtraShares",nlohmann::json::array()))s.extra_shares.push_back({to_utf16(item.at("Alias").get<std::string>()),fs::path(to_utf16(item.at("Root").get<std::string>()))});}if(!fs::exists(settings_directory()/L"unc.json")){auto language=settings_directory().parent_path()/L"WinSquashfs"/L"language.txt";if(fs::exists(language)){std::ifstream file(language);std::string value;file>>value;if(value=="en-US"||value=="zh-CN"||value=="system")s.language=value;}}validate(s);return s;}
void Settings::save()const{validate(*this);shares();fs::create_directories(settings_directory());auto path=settings_directory()/L"unc.json";auto temporary=path;temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";struct Cleanup{fs::path path;~Cleanup(){std::error_code ec;fs::remove(path,ec);}}cleanup{temporary};nlohmann::json j={{"Root",to_utf8(root.u16string())},{"Alias",to_utf8(alias)},{"IncludeDrives",include_drives},{"StartAtLogin",start_at_login},{"IdleSeconds",idle_seconds},{"MaxIdleImages",max_idle_images},{"CacheMiB",cache_mib},{"Language",language},{"Theme",theme}};j["ExtraShares"]=nlohmann::json::array();for(auto const& extra:extra_shares)j["ExtraShares"].push_back({{"Alias",to_utf8(extra.name)},{"Root",to_utf8(extra.root.u16string())}});{std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out.exceptions(std::ios::badbit|std::ios::failbit);out<<j.dump(2)<<'\n';out.close();}if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))fail("save settings");autostart(start_at_login);}
void migrate_settings(){
    auto current=settings_directory();
    auto legacy=current.parent_path()/L"WinSquashfs";
    if(!fs::exists(current/L"unc.json")&&
       (fs::exists(legacy/L"unc.json")||fs::exists(legacy/L"language.txt")))Settings::load().save();
    if(!fs::exists(current/L"recent.json")&&fs::exists(legacy/L"recent.json"))save_recent(recent_images());
}
std::vector<Share> Settings::shares()const{validate(*this);std::vector<Share> result;for(auto server:{u"sfs$",u"sfs.localhost"}){result.push_back({alias,root,false,server});for(auto const& extra:extra_shares)result.push_back({extra.name,extra.root,false,server});if(include_drives){auto mask=GetLogicalDrives();for(unsigned i=0;i<26;++i){if(!(mask&(1u<<i)))continue;std::u16string name(1,static_cast<char16_t>(u'A'+i));if(std::any_of(result.begin(),result.end(),[&](auto const& share){WindowsNameLess less;return share.server==server&&!less(share.name,name)&&!less(name,share.name);}))continue;auto drive=fs::path(name+u":\\");wchar_t format[64]{};if(GetVolumeInformationW(drive.c_str(),nullptr,0,nullptr,nullptr,nullptr,format,64)&&(!_wcsicmp(format,L"ArchiveFS")||!_wcsicmp(format,L"SquashFS")))continue;result.push_back({name,drive,false,server});}}result.push_back({u"UNC",{},true,server});}return result;}
std::u16string Settings::unc_path(const fs::path& source)const{auto p=fs::absolute(source).lexically_normal();if(p.u16string().starts_with(u"\\\\?\\UNC\\"))p=fs::path(u"\\\\"+p.u16string().substr(8));bool remote=p.u16string().starts_with(u"\\\\");for(auto& s:shares())if(s.server==u"sfs$"&&s.network==remote)if(auto path=image_unc_path(s,p))return *path;throw std::invalid_argument("image is outside the configured UNC shares");}
void open_in_explorer(std::u16string_view path){auto p=wide(path);auto result=ShellExecuteW(nullptr,L"open",p.c_str(),nullptr,nullptr,SW_SHOWNORMAL);if(reinterpret_cast<INT_PTR>(result)<=32)throw std::runtime_error("Explorer could not open the path");}
bool agent_running(){Handle h(OpenEventW(SYNCHRONIZE,FALSE,agent_name(L"Ready").c_str()));return h&&WaitForSingleObject(h.value,0)==WAIT_OBJECT_0;}
void start_agent(std::stop_token stop){cancelled(stop);if(agent_running())return;winfsp_runtime_info();auto executable=application_directory()/L"WinArchiveFSPro.Unc.exe";auto command=L"\""+executable.wstring()+L"\"";STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,DETACHED_PROCESS|CREATE_NEW_PROCESS_GROUP,nullptr,executable.parent_path().c_str(),&si,&pi))fail("start UNC agent");Handle proc(pi.hProcess),thread(pi.hThread);std::stop_callback cancel_start(stop,[&] noexcept {try{if(!agent_running())TerminateProcess(proc.value,1);}catch(...){TerminateProcess(proc.value,1);}});for(unsigned i=0;i<300;++i){if(agent_running())return;cancelled(stop);if(WaitForSingleObject(proc.value,50)==WAIT_OBJECT_0){if(instance_stopped())break;std::this_thread::sleep_for(50ms);}}if(!agent_running()&&WaitForSingleObject(proc.value,0)==WAIT_TIMEOUT)TerminateProcess(proc.value,1);throw std::runtime_error("UNC agent did not become ready; see unc.log in application settings");}
void stop_agent(std::stop_token stop){for(unsigned i=0;i<300;++i){cancelled(stop);Handle event(OpenEventW(EVENT_MODIFY_STATE,FALSE,agent_name(L"Stop").c_str()));if(event)SetEvent(event.value);if(instance_stopped())return;std::this_thread::sleep_for(50ms);}throw std::runtime_error("UNC agent did not stop");}

namespace {
struct RemoteAlias {
    std::u16string host,share;
    std::u16string server(bool qualified=true)const{
        return u"sfs."+host+(qualified?u".localhost":u"");
    }
    std::u16string prefix(bool qualified=true)const{
        return u"\\"+server(qualified)+u"\\"+share;
    }
    Share projection(bool qualified=true)const{
        return {share,fs::path(u"\\\\"+host+u"\\"+share+u"\\"),false,server(qualified)};
    }
};
bool alias_component(std::u16string_view value){
    return !value.empty()&&value.size()<=255&&value!=u"."&&value!=u".."&&
        value.back()!=u'.'&&value.back()!=u' '&&
        std::none_of(value.begin(),value.end(),[](char16_t ch){
            return ch<32||std::u16string_view(u"\\/:*?\"<>|").find(ch)!=std::u16string_view::npos;
        });
}
bool equal_name(std::u16string_view a,std::u16string_view b){
    WindowsNameLess less;return !less(a,b)&&!less(b,a);
}
bool reserved_host(std::u16string_view host){
    if(equal_name(host,u"sfs$")||equal_name(host,u"sfs.localhost"))return true;
    if(host.size()>=14&&equal_name(host.substr(0,4),u"sfs.")&&
        equal_name(host.substr(host.size()-10),u".localhost"))return true;
    return false;
}
RemoteAlias parse_alias(std::u16string host,std::u16string share){
    if(!alias_component(host)||!alias_component(share)||reserved_host(host))
        throw std::invalid_argument("invalid remote UNC source");
    return {std::move(host),std::move(share)};
}
std::optional<RemoteAlias> source_alias(const fs::path& source){
    auto value=fs::absolute(source).lexically_normal().u16string();
    if(value.starts_with(u"\\\\?\\UNC\\"))value=u"\\\\"+value.substr(8);
    if(!value.starts_with(u"\\\\")||value.starts_with(u"\\\\?\\")||
       value.starts_with(u"\\\\.\\"))return std::nullopt;
    auto host_end=value.find(u'\\',2);
    if(host_end==std::u16string::npos)return std::nullopt;
    auto share_end=value.find(u'\\',host_end+1);
    if(share_end==std::u16string::npos||share_end==value.size()-1)return std::nullopt;
    return parse_alias(value.substr(2,host_end-2),value.substr(host_end+1,share_end-host_end-1));
}
class AliasLock {
public:
    explicit AliasLock():security_(),handle_(CreateMutexW(&security_.attributes,FALSE,agent_name(L"AliasRegistry").c_str())){
        if(!handle_)fail("create alias registry mutex");
        auto result=WaitForSingleObject(handle_.value,10000);
        if(result!=WAIT_OBJECT_0&&result!=WAIT_ABANDONED)throw std::runtime_error("remote alias registry is busy");
    }
    ~AliasLock(){ReleaseMutex(handle_.value);}
private:
    UserSecurity security_;Handle handle_;
};
fs::path alias_requests(){return settings_directory()/L"remote-aliases.json";}
fs::path alias_status(){
    DWORD session=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&session))fail("get session ID");
    return settings_directory()/(L"remote-aliases-active-"+std::to_wstring(session)+L".json");
}
nlohmann::json read_alias_json(const fs::path& file){
    if(!fs::exists(file))return nlohmann::json::array();
    if(fs::file_size(file)>4u<<20)throw std::runtime_error("remote alias registry is too large");
    std::ifstream stream(file,std::ios::binary);
    auto json=nlohmann::json::parse(stream);
    if(!json.is_array())throw std::runtime_error("invalid remote alias registry");
    return json;
}
void write_alias_json(const fs::path& file,const nlohmann::json& json){
    fs::create_directories(file.parent_path());
    auto temp=file;temp+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    struct Cleanup{fs::path p;~Cleanup(){std::error_code ec;fs::remove(p,ec);}}cleanup{temp};
    {std::ofstream stream(temp,std::ios::binary|std::ios::trunc);
        stream.exceptions(std::ios::failbit|std::ios::badbit);stream<<json.dump(2)<<'\n';stream.close();}
    if(!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        fail("save remote alias registry");
}
void request_alias(const RemoteAlias& alias){
    AliasLock lock;
    auto requests=read_alias_json(alias_requests());
    for(auto& row:requests)
        if(equal_name(to_utf16(row.at("Host").get<std::string>()),alias.host)&&
           equal_name(to_utf16(row.at("Share").get<std::string>()),alias.share))return;
    requests.push_back({{"Host",to_utf8(alias.host)},{"Share",to_utf8(alias.share)}});
    write_alias_json(alias_requests(),requests);
}
bool alias_active(std::u16string_view prefix){
    AliasLock lock;
    for(auto& value:read_alias_json(alias_status()))
        if(equal_name(to_utf16(value.get<std::string>()),prefix))return true;
    return false;
}
class RemoteAliasMounts {
public:
    explicit RemoteAliasMounts(std::shared_ptr<ImagePool> pool,const std::vector<Share>& builtin,bool reserve_drives):pool_(std::move(pool)){
        for(auto& share:builtin)reserved_.insert(u"\\"+share.server+u"\\"+share.name);
        if(reserve_drives)for(auto server:{u"sfs$",u"sfs.localhost"})
            for(char16_t letter=u'A';letter<=u'Z';++letter)
                reserved_.insert(std::u16string(u"\\")+server+u"\\"+letter);
    }
    void refresh(){
        AliasLock lock;
        auto requests=read_alias_json(alias_requests());
        for(auto& row:requests){
            auto alias=parse_alias(to_utf16(row.at("Host").get<std::string>()),to_utf16(row.at("Share").get<std::string>()));
            for(bool qualified:{true,false}){
                auto prefix=alias.prefix(qualified);
                if(prefix.size()>=192||reserved_.contains(prefix)||mounts_.contains(prefix))continue;
                try{
                    auto share=alias.projection(qualified);
                    auto mount=std::make_unique<NativeMount>(unc_projection(share,pool_),u"",prefix);
                    mounts_.emplace(std::move(prefix),std::move(mount));
                }catch(const std::exception&){/* The generic UNC namespace remains available. */}
            }
        }
        auto active=nlohmann::json::array();
        for(auto& [prefix,mount]:mounts_)active.push_back(to_utf8(prefix));
        write_alias_json(alias_status(),active);
    }
    ~RemoteAliasMounts(){
        try{AliasLock lock;std::error_code ec;fs::remove(alias_status(),ec);}catch(...){}
    }
private:
    std::shared_ptr<ImagePool> pool_;
    std::set<std::u16string,WindowsNameLess> reserved_;
    std::map<std::u16string,std::unique_ptr<NativeMount>,WindowsNameLess> mounts_;
};
}

int run_agent(){
    winfsp_runtime_info();
    UserSecurity security;
    Handle instance(CreateMutexW(&security.attributes,FALSE,agent_name(L"Instance").c_str()));
    if(!instance)fail("CreateMutexW");
    auto ownership=WaitForSingleObject(instance.value,0);
    if(ownership==WAIT_TIMEOUT)return 0;
    if(ownership!=WAIT_OBJECT_0&&ownership!=WAIT_ABANDONED)fail("WaitForSingleObject");
    struct Release{HANDLE h;~Release(){ReleaseMutex(h);}}release{instance.value};
    Handle stop(CreateEventW(&security.attributes,TRUE,FALSE,agent_name(L"Stop").c_str())),
        ready(CreateEventW(&security.attributes,TRUE,FALSE,agent_name(L"Ready").c_str())),
        aliases(CreateEventW(&security.attributes,FALSE,FALSE,agent_name(L"AliasesChanged").c_str()));
    if(!stop||!ready||!aliases)fail("CreateEventW");
    ResetEvent(ready.value);ResetEvent(stop.value);
    auto settings=Settings::load();
    auto pool=std::make_shared<ImagePool>(make_archive_opener(application_directory(),
        static_cast<std::size_t>(settings.cache_mib)<<20),
        ImagePool::Settings{std::chrono::seconds(settings.idle_seconds),settings.max_idle_images});
    auto shares=settings.shares();
    std::map<std::u16string,std::unique_ptr<NativeMount>,WindowsNameLess> mounts;
    auto update_mounts=[&](const std::vector<Share>& current,bool initial){
        std::set<std::u16string,WindowsNameLess> wanted;
        bool complete=true;
        for(auto& share:current){
            auto prefix=u"\\"+share.server+u"\\"+share.name;
            wanted.insert(prefix);
            if(mounts.contains(prefix))continue;
            try{mounts.emplace(prefix,std::make_unique<NativeMount>(unc_projection(share,pool),u"",prefix));}
            catch(...){if(initial)throw;complete=false;}
        }
        std::erase_if(mounts,[&](const auto& entry){return !wanted.contains(entry.first);});
        return complete;
    };
    update_mounts(shares,true);
    DWORD drive_mask=GetLogicalDrives();
    RemoteAliasMounts remote(pool,shares,settings.include_drives);
    try{remote.refresh();}catch(const std::exception&){}
    if(!SetEvent(ready.value))fail("SetEvent");
    struct ResetReady{HANDLE event;~ResetReady(){ResetEvent(event);}}reset_ready{ready.value};
    HANDLE events[]{stop.value,aliases.value};
    for(;;){
        auto result=WaitForMultipleObjects(2,events,FALSE,settings.include_drives?1000:INFINITE);
        if(result==WAIT_OBJECT_0)break;
        if(result==WAIT_OBJECT_0+1){try{remote.refresh();}catch(const std::exception&){}}
        else if(result!=WAIT_TIMEOUT)fail("wait for UNC agent");
        if(settings.include_drives){
            auto mask=GetLogicalDrives();
            if(mask&&mask!=drive_mask){
                try{if(update_mounts(settings.shares(),false))drive_mask=mask;}catch(const std::exception&){}
            }
        }
    }
    return 0;
}
std::u16string register_unc_alias(const fs::path& source,std::stop_token stop){
    auto fallback=Settings::load().unc_path(source);
    auto alias=source_alias(source);
    if(!alias)return fallback;
    start_agent(stop);
    cancelled(stop);
    auto preferred=alias->prefix();
    if(preferred.size()>=192)return fallback;
    request_alias(*alias);
    Handle changed(OpenEventW(EVENT_MODIFY_STATE,FALSE,agent_name(L"AliasesChanged").c_str()));
    if(!changed)throw std::runtime_error("remote alias registry is unavailable");
    if(!SetEvent(changed.value))fail("notify remote alias registry");
    for(unsigned i=0;i<100;++i){
        cancelled(stop);
        if(alias_active(preferred)){
            auto normalized=source;
            auto value=normalized.u16string();
            if(value.starts_with(u"\\\\?\\UNC\\"))normalized=fs::path(u"\\\\"+value.substr(8));
            if(auto path=image_unc_path(alias->projection(),normalized))return *path;
            break;
        }
        if(!agent_running())break;
        std::this_thread::sleep_for(100ms);
    }
    return fallback;
}
void open_unc(const fs::path& source,std::stop_token stop){auto path=register_unc_alias(source,stop);start_agent(stop);cancelled(stop);auto attributes=GetFileAttributesW(wide(path).c_str());if(attributes==INVALID_FILE_ATTRIBUTES)fail("open UNC image");if(!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("UNC path is not an image directory");open_in_explorer(path);}
struct MountedVolume::Impl {
    std::unique_ptr<NativeMount> mount;fs::path source_path;std::u16string point;Handle stop,done;std::jthread worker;std::mutex mutex;
    Impl(const fs::path& source,const fs::path& decoders,std::u16string drive):source_path(fs::absolute(source)){winfsp_runtime_info();mount=std::make_unique<NativeMount>(mount_projection(make_archive_opener(decoders)(source,SourcePolicy::protected_handle)),std::move(drive));point=mount->mount_point();UserSecurity security;stop.value=CreateEventW(&security.attributes,TRUE,FALSE,eject_name(point,L"Stop").c_str());done.value=CreateEventW(&security.attributes,TRUE,FALSE,eject_name(point,L"Done").c_str());if(!stop||!done)fail("create eject events");ResetEvent(stop.value);ResetEvent(done.value);worker=std::jthread([this]{WaitForSingleObject(stop.value,INFINITE);std::lock_guard lock(mutex);mount->unmount();SetEvent(done.value);});}
    ~Impl(){SetEvent(stop.value);if(worker.joinable())worker.join();}
};
MountedVolume::MountedVolume(const fs::path& path,const fs::path& decoders,std::u16string drive):impl_(std::make_unique<Impl>(path,decoders,std::move(drive))){}
MountedVolume::~MountedVolume()=default;
std::u16string MountedVolume::drive()const{return impl_->point;}
const fs::path& MountedVolume::source()const{return impl_->source_path;}
bool MountedVolume::mounted()const noexcept{return WaitForSingleObject(impl_->done.value,0)==WAIT_TIMEOUT;}
void MountedVolume::wait(std::stop_token token){std::stop_callback callback(token,[this]{SetEvent(impl_->stop.value);});if(WaitForSingleObject(impl_->done.value,INFINITE)!=WAIT_OBJECT_0)fail("wait for unmount");}
void MountedVolume::unmount(){SetEvent(impl_->stop.value);wait();}
void eject_volume(std::u16string_view drive){Handle done(OpenEventW(SYNCHRONIZE,FALSE,eject_name(drive,L"Done").c_str()));if(!done)fail("find mounted image");Handle stop(OpenEventW(EVENT_MODIFY_STATE,FALSE,eject_name(drive,L"Stop").c_str()));if(!stop)fail("find mounted image");if(!SetEvent(stop.value))fail("request eject");if(WaitForSingleObject(done.value,20000)!=WAIT_OBJECT_0)throw std::runtime_error("eject did not finish");}
void serve_share(Share share,const fs::path& decoders,unsigned idle_seconds,std::stop_token stop){if(idle_seconds>86400)throw std::invalid_argument("invalid idle timeout");winfsp_runtime_info();auto pool=std::make_shared<ImagePool>(make_archive_opener(decoders),ImagePool::Settings{std::chrono::seconds(idle_seconds),8});NativeMount mount(unc_projection(share,pool),u"",u"\\"+share.server+u"\\"+share.name);std::mutex mutex;std::condition_variable_any cv;std::unique_lock lock(mutex);cv.wait(lock,stop,[]{return false;});}
}
