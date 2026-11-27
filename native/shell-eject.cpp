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

namespace {
const CLSID EjectClass = {0x9b20352f,0x1bc4,0x48e6,{0xae,0x3b,0x14,0x6f,0x32,0x65,0x7d,0xa9}};
constexpr wchar_t Verb[] = L"WinSquashfs.Eject";
HINSTANCE module = nullptr;
volatile LONG objects = 0;
volatile LONG locks = 0;

bool Chinese()
{
    DWORD language = 0, size = sizeof(language), type = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WinSquashfs", 0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
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
        wchar_t pipe[512];
        if (ProcessIdToSessionId(GetCurrentProcessId(), &session) &&
            SUCCEEDED(StringCchPrintfW(pipe, ARRAYSIZE(pipe),
                L"\\\\.\\pipe\\WinSquashfs.Eject.%ls.%lu.%lc", sid, session, drive[0])))
            ready = WaitNamedPipeW(pipe, 1) != FALSE;
    }
    if (sid) LocalFree(sid);
    if (buffer) LocalFree(buffer);
    CloseHandle(token);
    if (!ready) return false;
    wchar_t fs[64] = {};
    return GetVolumeInformationW(drive, nullptr, 0, nullptr, nullptr, nullptr, fs, ARRAYSIZE(fs)) &&
        _wcsicmp(fs, L"SquashFS") == 0;
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

HRESULT LaunchEject(const wchar_t (&drive)[4])
{
    if (!ActiveMount(drive)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    wchar_t exe[32768];
    DWORD n = GetModuleFileNameW(module, exe, ARRAYSIZE(exe));
    if (!n || n >= ARRAYSIZE(exe)) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    wchar_t *separator = wcsrchr(exe, L'\\');
    if (!separator) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    separator[1] = 0;
    HRESULT hr = StringCchCatW(exe, ARRAYSIZE(exe), L"WinSquashfs.exe");
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
public:
    EjectMenu() { InterlockedIncrement(&objects); }
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
        DriveFromSelection(data, drive);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last, UINT flags) override
    {
        if ((flags & CMF_DEFAULTONLY) || first > last || !ActiveMount(drive))
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
        const wchar_t *title = Chinese() ? L"弹出（WinSquashfs）" : L"Eject (WinSquashfs)";
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
            if (!ex->lpVerbW || IS_INTRESOURCE(ex->lpVerbW) || _wcsicmp(ex->lpVerbW, Verb)) return E_INVALIDARG;
        }
        else if (_stricmp(info->lpVerb, "WinSquashfs.Eject")) return E_INVALIDARG;
        return LaunchEject(drive);
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR id, UINT type, UINT *, LPSTR text, UINT size) override
    {
        if (id != 0) return E_INVALIDARG;
        switch (type)
        {
            case GCS_VERBW: return StringCchCopyW(reinterpret_cast<LPWSTR>(text), size, Verb);
            case GCS_VERBA: return StringCchCopyA(text, size, "WinSquashfs.Eject");
            case GCS_HELPTEXTW: return StringCchCopyW(reinterpret_cast<LPWSTR>(text), size,
                Chinese() ? L"卸载此 SquashFS 镜像并释放盘符" : L"Unmount this SquashFS image and release the drive letter");
            case GCS_HELPTEXTA: return StringCchCopyA(text, size, "Unmount this SquashFS image");
            case GCS_VALIDATEA: case GCS_VALIDATEW: return S_OK;
            default: return E_NOTIMPL;
        }
    }
};

class Factory final : public IClassFactory
{
    volatile LONG refs = 1;
public:
    Factory() { InterlockedIncrement(&objects); }
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
        auto *object = new(std::nothrow) EjectMenu;
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
    if (!IsEqualCLSID(clsid, EjectClass)) return CLASS_E_CLASSNOTAVAILABLE;
    auto *factory = new(std::nothrow) Factory;
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(iid, out);
    factory->Release();
    return hr;
}
