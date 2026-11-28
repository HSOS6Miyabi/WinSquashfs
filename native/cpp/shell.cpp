// SPDX-License-Identifier: GPL-3.0-only
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <sddl.h>
#include <strsafe.h>
#include <new>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {
const CLSID EjectClass = {0x9b20352f,0x1bc4,0x48e6,{0xae,0x3b,0x14,0x6f,0x32,0x65,0x7d,0xa9}};
const CLSID PackClass = {0x57ab2c11,0x7881,0x46eb,{0x9a,0xf8,0x33,0x19,0x4e,0xdf,0x60,0x71}};
constexpr wchar_t Verb[] = L"WinArchiveFSPro.Eject";
constexpr wchar_t PackVerb[] = L"WinArchiveFSPro.Pack";
HINSTANCE module = nullptr;
volatile LONG objects = 0;
volatile LONG locks = 0;

bool Chinese()
{
    DWORD language = 0, size = sizeof(language), type = 0;
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WinArchiveFSPro", 0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (status != ERROR_SUCCESS)
        status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WinArchiveFSPro", 0,
            KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key);
    if (status == ERROR_SUCCESS)
    {
        LONG result = RegQueryValueExW(key, L"InstallerLanguage", nullptr, &type,
            reinterpret_cast<BYTE *>(&language), &size);
        RegCloseKey(key);
        if (result == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(language))
            return language == 2052;
    }
    LANGID lang = GetUserDefaultUILanguage();
    return lang == 2052 || lang == 4100;
}

bool DriveRoot(const wchar_t *path, wchar_t (&drive)[4])
{
    if (!path) return false;
    size_t n = wcslen(path);
    wchar_t letter = path[0];
    if (letter >= L'a' && letter <= L'z') letter -= L'a' - L'A';
    if ((n != 2 && n != 3) || letter < L'A' || letter > L'Z' || path[1] != L':' ||
        (n == 3 && path[2] != L'\\' && path[2] != L'/')) return false;
    drive[0] = letter; drive[1] = L':'; drive[2] = L'\\'; drive[3] = 0;
    return true;
}

bool ActiveMount(const wchar_t (&drive)[4])
{
    if (!drive[0]) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    void *buffer = bytes ? LocalAlloc(LMEM_FIXED, bytes) : nullptr;
    LPWSTR sid = nullptr;
    bool ready = false;
    if (buffer && GetTokenInformation(token, TokenUser, buffer, bytes, &bytes) &&
        ConvertSidToStringSidW(static_cast<TOKEN_USER *>(buffer)->User.Sid, &sid))
    {
        DWORD session = 0;
        wchar_t event[512];
        if (ProcessIdToSessionId(GetCurrentProcessId(), &session) &&
            SUCCEEDED(StringCchPrintfW(event, ARRAYSIZE(event),
                L"Local\\WinArchiveFSPro.Eject.%ls.%lu.%lc.Stop", sid, session, drive[0])))
        {
            HANDLE handle = OpenEventW(EVENT_MODIFY_STATE, FALSE, event);
            if (handle) { ready = true; CloseHandle(handle); }
        }
    }
    if (sid) LocalFree(sid);
    if (buffer) LocalFree(buffer);
    CloseHandle(token);
    if (!ready) return false;
    wchar_t fs[64] = {};
    return GetVolumeInformationW(drive, nullptr, 0, nullptr, nullptr, nullptr, fs, ARRAYSIZE(fs)) &&
        _wcsicmp(fs, L"ArchiveFS") == 0;
}

bool ValidPidl(const BYTE *bytes, SIZE_T size, UINT offset)
{
    SIZE_T pos = offset;
    while (pos <= size && size - pos >= sizeof(USHORT))
    {
        USHORT cb;
        std::memcpy(&cb, bytes + pos, sizeof(cb));
        if (cb == 0) return true;
        if (cb < sizeof(USHORT) || cb > size - pos) return false;
        pos += cb;
    }
    return false;
}

bool DriveFromSelection(IDataObject *data, wchar_t (&drive)[4])
{
    if (!data) return false;
    wchar_t path[MAX_PATH] = {};
    FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium = {};
    if (SUCCEEDED(data->GetData(&format, &medium)))
    {
        HDROP drop = static_cast<HDROP>(medium.hGlobal);
        bool ok = medium.tymed == TYMED_HGLOBAL && drop &&
            DragQueryFileW(drop, 0xffffffff, nullptr, 0) == 1 &&
            DragQueryFileW(drop, 0, path, ARRAYSIZE(path)) < ARRAYSIZE(path) &&
            DriveRoot(path, drive);
        ReleaseStgMedium(&medium);
        if (ok) return true;
    }
    // Shell namespace selections need not provide CF_HDROP.
    format.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
    if (!format.cfFormat || FAILED(data->GetData(&format, &medium))) return false;
    bool ok = false;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal)
    {
        SIZE_T size = GlobalSize(medium.hGlobal);
        const BYTE *bytes = static_cast<const BYTE *>(GlobalLock(medium.hGlobal));
        if (bytes && size >= offsetof(CIDA, aoffset) + 2 * sizeof(UINT))
        {
            const CIDA *cida = reinterpret_cast<const CIDA *>(bytes);
            if (cida->cidl == 1 && ValidPidl(bytes, size, cida->aoffset[0]) &&
                ValidPidl(bytes, size, cida->aoffset[1]))
            {
                PIDLIST_ABSOLUTE full = ILCombine(
                    reinterpret_cast<PCIDLIST_ABSOLUTE>(bytes + cida->aoffset[0]),
                    reinterpret_cast<PCUIDLIST_RELATIVE>(bytes + cida->aoffset[1]));
                if (full)
                {
                    ok = SHGetPathFromIDListW(full, path) && DriveRoot(path, drive);
                    ILFree(full);
                }
            }
        }
        if (bytes) GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return ok;
}

struct Medium {
    STGMEDIUM value{};
    ~Medium() { if (value.tymed != TYMED_NULL) ReleaseStgMedium(&value); }
};
struct GlobalView {
    HGLOBAL handle;
    const BYTE *bytes;
    explicit GlobalView(HGLOBAL h) : handle(h), bytes(static_cast<const BYTE *>(GlobalLock(h))) {}
    ~GlobalView() { if (bytes) GlobalUnlock(handle); }
};

bool FilesFromSelection(IDataObject *data, std::vector<std::wstring> &paths)
{
    paths.clear();
    if (!data) return false;
    FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    {
        Medium m;
        if (SUCCEEDED(data->GetData(&format, &m.value)) && m.value.tymed == TYMED_HGLOBAL)
        {
            HDROP drop = static_cast<HDROP>(m.value.hGlobal);
            UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
            for (UINT i=0; i<count; ++i)
            {
                UINT len=DragQueryFileW(drop,i,nullptr,0);
                if (!len) { paths.clear(); break; }
                std::wstring path(len+1,L'\0');
                if (DragQueryFileW(drop,i,path.data(),len+1)!=len) { paths.clear(); break; }
                path.resize(len); paths.push_back(std::move(path));
            }
            if (!paths.empty()) return true;
        }
    }
    format.cfFormat=static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
    Medium m;
    if (!format.cfFormat || FAILED(data->GetData(&format,&m.value)) || m.value.tymed!=TYMED_HGLOBAL)
        return false;
    SIZE_T size=GlobalSize(m.value.hGlobal);
    GlobalView view(m.value.hGlobal);
    if(!view.bytes || size<offsetof(CIDA,aoffset)+2*sizeof(UINT))return false;
    auto *cida=reinterpret_cast<const CIDA *>(view.bytes);
    if(!cida->cidl || cida->cidl>(size-offsetof(CIDA,aoffset))/sizeof(UINT)-1 ||
        !ValidPidl(view.bytes,size,cida->aoffset[0]))return false;
    for(UINT i=0;i<cida->cidl;++i)
    {
        if(!ValidPidl(view.bytes,size,cida->aoffset[i+1])){paths.clear();return false;}
        PIDLIST_ABSOLUTE full=ILCombine(
            reinterpret_cast<PCIDLIST_ABSOLUTE>(view.bytes+cida->aoffset[0]),
            reinterpret_cast<PCUIDLIST_RELATIVE>(view.bytes+cida->aoffset[i+1]));
        if(!full){paths.clear();return false;}
        wchar_t path[32768]={};
        BOOL ok=SHGetPathFromIDListEx(full,path,ARRAYSIZE(path),GPFIDL_DEFAULT);
        ILFree(full);
        if(!ok){paths.clear();return false;}
        paths.emplace_back(path);
    }
    return !paths.empty();
}

HRESULT LaunchPack(const std::vector<std::wstring> &paths)
{
    if(paths.empty())return E_INVALIDARG;
    wchar_t folder[MAX_PATH+1]={}, list[MAX_PATH+1]={};
    DWORD len=GetTempPathW(ARRAYSIZE(folder),folder);
    if(!len || len>=ARRAYSIZE(folder) || !GetTempFileNameW(folder,L"wsq",0,list))
        return HRESULT_FROM_WIN32(GetLastError());
    HANDLE file=CreateFileW(list,GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){DWORD error=GetLastError();DeleteFileW(list);return HRESULT_FROM_WIN32(error);}
    DWORD written=0;
    bool ok=true;
    for(const auto &path:paths)
    {
        DWORD size=static_cast<DWORD>((path.size()+1)*sizeof(wchar_t));
        if(!WriteFile(file,path.c_str(),size,&written,nullptr)||written!=size){ok=false;break;}
    }
    DWORD error=GetLastError();
    CloseHandle(file);
    if(!ok){DeleteFileW(list);return HRESULT_FROM_WIN32(error?error:ERROR_WRITE_FAULT);}
    wchar_t exe[32768]={}, command[32768]={};
    DWORD n=GetModuleFileNameW(module,exe,ARRAYSIZE(exe));
    wchar_t *separator=wcsrchr(exe,L'\\');
    if(!n || n>=ARRAYSIZE(exe) || !separator){DeleteFileW(list);return E_FAIL;}
    separator[1]=0;
    HRESULT hr=StringCchCatW(exe,ARRAYSIZE(exe),L"WinArchiveFSPro.exe");
    if(SUCCEEDED(hr))hr=StringCchPrintfW(command,ARRAYSIZE(command),L"\"%ls\" --pack-list \"%ls\"",exe,list);
    STARTUPINFOW si={};si.cb=sizeof(si);PROCESS_INFORMATION pi={};
    if(FAILED(hr)||!CreateProcessW(exe,command,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))
    { if(SUCCEEDED(hr))hr=HRESULT_FROM_WIN32(GetLastError());DeleteFileW(list);return hr; }
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    return S_OK;
}

HRESULT LaunchEject(const wchar_t (&drive)[4])
{
    if (!ActiveMount(drive)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    wchar_t exe[32768];
    DWORD n = GetModuleFileNameW(module, exe, ARRAYSIZE(exe));
    if (!n || n >= ARRAYSIZE(exe)) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    wchar_t *separator = wcsrchr(exe, L'\\');
    if (!separator) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    separator[1] = 0;
    HRESULT hr = StringCchCatW(exe, ARRAYSIZE(exe), L"WinArchiveFSPro.exe");
    if (FAILED(hr)) return hr;
    wchar_t command[32768];
    hr = StringCchPrintfW(command, ARRAYSIZE(command), L"\"%ls\" --eject %lc:", exe, drive[0]);
    if (FAILED(hr)) return hr;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi)) return HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return S_OK;
}

class EjectMenu final : public IShellExtInit, public IContextMenu
{
    volatile LONG refs = 1;
    wchar_t drive[4] = {};
    bool pack;
    std::vector<std::wstring> paths;
public:
    explicit EjectMenu(bool create) : pack(create) { InterlockedIncrement(&objects); }
    ~EjectMenu() { InterlockedDecrement(&objects); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IShellExtInit))
            *out = static_cast<IShellExtInit *>(this);
        else if (IsEqualIID(iid, IID_IContextMenu)) *out = static_cast<IContextMenu *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG remaining = InterlockedDecrement(&refs);
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE, IDataObject *data, HKEY) override
    {
        drive[0] = 0;
        try { if(pack) FilesFromSelection(data,paths); else DriveFromSelection(data,drive); }
        catch (...) { paths.clear(); return E_OUTOFMEMORY; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last, UINT flags) override
    {
        if ((flags & CMF_DEFAULTONLY) || first > last || (pack ? paths.empty() : !ActiveMount(drive)))
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
        const wchar_t *title = pack ? (Chinese() ? L"使用 WinArchiveFSPro 创建存档" : L"Create archive with WinArchiveFSPro") :
            (Chinese() ? L"弹出（WinArchiveFSPro）" : L"Eject (WinArchiveFSPro)");
        if (!InsertMenuW(menu, index, MF_BYPOSITION | MF_STRING, first, title))
            return HRESULT_FROM_WIN32(GetLastError());
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 1);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO *info) override
    {
        if (!info) return E_INVALIDARG;
        if (IS_INTRESOURCE(info->lpVerb))
        {
            if (LOWORD(reinterpret_cast<ULONG_PTR>(info->lpVerb)) != 0) return E_INVALIDARG;
        }
        else if (info->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) && (info->fMask & CMIC_MASK_UNICODE))
        {
            auto *ex = reinterpret_cast<CMINVOKECOMMANDINFOEX *>(info);
            if (!ex->lpVerbW || IS_INTRESOURCE(ex->lpVerbW) || _wcsicmp(ex->lpVerbW, pack ? PackVerb : Verb)) return E_INVALIDARG;
        }
        else if (_stricmp(info->lpVerb, pack ? "WinArchiveFSPro.Pack" : "WinArchiveFSPro.Eject")) return E_INVALIDARG;
        try { return pack ? LaunchPack(paths) : LaunchEject(drive); }
        catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR id, UINT type, UINT *, LPSTR text, UINT size) override
    {
        if (id != 0) return E_INVALIDARG;
        switch (type)
        {
            case GCS_VERBW: return StringCchCopyW(reinterpret_cast<LPWSTR>(text), size, pack ? PackVerb : Verb);
            case GCS_VERBA: return StringCchCopyA(text, size, pack ? "WinArchiveFSPro.Pack" : "WinArchiveFSPro.Eject");
            case GCS_HELPTEXTW: return StringCchCopyW(reinterpret_cast<LPWSTR>(text), size,
                pack ? (Chinese() ? L"将所选文件及文件夹创建为一个存档" : L"Create one archive from the selected files and folders") :
                (Chinese() ? L"卸载此镜像并释放盘符" : L"Unmount this image and release the drive letter"));
            case GCS_HELPTEXTA: return StringCchCopyA(text, size, pack ? "Create one archive from the selection" : "Unmount this image");
            case GCS_VALIDATEA: case GCS_VALIDATEW: return S_OK;
            default: return E_NOTIMPL;
        }
    }
};

class Factory final : public IClassFactory
{
    volatile LONG refs = 1;
    bool pack;
public:
    explicit Factory(bool create) : pack(create) { InterlockedIncrement(&objects); }
    ~Factory() { InterlockedDecrement(&objects); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IClassFactory)) return E_NOINTERFACE;
        *out = static_cast<IClassFactory *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG remaining = InterlockedDecrement(&refs);
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto *object = new(std::nothrow) EjectMenu(pack);
        if (!object) return E_OUTOFMEMORY;
        HRESULT hr = object->QueryInterface(iid, out);
        object->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if (lock) InterlockedIncrement(&locks);
        else InterlockedDecrement(&locks);
        return S_OK;
    }
};
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow()
{
    return InterlockedCompareExchange(&objects, 0, 0) == 0 &&
        InterlockedCompareExchange(&locks, 0, 0) == 0 ? S_OK : S_FALSE;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    bool pack=IsEqualCLSID(clsid,PackClass);
    if (!pack && !IsEqualCLSID(clsid, EjectClass)) return CLASS_E_CLASSNOTAVAILABLE;
    auto *factory = new(std::nothrow) Factory(pack);
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(iid, out);
    factory->Release();
    return hr;
}
