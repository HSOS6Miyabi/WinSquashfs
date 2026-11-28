Unicode true
ManifestDPIAware true
ManifestDPIAwareness "PerMonitorV2,System"
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WordFunc.nsh"
!ifndef PRODUCT_VERSION
  !error "Pass /DPRODUCT_VERSION=<version> from Directory.Build.props"
!endif
!ifndef WINFSP_MIN_VERSION
  !error "Pass /DWINFSP_MIN_VERSION from Directory.Build.props"
!endif
!ifndef PRODUCT_ARCH
  !define PRODUCT_ARCH "x64"
!endif
!ifndef DIST_DIR
  !define DIST_DIR "..\\artifacts\\dist"
!endif
!if "${PRODUCT_ARCH}" == "x86"
  !define PRODUCT_REGVIEW 32
  !define PRODUCT_PROGRAMFILES "$PROGRAMFILES32"
  !define WINFSP_ARCH "x86"
!else if "${PRODUCT_ARCH}" == "arm64"
  !define PRODUCT_REGVIEW 64
  !define PRODUCT_PROGRAMFILES "$PROGRAMFILES64"
  !define WINFSP_ARCH "a64"
!else if "${PRODUCT_ARCH}" == "x64"
  !define PRODUCT_REGVIEW 64
  !define PRODUCT_PROGRAMFILES "$PROGRAMFILES64"
  !define WINFSP_ARCH "x64"
!else
  !error "Unsupported PRODUCT_ARCH"
!endif
VIProductVersion "${PRODUCT_VERSION}.0"
!define PROGID "WinSquashfs.Image"
!define EJECT_CLSID "{9B20352F-1BC4-48E6-AE3B-146F32657DA9}"
!define PACK_CLSID "{57AB2C11-7881-46EB-9AF8-33194EDF6071}"
!define PASTE_CLSID "{A0BD4E92-67D5-4F50-9DC7-A1618DE82A63}"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\WinArchiveFSPro"
Name "WinArchiveFSPro"
!ifndef SETUP_OUTPUT
  !define SETUP_OUTPUT "..\artifacts\WinArchiveFSPro-Setup-${PRODUCT_ARCH}.exe"
!endif
OutFile "${SETUP_OUTPUT}"
!ifdef ARCHIVEFS_SIGN_INSTALLER
  !uninstfinalize 'powershell.exe -NoProfile -NonInteractive -File "$%ARCHIVEFS_SIGN_SCRIPT%" -Path "%1"' = 0
  !finalize 'powershell.exe -NoProfile -NonInteractive -File "$%ARCHIVEFS_SIGN_SCRIPT%" -Path "%1"' = 0
!endif
InstallDir "${PRODUCT_PROGRAMFILES}\WinArchiveFSPro"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
!define MUI_ICON "..\src\App\Assets\app-icon.ico"
!define MUI_UNICON "..\src\App\Assets\app-icon.ico"

!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "assets\header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "assets\header.bmp"
!define MUI_HEADERIMAGE_BITMAP_STRETCH "FitControl"
!define MUI_HEADERIMAGE_UNBITMAP_STRETCH "FitControl"
!define MUI_WELCOMEFINISHPAGE_BITMAP "assets\wizard.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "assets\wizard.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP_STRETCH "FitControl"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP_STRETCH "FitControl"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

VIAddVersionKey /LANG=1033 "ProductName" "WinArchiveFSPro"
VIAddVersionKey /LANG=1033 "FileDescription" "WinArchiveFSPro Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "ProductName" "WinArchiveFSPro"
VIAddVersionKey /LANG=2052 "FileDescription" "WinArchiveFSPro 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${PRODUCT_VERSION}"

LangString BrowseVerb ${LANG_ENGLISH} "Browse with WinArchiveFSPro"
LangString BrowseVerb ${LANG_SIMPCHINESE} "使用 WinArchiveFSPro 浏览"
LangString MountVerb ${LANG_ENGLISH} "Mount with WinArchiveFSPro"
LangString MountVerb ${LANG_SIMPCHINESE} "使用 WinArchiveFSPro 挂载"
LangString ExtractVerb ${LANG_ENGLISH} "Extract with WinArchiveFSPro"
LangString ExtractVerb ${LANG_SIMPCHINESE} "使用 WinArchiveFSPro 提取"
LangString UncVerb ${LANG_ENGLISH} "Open via archive UNC path"
LangString UncVerb ${LANG_SIMPCHINESE} "通过存档 UNC 路径打开"
LangString PackVerb ${LANG_ENGLISH} "Create archive image"
LangString PackVerb ${LANG_SIMPCHINESE} "创建存档镜像"
LangString Description ${LANG_ENGLISH} "Browse and mount archives and filesystem images; create SquashFS, DwarFS and EROFS images."
LangString Description ${LANG_SIMPCHINESE} "浏览和挂载存档及文件系统镜像，创建 SquashFS、DwarFS 和 EROFS 镜像。"
LangString ImageType ${LANG_ENGLISH} "Read-only archive or filesystem image"
LangString ImageType ${LANG_SIMPCHINESE} "只读存档或文件系统镜像"
LangString EditVerb ${LANG_ENGLISH} "Edit with WinArchiveFSPro"
LangString EditVerb ${LANG_SIMPCHINESE} "使用 WinArchiveFSPro 编辑"
LangString UninstallLink ${LANG_ENGLISH} "Uninstall"
LangString UninstallLink ${LANG_SIMPCHINESE} "卸载"
LangString DriverError ${LANG_ENGLISH} "WinFsp installation failed (exit code $1)."
LangString DriverError ${LANG_SIMPCHINESE} "WinFsp 安装失败（退出代码：$1）。"
LangString DriverIncompatible ${LANG_ENGLISH} "Installed WinFsp $4 is incompatible with this release. WinFsp ${WINFSP_MIN_VERSION} or a later 2.x release is required."
LangString DriverIncompatible ${LANG_SIMPCHINESE} "当前安装的 WinFsp $4 与本版本不兼容。需要 WinFsp ${WINFSP_MIN_VERSION} 或更新的 2.x 版本。"
LangString DriverMissing ${LANG_ENGLISH} "WinFsp installation did not provide a compatible runtime. Restart Windows if required, then reinstall WinArchiveFSPro."
LangString DriverMissing ${LANG_SIMPCHINESE} "WinFsp 安装后仍缺少兼容的运行时。请按需重新启动 Windows，再重新安装 WinArchiveFSPro。"
LangString DriverRestart ${LANG_ENGLISH} "Restart Windows before mounting images or starting UNC access."
LangString DriverRestart ${LANG_SIMPCHINESE} "请重新启动 Windows 后再挂载镜像或启动 UNC 访问。"

Function .onInit
  SetRegView ${PRODUCT_REGVIEW}
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/LANG=" $R1
  ${IfNot} ${Errors}
    ${If} $R1 == "2052"
      StrCpy $LANGUAGE 2052
    ${ElseIf} $R1 == "1033"
      StrCpy $LANGUAGE 1033
    ${Else}
      SetErrorLevel 2
      Quit
    ${EndIf}
  ${Else}
    !insertmacro MUI_LANGDLL_DISPLAY
  ${EndIf}
  ReadRegStr $0 HKLM "Software\WinArchiveFSPro" "InstallDir"
  ${If} $0 != ""
    StrCpy $INSTDIR $0
  ${EndIf}
FunctionEnd

!macro RegisterExtension EXT CLAIM_DEFAULT
  !if ${CLAIM_DEFAULT} == 1
  ReadRegStr $0 HKLM "Software\Classes\${EXT}" ""
  ${If} $0 == ""
    WriteRegStr HKLM "Software\Classes\${EXT}" "" "${PROGID}"
  ${EndIf}
  !endif
  WriteRegStr HKLM "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}" ""
  WriteRegStr HKLM "Software\WinArchiveFSPro\Capabilities\FileAssociations" "${EXT}" "${PROGID}"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Browse" "" "$(BrowseVerb)"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Browse\command" "" '"$INSTDIR\app\WinArchiveFSPro.exe" --open "%1"'
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Mount" "" "$(MountVerb)"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Mount\command" "" '"$INSTDIR\app\WinArchiveFSPro.exe" --mount "%1"'
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Extract" "" "$(ExtractVerb)"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Extract\command" "" '"$INSTDIR\app\WinArchiveFSPro.exe" --extract "%1"'
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Unc" "" "$(UncVerb)"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Unc\command" "" '"$INSTDIR\app\WinArchiveFSPro.exe" --unc "%1"'
!macroend
!macro UnregisterExtension EXT
  ReadRegStr $0 HKLM "Software\Classes\${EXT}" ""
  ${If} $0 == "${PROGID}"
    DeleteRegValue HKLM "Software\Classes\${EXT}" ""
  ${EndIf}
  DeleteRegValue HKLM "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}"
  DeleteRegKey HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Browse"
  DeleteRegKey HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Mount"
  DeleteRegKey HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Extract"
  DeleteRegKey HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinArchiveFSPro.Unc"
!macroend

Function ReadWinFspVersion
  SetRegView 32
  ReadRegStr $0 HKLM "SOFTWARE\WinFsp" "InstallDir"
  SetRegView ${PRODUCT_REGVIEW}
  StrCpy $4 ""
  StrCpy $5 0
  ${If} $0 != ""
    ClearErrors
    GetDLLVersion "$0\bin\winfsp-${WINFSP_ARCH}.dll" $2 $3
    ${IfNot} ${Errors}
      IntOp $5 $2 >> 16
      IntOp $6 $2 & 0xFFFF
      IntOp $7 $3 >> 16
      IntOp $8 $3 & 0xFFFF
      StrCpy $4 "$5.$6.$7.$8"
    ${EndIf}
  ${EndIf}
FunctionEnd

Section "WinFsp runtime"
  Call ReadWinFspVersion
  ${If} $4 != ""
    ${VersionCompare} "$4" "${WINFSP_MIN_VERSION}" $6
    ${If} $5 == 2
      ${If} $6 != 2
        DetailPrint "WinFsp $4"
        Goto winfsp_done
      ${EndIf}
    ${ElseIf} $5 > 2
      MessageBox MB_ICONSTOP "$(DriverIncompatible)" /SD IDOK
      SetErrorLevel 1
      Quit
    ${EndIf}
  ${EndIf}

  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File /oname=winfsp.msi "..\artifacts\winfsp.msi"
  ExecWait 'msiexec.exe /i "$PLUGINSDIR\winfsp.msi" /qn /norestart ADDLOCAL=ALL /L*v "$TEMP\WinArchiveFSPro-WinFsp-install.log"' $1
  ${If} $1 == 3010
    SetRebootFlag true
    DetailPrint "$(DriverRestart)"
    SetErrorLevel 3010
    Goto winfsp_done
  ${ElseIf} $1 != 0
    MessageBox MB_ICONSTOP "$(DriverError)" /SD IDOK
    SetErrorLevel $1
    Quit
  ${EndIf}
  Call ReadWinFspVersion
  ${VersionCompare} "$4" "${WINFSP_MIN_VERSION}" $6
  ${If} $4 == ""
  ${OrIf} $5 != 2
  ${OrIf} $6 == 2
    MessageBox MB_ICONSTOP "$(DriverMissing)" /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}
  winfsp_done:
SectionEnd

Section "WinArchiveFSPro"
  SetRegView ${PRODUCT_REGVIEW}
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
  IfFileExists "$INSTDIR\app\WinArchiveFSPro.Unc.exe" 0 +2
    ExecWait '"$INSTDIR\app\WinArchiveFSPro.Unc.exe" --stop'
  SetRegView ${PRODUCT_REGVIEW}
  SetShellVarContext all
  SetOutPath "$INSTDIR\app"
  File /r "${DIST_DIR}\*"
  ExecWait '"$INSTDIR\app\archivefs.exe" migrate-settings' $1
  ${If} $1 != 0
    DetailPrint "User settings will be imported when the application starts."
  ${EndIf}
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\WinArchiveFSPro"
  CreateShortCut "$SMPROGRAMS\WinArchiveFSPro\WinArchiveFSPro.lnk" "$INSTDIR\app\WinArchiveFSPro.exe"
  Delete "$SMPROGRAMS\WinArchiveFSPro\Uninstall.lnk"
  Delete "$SMPROGRAMS\WinArchiveFSPro\卸载.lnk"
  CreateShortCut "$SMPROGRAMS\WinArchiveFSPro\$(UninstallLink).lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\WinArchiveFSPro" "InstallDir" "$INSTDIR"
  WriteRegDWORD HKLM "Software\WinArchiveFSPro" "InstallerLanguage" $LANGUAGE
  WriteRegStr HKLM "Software\WinArchiveFSPro\Capabilities" "ApplicationName" "WinArchiveFSPro"
  WriteRegStr HKLM "Software\WinArchiveFSPro\Capabilities" "ApplicationIcon" '"$INSTDIR\app\WinArchiveFSPro.exe",0'
  WriteRegStr HKLM "Software\WinArchiveFSPro\Capabilities" "ApplicationDescription" "$(Description)"
  WriteRegStr HKLM "Software\RegisteredApplications" "WinArchiveFSPro" "Software\WinArchiveFSPro\Capabilities"
  WriteRegStr HKLM "Software\Classes\${PROGID}" "" "$(ImageType)"
  WriteRegStr HKLM "Software\Classes\${PROGID}\DefaultIcon" "" '"$INSTDIR\app\Assets\file-icon.ico",0'
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell" "" "open"
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\app\WinArchiveFSPro.exe" --unc "%1"'
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\edit" "" "$(EditVerb)"
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\edit\command" "" '"$INSTDIR\app\WinArchiveFSPro.exe" --open "%1"'
  !insertmacro RegisterExtension ".squashfs" 1
  !insertmacro RegisterExtension ".sqfs" 1
  !insertmacro RegisterExtension ".sfs" 1
  !insertmacro RegisterExtension ".dwarfs" 1
  !insertmacro RegisterExtension ".erofs" 1
  !insertmacro RegisterExtension ".zip" 0
  !insertmacro RegisterExtension ".zipx" 0
  !insertmacro RegisterExtension ".7z" 0
  !insertmacro RegisterExtension ".rar" 0
  !insertmacro RegisterExtension ".tar" 0
  !insertmacro RegisterExtension ".tgz" 0
  !insertmacro RegisterExtension ".tbz" 0
  !insertmacro RegisterExtension ".tbz2" 0
  !insertmacro RegisterExtension ".txz" 0
  !insertmacro RegisterExtension ".tzst" 0
  !insertmacro RegisterExtension ".tlz" 0
  !insertmacro RegisterExtension ".gz" 0
  !insertmacro RegisterExtension ".bz2" 0
  !insertmacro RegisterExtension ".xz" 0
  !insertmacro RegisterExtension ".zst" 0
  !insertmacro RegisterExtension ".lz4" 0
  !insertmacro RegisterExtension ".lz" 0
  !insertmacro RegisterExtension ".lzma" 0
  !insertmacro RegisterExtension ".z" 0
  !insertmacro RegisterExtension ".cpio" 0
  !insertmacro RegisterExtension ".cab" 0
  !insertmacro RegisterExtension ".iso" 0
  !insertmacro RegisterExtension ".ar" 0
  !insertmacro RegisterExtension ".deb" 0
  !insertmacro RegisterExtension ".lha" 0
  !insertmacro RegisterExtension ".lzh" 0
  DeleteRegKey HKLM "Software\Classes\Drive\shell\WinArchiveFSPro.Eject"
  WriteRegStr HKLM "Software\Classes\CLSID\${EJECT_CLSID}" "" "WinArchiveFSPro Eject"
  WriteRegStr HKLM "Software\Classes\CLSID\${EJECT_CLSID}\InprocServer32" "" "$INSTDIR\app\winsqfs-shell-${PRODUCT_VERSION}.dll"
  WriteRegStr HKLM "Software\Classes\CLSID\${EJECT_CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  WriteRegStr HKLM "Software\Classes\Drive\shellex\ContextMenuHandlers\WinArchiveFSPro.Eject" "" "${EJECT_CLSID}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${EJECT_CLSID}" "WinArchiveFSPro Eject"
  DeleteRegKey HKLM "Software\Classes\Directory\shell\WinArchiveFSPro.Pack"
  WriteRegStr HKLM "Software\Classes\CLSID\${PACK_CLSID}" "" "WinArchiveFSPro Create Archive"
  WriteRegStr HKLM "Software\Classes\CLSID\${PACK_CLSID}\InprocServer32" "" "$INSTDIR\app\winsqfs-shell-${PRODUCT_VERSION}.dll"
  WriteRegStr HKLM "Software\Classes\CLSID\${PACK_CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  WriteRegStr HKLM "Software\Classes\AllFilesystemObjects\shellex\ContextMenuHandlers\WinArchiveFSPro.Pack" "" "${PACK_CLSID}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${PACK_CLSID}" "WinArchiveFSPro Create Archive"
  WriteRegStr HKLM "Software\Classes\CLSID\${PASTE_CLSID}" "" "WinArchiveFSPro Server Paste"
  WriteRegStr HKLM "Software\Classes\CLSID\${PASTE_CLSID}\InprocServer32" "" "$INSTDIR\app\winsqfs-shell-${PRODUCT_VERSION}.dll"
  WriteRegStr HKLM "Software\Classes\CLSID\${PASTE_CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  WriteRegStr HKLM "Software\Classes\Directory\Background\shellex\ContextMenuHandlers\WinArchiveFSPro.ServerPaste" "" "${PASTE_CLSID}"
  WriteRegStr HKLM "Software\Classes\Directory\shellex\ContextMenuHandlers\WinArchiveFSPro.ServerPaste" "" "${PASTE_CLSID}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${PASTE_CLSID}" "WinArchiveFSPro Server Paste"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "WinArchiveFSPro"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\app\WinArchiveFSPro.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Function un.onInit
  SetRegView ${PRODUCT_REGVIEW}
  ReadRegDWORD $0 HKLM "Software\WinArchiveFSPro" "InstallerLanguage"
  ${If} $0 == 2052
    StrCpy $LANGUAGE 2052
  ${Else}
    StrCpy $LANGUAGE 1033
  ${EndIf}
FunctionEnd

Section "Uninstall"
  IfFileExists "$INSTDIR\app\WinArchiveFSPro.Unc.exe" 0 +2
    ExecWait '"$INSTDIR\app\WinArchiveFSPro.Unc.exe" --stop'
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WinArchiveFSProUNC"
  ${If} $0 == '"$INSTDIR\app\WinArchiveFSPro.Unc.exe"'
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WinArchiveFSProUNC"
  ${EndIf}
  SetRegView ${PRODUCT_REGVIEW}
  SetShellVarContext all
  !insertmacro UnregisterExtension ".squashfs"
  !insertmacro UnregisterExtension ".sqfs"
  !insertmacro UnregisterExtension ".sfs"
  !insertmacro UnregisterExtension ".dwarfs"
  !insertmacro UnregisterExtension ".erofs"
  !insertmacro UnregisterExtension ".zip"
  !insertmacro UnregisterExtension ".zipx"
  !insertmacro UnregisterExtension ".7z"
  !insertmacro UnregisterExtension ".rar"
  !insertmacro UnregisterExtension ".tar"
  !insertmacro UnregisterExtension ".tgz"
  !insertmacro UnregisterExtension ".tbz"
  !insertmacro UnregisterExtension ".tbz2"
  !insertmacro UnregisterExtension ".txz"
  !insertmacro UnregisterExtension ".tzst"
  !insertmacro UnregisterExtension ".tlz"
  !insertmacro UnregisterExtension ".gz"
  !insertmacro UnregisterExtension ".bz2"
  !insertmacro UnregisterExtension ".xz"
  !insertmacro UnregisterExtension ".zst"
  !insertmacro UnregisterExtension ".lz4"
  !insertmacro UnregisterExtension ".lz"
  !insertmacro UnregisterExtension ".lzma"
  !insertmacro UnregisterExtension ".z"
  !insertmacro UnregisterExtension ".cpio"
  !insertmacro UnregisterExtension ".cab"
  !insertmacro UnregisterExtension ".iso"
  !insertmacro UnregisterExtension ".ar"
  !insertmacro UnregisterExtension ".deb"
  !insertmacro UnregisterExtension ".lha"
  !insertmacro UnregisterExtension ".lzh"
  DeleteRegKey HKLM "Software\Classes\${PROGID}"
  DeleteRegKey HKLM "Software\Classes\Drive\shell\WinArchiveFSPro.Eject"
  DeleteRegKey HKLM "Software\Classes\Drive\shellex\ContextMenuHandlers\WinArchiveFSPro.Eject"
  DeleteRegKey HKLM "Software\Classes\CLSID\${EJECT_CLSID}"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${EJECT_CLSID}"
  DeleteRegKey HKLM "Software\Classes\Directory\shell\WinArchiveFSPro.Pack"
  DeleteRegKey HKLM "Software\Classes\AllFilesystemObjects\shellex\ContextMenuHandlers\WinArchiveFSPro.Pack"
  DeleteRegKey HKLM "Software\Classes\CLSID\${PACK_CLSID}"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${PACK_CLSID}"
  DeleteRegKey HKLM "Software\Classes\Directory\Background\shellex\ContextMenuHandlers\WinArchiveFSPro.ServerPaste"
  DeleteRegKey HKLM "Software\Classes\Directory\shellex\ContextMenuHandlers\WinArchiveFSPro.ServerPaste"
  DeleteRegKey HKLM "Software\Classes\CLSID\${PASTE_CLSID}"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${PASTE_CLSID}"
  DeleteRegValue HKLM "Software\RegisteredApplications" "WinArchiveFSPro"
  DeleteRegKey HKLM "Software\WinArchiveFSPro"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  RMDir /r "$SMPROGRAMS\WinArchiveFSPro"
  RMDir /r /REBOOTOK "$INSTDIR\app"
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
