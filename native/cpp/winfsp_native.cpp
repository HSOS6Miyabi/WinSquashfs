// SPDX-License-Identifier: GPL-3.0-only
#include "winfsp_native.hpp"
#include "remote_transport.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
typedef NTSTATUS* PNTSTATUS;
#include <winfsp/winfsp.h>
#include <sddl.h>
#include <winver.h>
#include <array>
#include <cstring>
#include <mutex>
#include <system_error>
namespace archivefs {
namespace {
static_assert(sizeof(wchar_t)==sizeof(char16_t));
std::wstring wide(std::u16string_view s){return {reinterpret_cast<const wchar_t*>(s.data()),s.size()};}
std::u16string narrow(PWSTR s){return s?std::u16string(reinterpret_cast<const char16_t*>(s)):std::u16string{};}
[[noreturn]] void fail(const char* message){throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),message);}
void status(NTSTATUS s,const char* message){if(s<0)throw std::runtime_error(std::string(message)+" (NTSTATUS "+std::to_string(static_cast<std::uint32_t>(s))+")");}
struct Api {
    HMODULE module=nullptr;
    std::string version;
    decltype(&FspFileSystemCreate) create=nullptr;
    decltype(&FspFileSystemDelete) destroy=nullptr;
    decltype(&FspFileSystemSetMountPointEx) mount=nullptr;
    decltype(&FspFileSystemStartDispatcher) start=nullptr;
    decltype(&FspFileSystemStopDispatcher) stop=nullptr;
    decltype(&FspFileSystemAddDirInfo) add_dir=nullptr;
    decltype(&FspNtStatusFromWin32) from_win32=nullptr;
    template<class T>T get(const char* name){auto p=GetProcAddress(module,name);if(!p)throw std::runtime_error(std::string("WinFsp missing export: ")+name);T value;static_assert(sizeof(value)==sizeof(p));std::memcpy(&value,&p,sizeof(value));return value;}
    Api(){
        // Load the installed SDK/runtime, rather than any same-named DLL in
        // the current working directory.
        wchar_t path[32768]{};DWORD bytes=sizeof(path);
        auto result=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\WOW6432Node\\WinFsp",L"InstallDir",RRF_RT_REG_SZ,nullptr,path,&bytes);
        if(result!=ERROR_SUCCESS){bytes=sizeof(path);result=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\WinFsp",L"InstallDir",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6432KEY,nullptr,path,&bytes);}
        if(result!=ERROR_SUCCESS)throw std::system_error(static_cast<int>(result),std::system_category(),"WinFsp InstallDir");
        auto dll=std::filesystem::path(path)/L"bin"/L"winfsp-" L"" FSP_FSCTL_PRODUCT_FILE_ARCH L".dll";
        DWORD ignored=0;const auto bytes_version=GetFileVersionInfoSizeW(dll.c_str(),&ignored);
        if(!bytes_version)fail("read WinFsp version");
        std::vector<std::byte> version_bytes(bytes_version);
        if(!GetFileVersionInfoW(dll.c_str(),0,bytes_version,version_bytes.data()))fail("read WinFsp version");
        void* data=nullptr;UINT size=0;
        if(!VerQueryValueW(version_bytes.data(),L"\\",&data,&size)||size<sizeof(VS_FIXEDFILEINFO))throw std::runtime_error("invalid WinFsp version information");
        const auto& v=*static_cast<VS_FIXEDFILEINFO*>(data);
        auto major=HIWORD(v.dwFileVersionMS),minor=LOWORD(v.dwFileVersionMS),build=HIWORD(v.dwFileVersionLS),revision=LOWORD(v.dwFileVersionLS);
        version=std::to_string(major)+"."+std::to_string(minor)+"."+std::to_string(build)+"."+std::to_string(revision);
        if(major!=2||minor<2||(minor==2&&build<26323))throw std::runtime_error("WinFsp 2.2.26323 or newer 2.x is required; installed: "+version);
        module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module)fail("LoadLibraryExW WinFsp");
        try{create=get<decltype(create)>("FspFileSystemCreate");destroy=get<decltype(destroy)>("FspFileSystemDelete");mount=get<decltype(mount)>("FspFileSystemSetMountPointEx");start=get<decltype(start)>("FspFileSystemStartDispatcher");stop=get<decltype(stop)>("FspFileSystemStopDispatcher");add_dir=get<decltype(add_dir)>("FspFileSystemAddDirInfo");from_win32=get<decltype(from_win32)>("FspNtStatusFromWin32");}catch(...){FreeLibrary(module);throw;}
    }
    ~Api(){if(module)FreeLibrary(module);}
};
Api& api(){static Api instance;return instance;}
struct Security {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    explicit Security(bool writable=false){
        HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))fail("OpenProcessToken");
        struct Token{HANDLE h;~Token(){CloseHandle(h);}}owner{token};DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);if(!size)fail("GetTokenInformation");std::vector<std::byte> storage(size);if(!GetTokenInformation(token,TokenUser,storage.data(),size,&size))fail("GetTokenInformation");auto user=reinterpret_cast<TOKEN_USER*>(storage.data());LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(user->User.Sid,&sid))fail("ConvertSidToStringSidW");struct Local{LPWSTR p;~Local(){LocalFree(p);}}sid_owner{sid};
        std::wstring s=L"O:";s+=sid;s+=L"G:";s+=sid;s+=writable?L"D:P(A;;FA;;;":L"D:P(A;;0x1200A9;;;";s+=sid;s+=L")";
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(s.c_str(),SDDL_REVISION_1,&descriptor,nullptr))fail("ConvertStringSecurityDescriptorToSecurityDescriptorW");
    }
    ~Security(){if(descriptor)LocalFree(descriptor);}
    NTSTATUS copy(PSECURITY_DESCRIPTOR out,SIZE_T* length)const noexcept{if(!length)return STATUS_SUCCESS;auto needed=static_cast<SIZE_T>(GetSecurityDescriptorLength(descriptor));if(*length<needed){*length=needed;return STATUS_BUFFER_OVERFLOW;}*length=needed;if(out)std::memcpy(out,descriptor,needed);return STATUS_SUCCESS;}
};
UINT64 time_value(std::int64_t seconds)noexcept{constexpr auto epoch=11644473600LL;return static_cast<UINT64>(std::clamp<std::int64_t>(seconds,-epoch,253402300799LL)+epoch)*10000000ULL;}
void fill_info(const ProjectionInfo& info,FSP_FSCTL_FILE_INFO& result,bool writable=false){result={};result.FileAttributes=(writable?0:FILE_ATTRIBUTE_READONLY)|(info.directory?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_ARCHIVE)|(info.unavailable?FILE_ATTRIBUTE_SYSTEM|FILE_ATTRIBUTE_OFFLINE:0);result.FileSize=info.directory?0:info.size;result.AllocationSize=result.FileSize>UINT64_MAX-4095?result.FileSize:((result.FileSize+4095)/4096)*4096;result.CreationTime=result.LastAccessTime=result.LastWriteTime=result.ChangeTime=time_value(info.modified);result.IndexNumber=info.index;}
NTSTATUS exception_status()noexcept{try{throw;}catch(const std::bad_alloc&){return STATUS_INSUFFICIENT_RESOURCES;}catch(const RemoteFailure& e){switch(e.code()){case RemoteError::missing:return STATUS_OBJECT_NAME_NOT_FOUND;case RemoteError::exists:return STATUS_OBJECT_NAME_COLLISION;case RemoteError::permission:return STATUS_ACCESS_DENIED;case RemoteError::conflict:return STATUS_SHARING_VIOLATION;case RemoteError::unsupported:return STATUS_NOT_SUPPORTED;case RemoteError::timeout:return api().from_win32(ERROR_SEM_TIMEOUT);case RemoteError::offline:return api().from_win32(ERROR_CONNECTION_REFUSED);case RemoteError::not_empty:return STATUS_DIRECTORY_NOT_EMPTY;case RemoteError::no_space:return STATUS_DISK_FULL;default:return STATUS_FILE_INVALID;}}catch(const DeviceFailure& e){switch(e.code()){case DeviceError::unavailable:return api().from_win32(ERROR_CONNECTION_REFUSED);case DeviceError::offline:return api().from_win32(ERROR_DEVICE_NOT_CONNECTED);case DeviceError::permission:return STATUS_ACCESS_DENIED;case DeviceError::missing:return STATUS_OBJECT_NAME_NOT_FOUND;case DeviceError::timeout:return api().from_win32(ERROR_SEM_TIMEOUT);case DeviceError::unsupported:return STATUS_NOT_SUPPORTED;default:return STATUS_FILE_INVALID;}}catch(const std::invalid_argument&){return STATUS_INVALID_PARAMETER;}catch(const std::system_error& e){return api().from_win32(static_cast<ULONG>(e.code().value()));}catch(...){return STATUS_FILE_INVALID;}}
template<class F>NTSTATUS protect(F&& f)noexcept{try{return f();}catch(...){return exception_status();}}
constexpr UINT32 forbidden=FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|DELETE|WRITE_DAC|WRITE_OWNER;
}
struct NativeMount::Impl {
    struct Context {ProjectedNodePtr node;std::mutex mutex;std::vector<ProjectedNodePtr> enumeration;bool loaded=false;};
    std::shared_ptr<Projection> projection;Security security,write_security{true};FSP_FILE_SYSTEM* fs=nullptr;std::u16string point;std::mutex mutex;bool started=false;
    static Impl& self(FSP_FILE_SYSTEM* fs){return *static_cast<Impl*>(fs->UserContext);}
    static Context& context(PVOID raw){return *static_cast<Context*>(raw);}
    static NTSTATUS volume(FSP_FILE_SYSTEM* fs,FSP_FSCTL_VOLUME_INFO* info)noexcept{return protect([&]()->NTSTATUS{*info={};info->TotalSize=self(fs).projection->capacity();info->FreeSize=self(fs).projection->writable_volume()?info->TotalSize:0;constexpr wchar_t label[]=L"WinArchiveFSPro";std::copy_n(label,std::size(label)-1,info->VolumeLabel);info->VolumeLabelLength=static_cast<UINT16>((std::size(label)-1)*sizeof(wchar_t));return STATUS_SUCCESS;});}
    static void file_info(Impl& self,const ProjectedNodePtr& node,FSP_FSCTL_FILE_INFO& result){fill_info(self.projection->query_info(node),result,self.projection->writable(node));}
    static NTSTATUS security_name(FSP_FILE_SYSTEM* fs,PWSTR name,PUINT32 attrs,PSECURITY_DESCRIPTOR sd,SIZE_T* size)noexcept{return protect([&]()->NTSTATUS{
        auto& s=self(fs);auto node=s.projection->open(narrow(name));if(!node)return STATUS_OBJECT_NAME_NOT_FOUND;
        if(attrs){FSP_FSCTL_FILE_INFO info{};file_info(s,node,info);*attrs=info.FileAttributes;}
        return (s.projection->writable(node)?s.write_security:s.security).copy(sd,size);
    });}
    static NTSTATUS open(FSP_FILE_SYSTEM* fs,PWSTR name,UINT32 options,UINT32 access,PVOID* out,FSP_FSCTL_FILE_INFO* info)noexcept{return protect([&]()->NTSTATUS{
        auto& s=self(fs);if((access&forbidden)&&!s.projection->writable_volume())return STATUS_MEDIA_WRITE_PROTECTED;
        auto node=s.projection->open(narrow(name));if(!node)return STATUS_OBJECT_NAME_NOT_FOUND;
        if((access&forbidden)&&!s.projection->writable(node))return STATUS_MEDIA_WRITE_PROTECTED;
        if(node->info.unavailable&&(access&(FILE_READ_DATA|FILE_EXECUTE)))return STATUS_NOT_SUPPORTED;
        if((options&FILE_DIRECTORY_FILE)&&!node->info.directory)return STATUS_NOT_A_DIRECTORY;
        if((options&FILE_NON_DIRECTORY_FILE)&&node->info.directory)return STATUS_FILE_IS_A_DIRECTORY;
        auto holder=std::make_unique<Context>();holder->node=std::move(node);file_info(s,holder->node,*info);*out=holder.release();return STATUS_SUCCESS;
    });}
    static void report_error(Impl& s)noexcept{try{throw;}catch(const std::exception& e){s.projection->operation_error(e.what());}catch(...){s.projection->operation_error("Remote filesystem operation failed");}}
    static void cleanup(FSP_FILE_SYSTEM* fs,PVOID raw,PWSTR,ULONG flags)noexcept{
        auto& s=self(fs);auto& c=context(raw);
        if(!s.projection->writable(c.node))return;
        try{if(flags&FspCleanupDelete)s.projection->remove(c.node);else s.projection->flush(c.node);}
        catch(...){report_error(s);}
    }
    static void close(FSP_FILE_SYSTEM* fs,PVOID raw)noexcept{
        auto& s=self(fs);auto holder=std::unique_ptr<Context>(static_cast<Context*>(raw));
        if(holder&&s.projection->writable(holder->node))try{s.projection->flush(holder->node);}catch(...){report_error(s);}
    }
    static NTSTATUS read(FSP_FILE_SYSTEM* fs,PVOID raw,PVOID buffer,UINT64 offset,ULONG length,PULONG transferred)noexcept{return protect([&]()->NTSTATUS{
        *transferred=0;auto& s=self(fs);auto& c=context(raw);auto info=s.projection->query_info(c.node);
        if(info.directory)return STATUS_FILE_IS_A_DIRECTORY;if(info.unavailable)return STATUS_NOT_SUPPORTED;
        if(offset>=info.size&&length)return STATUS_END_OF_FILE;
        *transferred=static_cast<ULONG>(s.projection->read(c.node,offset,std::span(static_cast<std::byte*>(buffer),length)));return STATUS_SUCCESS;
    });}
    static NTSTATUS info(FSP_FILE_SYSTEM* fs,PVOID raw,FSP_FSCTL_FILE_INFO* result)noexcept{return protect([&]()->NTSTATUS{file_info(self(fs),context(raw).node,*result);return STATUS_SUCCESS;});}
    static NTSTATUS get_security(FSP_FILE_SYSTEM* fs,PVOID raw,PSECURITY_DESCRIPTOR sd,SIZE_T* size)noexcept{return protect([&]()->NTSTATUS{
        auto& s=self(fs);auto& node=context(raw).node;s.projection->validate(node);return (s.projection->writable(node)?s.write_security:s.security).copy(sd,size);
    });}
    static bool add(const Projection& projection,const ProjectedNodePtr& node,std::u16string_view name,PVOID buffer,ULONG length,PULONG transferred){
        if(name.size()>255)return false;alignas(FSP_FSCTL_DIR_INFO) std::array<std::byte,sizeof(FSP_FSCTL_DIR_INFO)+255*sizeof(wchar_t)> storage{};auto row=reinterpret_cast<FSP_FSCTL_DIR_INFO*>(storage.data());row->Size=static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO)+name.size()*sizeof(wchar_t));fill_info(node->info,row->FileInfo,projection.writable(node));std::memcpy(row->FileNameBuf,name.data(),name.size()*sizeof(char16_t));return api().add_dir(row,buffer,length,transferred)!=FALSE;
    }
    static NTSTATUS directory(FSP_FILE_SYSTEM* fs,PVOID raw,PWSTR,PWSTR marker,PVOID buffer,ULONG length,PULONG transferred)noexcept{return protect([&]()->NTSTATUS{
        *transferred=0;auto& c=context(raw);auto& p=*self(fs).projection;if(!c.node->info.directory)return STATUS_NOT_A_DIRECTORY;p.validate(c.node);std::lock_guard lock(c.mutex);
        if(!marker||!c.loaded){c.enumeration=p.list(c.node);std::sort(c.enumeration.begin(),c.enumeration.end(),[](auto& a,auto& b){return WindowsNameLess{}(a->name,b->name);});c.loaded=true;}
        auto m=narrow(marker);if(c.node->path!=u"\\"){
            if(m.empty()&&!add(p,c.node,u".",buffer,length,transferred))return STATUS_SUCCESS;
            if(m.empty()||m==u"."){auto parent=c.node->path.substr(0,c.node->path.find_last_of(u'\\'));if(parent.empty())parent=u"\\";auto node=p.open(parent);if(node&&!add(p,node,u"..",buffer,length,transferred))return STATUS_SUCCESS;}
        }
        auto it=c.enumeration.begin();if(!m.empty()&&m!=u"."&&m!=u"..")it=std::upper_bound(it,c.enumeration.end(),m,[](std::u16string_view key,const ProjectedNodePtr& n){return WindowsNameLess{}(key,n->name);});
        for(;it!=c.enumeration.end();++it)if(!add(p,*it,(*it)->name,buffer,length,transferred))return STATUS_SUCCESS;api().add_dir(nullptr,buffer,length,transferred);return STATUS_SUCCESS;
    });}
    static NTSTATUS create(FSP_FILE_SYSTEM* fs,PWSTR name,UINT32 options,UINT32,UINT32,PSECURITY_DESCRIPTOR,UINT64,PVOID* out,FSP_FSCTL_FILE_INFO* result)noexcept{return protect([&]()->NTSTATUS{
        auto& s=self(fs);if(!s.projection->writable_volume())return STATUS_MEDIA_WRITE_PROTECTED;
        auto node=s.projection->create_file(narrow(name),(options&FILE_DIRECTORY_FILE)!=0);auto c=std::make_unique<Context>();c->node=std::move(node);
        file_info(s,c->node,*result);*out=c.release();return STATUS_SUCCESS;
    });}
    static NTSTATUS overwrite(FSP_FILE_SYSTEM* fs,PVOID raw,UINT32,BOOLEAN,UINT64,FSP_FSCTL_FILE_INFO* result)noexcept{return protect([&]()->NTSTATUS{
        auto& s=self(fs);auto& node=context(raw).node;if(!s.projection->writable(node))return STATUS_MEDIA_WRITE_PROTECTED;
        s.projection->resize(node,0);file_info(s,node,*result);return STATUS_SUCCESS;
    });}
    static NTSTATUS write(FSP_FILE_SYSTEM* fs,PVOID raw,PVOID buffer,UINT64 offset,ULONG length,BOOLEAN append,BOOLEAN constrained,PULONG transferred,FSP_FSCTL_FILE_INFO* result)noexcept{return protect([&]()->NTSTATUS{
        *transferred=0;auto& s=self(fs);auto& node=context(raw).node;if(!s.projection->writable(node))return STATUS_MEDIA_WRITE_PROTECTED;
        *transferred=static_cast<ULONG>(s.projection->write(node,offset,std::span(static_cast<const std::byte*>(buffer),length),append!=FALSE,constrained!=FALSE));
        file_info(s,node,*result);return STATUS_SUCCESS;
    });}
    static NTSTATUS flush(FSP_FILE_SYSTEM* fs,PVOID raw,FSP_FSCTL_FILE_INFO* result)noexcept{return protect([&]()->NTSTATUS{
        auto& s=self(fs);if(raw){auto& node=context(raw).node;s.projection->flush(node);if(result)file_info(s,node,*result);}
        else s.projection->flush({});return STATUS_SUCCESS;
    });}
    static NTSTATUS size(FSP_FILE_SYSTEM* fs,PVOID raw,UINT64 value,BOOLEAN allocation,FSP_FSCTL_FILE_INFO* result)noexcept{return protect([&]()->NTSTATUS{
        auto& s=self(fs);auto& node=context(raw).node;if(!s.projection->writable(node))return STATUS_MEDIA_WRITE_PROTECTED;
        auto current=s.projection->query_info(node);if(!allocation||value<current.size)s.projection->resize(node,value);file_info(s,node,*result);return STATUS_SUCCESS;
    });}
    static NTSTATUS basic(FSP_FILE_SYSTEM* fs,PVOID raw,UINT32 attributes,UINT64 creation,UINT64 access,UINT64 modified,UINT64 change,FSP_FSCTL_FILE_INFO* result)noexcept{return protect([&]()->NTSTATUS{
        auto& s=self(fs);auto& node=context(raw).node;if(!s.projection->writable(node))return STATUS_MEDIA_WRITE_PROTECTED;
        // Remote volumes do not persist Windows-specific attributes or ACLs.
        // Do not invent timestamps for a server that does not support them.
        if(creation||access||modified||change||(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_READONLY)))return STATUS_NOT_SUPPORTED;
        file_info(s,node,*result);return STATUS_SUCCESS;
    });}
    static NTSTATUS can_delete(FSP_FILE_SYSTEM* fs,PVOID raw,PWSTR)noexcept{return protect([&]()->NTSTATUS{
        auto& s=self(fs);auto& node=context(raw).node;if(!s.projection->writable(node))return STATUS_MEDIA_WRITE_PROTECTED;
        s.projection->check_delete(node);return STATUS_SUCCESS;
    });}
    static NTSTATUS rename(FSP_FILE_SYSTEM* fs,PVOID raw,PWSTR,PWSTR name,BOOLEAN replace)noexcept{return protect([&]()->NTSTATUS{
        auto& s=self(fs);auto& c=context(raw);if(!s.projection->writable(c.node))return STATUS_MEDIA_WRITE_PROTECTED;
        s.projection->rename(c.node,narrow(name),replace!=FALSE);
        auto node=s.projection->open(narrow(name));if(node)c.node=std::move(node);
        return STATUS_SUCCESS;
    });}
    static const FSP_FILE_SYSTEM_INTERFACE& interface(){static const auto table=[] {FSP_FILE_SYSTEM_INTERFACE t{};t.GetVolumeInfo=volume;t.GetSecurityByName=security_name;t.Create=create;t.Open=open;t.Overwrite=overwrite;t.Cleanup=cleanup;t.Close=close;t.Read=read;t.Write=write;t.Flush=flush;t.GetFileInfo=info;t.SetBasicInfo=basic;t.SetFileSize=size;t.CanDelete=can_delete;t.Rename=rename;t.GetSecurity=get_security;t.ReadDirectory=directory;return t;}();return table;}
    Impl(std::shared_ptr<Projection> p,std::u16string mount,std::u16string prefix):projection(std::move(p)){
        if(!projection)throw std::invalid_argument("missing mount projection");if(!prefix.empty()&&!mount.empty())throw std::invalid_argument("UNC mount cannot assign a drive");auto& lib=api();FSP_FSCTL_VOLUME_PARAMS params{};params.Version=sizeof(params);params.SectorSize=4096;params.SectorsPerAllocationUnit=1;params.MaxComponentLength=255*sizeof(wchar_t);params.VolumeCreationTime=time_value(0);params.VolumeSerialNumber=GetTickCount();params.FileInfoTimeout=prefix.empty()?1000:0;params.CasePreservedNames=1;params.UnicodeOnDisk=1;params.PersistentAcls=1;params.ReadOnlyVolume=!projection->writable_volume();params.UmFileContextIsUserContext2=1;params.FlushAndPurgeOnCleanup=!prefix.empty();
        auto name=wide(prefix);if(name.size()>=std::size(params.Prefix))throw std::invalid_argument("UNC prefix too long");std::copy(name.begin(),name.end(),params.Prefix);constexpr wchar_t format[]=L"ArchiveFS";std::copy_n(format,std::size(format),params.FileSystemName);auto device=prefix.empty()?L"" FSP_FSCTL_DISK_DEVICE_NAME:L"" FSP_FSCTL_NET_DEVICE_NAME;
        status(lib.create(const_cast<PWSTR>(device),&params,&interface(),&fs),"create WinFsp file system");fs->UserContext=this;fs->OpGuardStrategy=FSP_FILE_SYSTEM_OPERATION_GUARD_STRATEGY_FINE;
        try{if(prefix.empty()){auto m=wide(mount);status(lib.mount(fs,m.empty()?nullptr:m.data(),nullptr),"assign mount point");point=narrow(fs->MountPoint);}else point=u"\\"+prefix;status(lib.start(fs,0),"start WinFsp dispatcher");started=true;}catch(...){lib.destroy(fs);fs=nullptr;throw;}
    }
    ~Impl(){unmount();}
    void unmount(){std::lock_guard lock(mutex);if(fs){if(started)api().stop(fs);if(projection->writable_volume())try{projection->flush({});}catch(...){report_error(*this);}api().destroy(fs);fs=nullptr;started=false;}}
};
NativeMount::NativeMount(std::shared_ptr<Projection> projection,std::u16string mount,std::u16string prefix):impl_(std::make_unique<Impl>(std::move(projection),std::move(mount),std::move(prefix))){}
NativeMount::~NativeMount()=default;
std::u16string NativeMount::mount_point()const{return impl_->point;}
void NativeMount::unmount(){impl_->unmount();}
std::string winfsp_runtime_info(){try{return "WinFsp runtime "+api().version+" (native)";}catch(const std::exception& e){throw std::runtime_error(std::string("WinFsp could not initialize: ")+e.what());}}
}
