// SPDX-License-Identifier: GPL-3.0-only
#include "windows_services.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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
void validate(const Settings& s){if(s.root.empty()||!s.root.is_absolute()||s.alias.empty()||s.alias.size()>64||s.alias.find_first_of(u"\\/:*?\"<>| ")!=std::u16string::npos||s.alias==u"."||s.alias==u".."||(!WindowsNameLess{}(s.alias,u"UNC")&&!WindowsNameLess{}(u"UNC",s.alias))||s.idle_seconds>86400||s.max_idle_images>1024||s.cache_mib>4096)throw std::invalid_argument("invalid UNC settings");if(s.language!="system"&&s.language!="en-US"&&s.language!="zh-CN")throw std::invalid_argument("invalid language");}
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
Settings Settings::load(){Settings s;s.root=known_folder(FOLDERID_Profile);auto path=settings_directory()/L"unc.json";if(!fs::exists(path))path=settings_directory().parent_path()/L"WinSquashfs"/L"unc.json";if(fs::exists(path)){std::ifstream in(path,std::ios::binary);auto j=nlohmann::json::parse(in);s.root=fs::u8path(j.value("Root",to_utf8(s.root.u16string())));s.alias=to_utf16(j.value("Alias",std::string("path")));s.include_drives=j.value("IncludeDrives",true);s.start_at_login=j.value("StartAtLogin",false);s.idle_seconds=j.value("IdleSeconds",60u);s.max_idle_images=j.value("MaxIdleImages",8u);s.cache_mib=j.value("CacheMiB",64u);s.language=j.value("Language",std::string("system"));}if(!fs::exists(settings_directory()/L"unc.json")){auto language=settings_directory().parent_path()/L"WinSquashfs"/L"language.txt";if(fs::exists(language)){std::ifstream file(language);std::string value;file>>value;if(value=="en-US"||value=="zh-CN"||value=="system")s.language=value;}}validate(s);return s;}
void Settings::save()const{validate(*this);shares();fs::create_directories(settings_directory());auto path=settings_directory()/L"unc.json";auto temporary=path;temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";struct Cleanup{fs::path path;~Cleanup(){std::error_code ec;fs::remove(path,ec);}}cleanup{temporary};nlohmann::json j={{"Root",to_utf8(root.u16string())},{"Alias",to_utf8(alias)},{"IncludeDrives",include_drives},{"StartAtLogin",start_at_login},{"IdleSeconds",idle_seconds},{"MaxIdleImages",max_idle_images},{"CacheMiB",cache_mib},{"Language",language}};{std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out.exceptions(std::ios::badbit|std::ios::failbit);out<<j.dump(2)<<'\n';out.close();}if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))fail("save settings");autostart(start_at_login);}
std::vector<Share> Settings::shares()const{validate(*this);std::vector<Share> result;for(auto server:{u"sfs$",u"sfs.localhost"}){result.push_back({alias,root,false,server});if(include_drives){auto mask=GetLogicalDrives();for(unsigned i=0;i<26;++i){if(!(mask&(1u<<i)))continue;std::u16string name(1,static_cast<char16_t>(u'A'+i));if(!WindowsNameLess{}(name,alias)&&!WindowsNameLess{}(alias,name))throw std::invalid_argument("UNC alias conflicts with drive letter");auto drive=fs::path(name+u":\\");wchar_t format[64]{};if(GetVolumeInformationW(drive.c_str(),nullptr,0,nullptr,nullptr,nullptr,format,64)&&(!_wcsicmp(format,L"ArchiveFS")||!_wcsicmp(format,L"SquashFS")))continue;result.push_back({name,drive,false,server});}}result.push_back({u"UNC",{},true,server});}return result;}
std::u16string Settings::unc_path(const fs::path& source)const{auto p=fs::absolute(source).lexically_normal();if(p.u16string().starts_with(u"\\\\?\\UNC\\"))p=fs::path(u"\\\\"+p.u16string().substr(8));bool remote=p.u16string().starts_with(u"\\\\");for(auto& s:shares())if(s.server==u"sfs$"&&s.network==remote)if(auto path=image_unc_path(s,p))return *path;throw std::invalid_argument("image is outside the configured UNC shares");}
void open_in_explorer(std::u16string_view path){auto p=wide(path);auto result=ShellExecuteW(nullptr,L"open",p.c_str(),nullptr,nullptr,SW_SHOWNORMAL);if(reinterpret_cast<INT_PTR>(result)<=32)throw std::runtime_error("Explorer could not open the path");}
bool agent_running(){Handle h(OpenEventW(SYNCHRONIZE,FALSE,agent_name(L"Ready").c_str()));return h&&WaitForSingleObject(h.value,0)==WAIT_OBJECT_0;}
void start_agent(std::stop_token stop){cancelled(stop);if(agent_running())return;winfsp_runtime_info();auto executable=application_directory()/L"WinArchiveFSPro.Unc.exe";auto command=L"\""+executable.wstring()+L"\"";STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,DETACHED_PROCESS|CREATE_NEW_PROCESS_GROUP,nullptr,executable.parent_path().c_str(),&si,&pi))fail("start UNC agent");Handle proc(pi.hProcess),thread(pi.hThread);std::stop_callback cancel_start(stop,[&] noexcept {try{if(!agent_running())TerminateProcess(proc.value,1);}catch(...){TerminateProcess(proc.value,1);}});for(unsigned i=0;i<300;++i){if(agent_running())return;cancelled(stop);if(WaitForSingleObject(proc.value,50)==WAIT_OBJECT_0){if(instance_stopped())break;std::this_thread::sleep_for(50ms);}}if(!agent_running()&&WaitForSingleObject(proc.value,0)==WAIT_TIMEOUT)TerminateProcess(proc.value,1);throw std::runtime_error("UNC agent did not become ready; see unc.log in application settings");}
void stop_agent(std::stop_token stop){for(unsigned i=0;i<300;++i){cancelled(stop);Handle event(OpenEventW(EVENT_MODIFY_STATE,FALSE,agent_name(L"Stop").c_str()));if(event)SetEvent(event.value);if(instance_stopped())return;std::this_thread::sleep_for(50ms);}throw std::runtime_error("UNC agent did not stop");}
int run_agent(){winfsp_runtime_info();UserSecurity security;Handle instance(CreateMutexW(&security.attributes,FALSE,agent_name(L"Instance").c_str()));if(!instance)fail("CreateMutexW");auto ownership=WaitForSingleObject(instance.value,0);if(ownership==WAIT_TIMEOUT)return 0;if(ownership!=WAIT_OBJECT_0&&ownership!=WAIT_ABANDONED)fail("WaitForSingleObject");struct Release{HANDLE h;~Release(){ReleaseMutex(h);}}release{instance.value};Handle stop(CreateEventW(&security.attributes,TRUE,FALSE,agent_name(L"Stop").c_str())),ready(CreateEventW(&security.attributes,TRUE,FALSE,agent_name(L"Ready").c_str()));if(!stop||!ready)fail("CreateEventW");ResetEvent(ready.value);ResetEvent(stop.value);auto settings=Settings::load();auto pool=std::make_shared<ImagePool>(make_archive_opener(application_directory(),static_cast<std::size_t>(settings.cache_mib)<<20),ImagePool::Settings{std::chrono::seconds(settings.idle_seconds),settings.max_idle_images});std::vector<std::unique_ptr<NativeMount>> mounts;for(auto& share:settings.shares())mounts.push_back(std::make_unique<NativeMount>(unc_projection(share,pool),u"",u"\\"+share.server+u"\\"+share.name));if(!SetEvent(ready.value))fail("SetEvent");auto rv=WaitForSingleObject(stop.value,INFINITE);ResetEvent(ready.value);if(rv!=WAIT_OBJECT_0)fail("wait for UNC stop");return 0;}
void open_unc(const fs::path& source,std::stop_token stop){auto path=Settings::load().unc_path(source);start_agent(stop);cancelled(stop);auto attributes=GetFileAttributesW(wide(path).c_str());if(attributes==INVALID_FILE_ATTRIBUTES)fail("open UNC image");if(!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("UNC path is not an image directory");open_in_explorer(path);}
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
