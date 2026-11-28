#!/usr/bin/env python3
"""Prepare the original native reader/tool build for the C++ application."""
# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import re
import sys

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[3]

def read(path):
    return (root / path).read_text(encoding='utf-8-sig')

def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError(f'Unexpected upstream source: {before!r}')
    return source.replace(before, after, 1)

def write(path, value):
    target = root / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(value, encoding='utf-8', newline='\n')

native = read('scripts/build-native.sh')
native = replace_once(native, 'native/reader.c \\', 'native/cpp/squashfs_parallel.c \\')
native = replace_once(native, 'native/shell-eject.cpp -lole32', 'native/cpp/shell.cpp -lole32')
write('scripts/build-native-cpp.sh', native)
shell = read('native/shell-eject.cpp').replace('WinSquashfs', 'WinArchiveFSPro')
start = shell.index('bool ActiveMount(')
end = shell.index('\nbool ValidPidl(', start)
shell = shell[:start] + r'''bool ActiveMount(const wchar_t (&drive)[4])
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
''' + shell[end:]
write('native/cpp/shell.cpp', shell)
erofs = read('scripts/build-erofs.sh')
erofs = replace_once(erofs, 'git -C .deps/erofs-utils checkout --detach FETCH_HEAD\n', '''git -C .deps/erofs-utils checkout --detach FETCH_HEAD
python3 native/cpp/scripts/erofs_binary.py .deps/erofs-utils/lib/tar.c
''')
erofs = replace_once(erofs, 'cp native/erofs-reader.c .deps/erofs-utils/winsqfs-reader.c', 'cp native/erofs-reader.c .deps/erofs-utils/erofs-reader-impl.c\ncp native/cpp/erofs_abi.c .deps/erofs-utils/winsqfs-reader.c')
write('scripts/build-erofs-cpp.sh', erofs)
installer = read('installer/WinSquashfs.nsi').replace('WinSquashfs', 'WinArchiveFSPro')
installer = installer.replace('Open via SquashFS UNC path', 'Open via archive UNC path').replace('通过 SquashFS UNC 路径打开', '通过存档 UNC 路径打开').replace('Create SquashFS image', 'Create archive image').replace('创建 SquashFS 镜像', '创建存档镜像')
# Preserve the existing ProgID so Windows user-selected file associations
# remain valid across a product-name upgrade.
installer = replace_once(installer, '!define PROGID "WinArchiveFSPro.Image"', '!define PROGID "WinSquashfs.Image"')
# Remove the previous installation before registering the new product.
installer = replace_once(installer, 'Section "WinArchiveFSPro"\n', r'''Section "WinArchiveFSPro"
  SetRegView 64
  ReadRegStr $0 HKLM "Software\WinSquashfs" "InstallDir"
  ${If} $0 != ""
    IfFileExists "$0\Uninstall.exe" 0 old_uninstall_missing
    ExecWait '"$0\Uninstall.exe" /S _?=$0' $1
    ${If} $1 != 0
      SetErrorLevel $1
      Quit
    ${EndIf}
    Delete /REBOOTOK "$0\Uninstall.exe"
    RMDir "$0"
    Goto old_uninstall_done
    old_uninstall_missing:
    MessageBox MB_ICONSTOP "Cannot locate the previous uninstaller." /SD IDOK
    SetErrorLevel 1
    Quit
    old_uninstall_done:
  ${EndIf}
''')
write('installer/WinArchiveFSPro.nsi', installer)

# Reuse the original Explorer interaction tests with the stable association ID.
for filename in ('association.ps1','pack-shell.ps1'):
    source = read('tests/' + filename).replace('WinSquashfs', 'WinArchiveFSPro').replace('winsqfs.exe', 'archivefs.exe')
    source = source.replace('WinArchiveFSPro.Image', 'WinSquashfs.Image')
    write('tests/native-' + filename, source)
