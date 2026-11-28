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
VIProductVersion "${PRODUCT_VERSION}.0"
!define PROGID "WinSquashfs.Image"
!define EJECT_CLSID "{9B20352F-1BC4-48E6-AE3B-146F32657DA9}"
!define PACK_CLSID "{57AB2C11-7881-46EB-9AF8-33194EDF6071}"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\WinSquashfs"
Name "WinSquashfs"
OutFile "..\artifacts\WinSquashfs-Setup-x64.exe"
InstallDir "$PROGRAMFILES64\WinSquashfs"
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

VIAddVersionKey /LANG=1033 "ProductName" "WinSquashfs"
VIAddVersionKey /LANG=1033 "FileDescription" "WinSquashfs Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "ProductName" "WinSquashfs"
VIAddVersionKey /LANG=2052 "FileDescription" "WinSquashfs 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${PRODUCT_VERSION}"

LangString BrowseVerb ${LANG_ENGLISH} "Browse with WinSquashfs"
LangString BrowseVerb ${LANG_SIMPCHINESE} "使用 WinSquashfs 浏览"
LangString MountVerb ${LANG_ENGLISH} "Mount with WinSquashfs"
LangString MountVerb ${LANG_SIMPCHINESE} "使用 WinSquashfs 挂载"
LangString ExtractVerb ${LANG_ENGLISH} "Extract with WinSquashfs"
LangString ExtractVerb ${LANG_SIMPCHINESE} "使用 WinSquashfs 提取"
LangString UncVerb ${LANG_ENGLISH} "Open via SquashFS UNC path"
LangString UncVerb ${LANG_SIMPCHINESE} "通过 SquashFS UNC 路径打开"
LangString PackVerb ${LANG_ENGLISH} "Create SquashFS image"
LangString PackVerb ${LANG_SIMPCHINESE} "创建 SquashFS 镜像"
LangString Description ${LANG_ENGLISH} "Browse, create and mount SquashFS, DwarFS and EROFS images."
LangString Description ${LANG_SIMPCHINESE} "浏览、创建和挂载 SquashFS、DwarFS 及 EROFS 镜像。"
LangString ImageType ${LANG_ENGLISH} "Read-only filesystem image"
LangString ImageType ${LANG_SIMPCHINESE} "只读文件系统镜像"
LangString EditVerb ${LANG_ENGLISH} "Edit with WinSquashfs"
LangString EditVerb ${LANG_SIMPCHINESE} "使用 WinSquashfs 编辑"
LangString UninstallLink ${LANG_ENGLISH} "Uninstall"
LangString UninstallLink ${LANG_SIMPCHINESE} "卸载"
LangString DriverError ${LANG_ENGLISH} "WinFsp installation failed (exit code $1)."
LangString DriverError ${LANG_SIMPCHINESE} "WinFsp 安装失败（退出代码：$1）。"
LangString DriverIncompatible ${LANG_ENGLISH} "Installed WinFsp $4 is incompatible with this release. WinFsp ${WINFSP_MIN_VERSION} or a later 2.x release is required."
LangString DriverIncompatible ${LANG_SIMPCHINESE} "当前安装的 WinFsp $4 与本版本不兼容。需要 WinFsp ${WINFSP_MIN_VERSION} 或更新的 2.x 版本。"
LangString DriverMissing ${LANG_ENGLISH} "WinFsp installation did not provide a compatible runtime. Restart Windows if required, then reinstall WinSquashfs."
LangString DriverMissing ${LANG_SIMPCHINESE} "WinFsp 安装后仍缺少兼容的运行时。请按需重新启动 Windows，再重新安装 WinSquashfs。"
LangString DriverRestart ${LANG_ENGLISH} "Restart Windows before mounting images or starting UNC access."
LangString DriverRestart ${LANG_SIMPCHINESE} "请重新启动 Windows 后再挂载镜像或启动 UNC 访问。"

Function .onInit
  SetRegView 64
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
  ReadRegStr $0 HKLM "Software\WinSquashfs" "InstallDir"
  ${If} $0 != ""
    StrCpy $INSTDIR $0
  ${EndIf}
FunctionEnd

!macro RegisterExtension EXT
  ReadRegStr $0 HKLM "Software\Classes\${EXT}" ""
  ${If} $0 == ""
    WriteRegStr HKLM "Software\Classes\${EXT}" "" "${PROGID}"
  ${EndIf}
  WriteRegStr HKLM "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}" ""
  WriteRegStr HKLM "Software\WinSquashfs\Capabilities\FileAssociations" "${EXT}" "${PROGID}"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Browse" "" "$(BrowseVerb)"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Browse\command" "" '"$INSTDIR\app\WinSquashfs.exe" --open "%1"'
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Mount" "" "$(MountVerb)"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Mount\command" "" '"$INSTDIR\app\WinSquashfs.exe" --mount "%1"'
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Extract" "" "$(ExtractVerb)"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Extract\command" "" '"$INSTDIR\app\WinSquashfs.exe" --extract "%1"'
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Unc" "" "$(UncVerb)"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Unc\command" "" '"$INSTDIR\app\WinSquashfs.exe" --unc "%1"'
!macroend
!macro UnregisterExtension EXT
  ReadRegStr $0 HKLM "Software\Classes\${EXT}" ""
  ${If} $0 == "${PROGID}"
    DeleteRegValue HKLM "Software\Classes\${EXT}" ""
  ${EndIf}
  DeleteRegValue HKLM "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}"
  DeleteRegKey HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Browse"
  DeleteRegKey HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Mount"
  DeleteRegKey HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Extract"
  DeleteRegKey HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Unc"
!macroend

Function ReadWinFspVersion
  SetRegView 32
  ReadRegStr $0 HKLM "SOFTWARE\WinFsp" "InstallDir"
  SetRegView 64
  StrCpy $4 ""
  StrCpy $5 0
  ${If} $0 != ""
    ClearErrors
    GetDLLVersion "$0\bin\winfsp-x64.dll" $2 $3
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
  ExecWait 'msiexec.exe /i "$PLUGINSDIR\winfsp.msi" /qn /norestart /L*v "$TEMP\WinSquashfs-WinFsp-install.log"' $1
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

Section "WinSquashfs"
  IfFileExists "$INSTDIR\app\WinSquashfs.Unc.exe" 0 +2
    ExecWait '"$INSTDIR\app\WinSquashfs.Unc.exe" --stop'
  SetRegView 64
  SetShellVarContext all
  SetOutPath "$INSTDIR\app"
  File /r "..\artifacts\dist\*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\WinSquashfs"
  CreateShortCut "$SMPROGRAMS\WinSquashfs\WinSquashfs.lnk" "$INSTDIR\app\WinSquashfs.exe"
  Delete "$SMPROGRAMS\WinSquashfs\Uninstall.lnk"
  Delete "$SMPROGRAMS\WinSquashfs\卸载.lnk"
  CreateShortCut "$SMPROGRAMS\WinSquashfs\$(UninstallLink).lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\WinSquashfs" "InstallDir" "$INSTDIR"
  WriteRegDWORD HKLM "Software\WinSquashfs" "InstallerLanguage" $LANGUAGE
  WriteRegStr HKLM "Software\WinSquashfs\Capabilities" "ApplicationName" "WinSquashfs"
  WriteRegStr HKLM "Software\WinSquashfs\Capabilities" "ApplicationIcon" '"$INSTDIR\app\WinSquashfs.exe",0'
  WriteRegStr HKLM "Software\WinSquashfs\Capabilities" "ApplicationDescription" "$(Description)"
  WriteRegStr HKLM "Software\RegisteredApplications" "WinSquashfs" "Software\WinSquashfs\Capabilities"
  WriteRegStr HKLM "Software\Classes\${PROGID}" "" "$(ImageType)"
  WriteRegStr HKLM "Software\Classes\${PROGID}\DefaultIcon" "" '"$INSTDIR\app\Assets\file-icon.ico",0'
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell" "" "open"
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\app\WinSquashfs.exe" --unc "%1"'
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\edit" "" "$(EditVerb)"
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\edit\command" "" '"$INSTDIR\app\WinSquashfs.exe" --open "%1"'
  !insertmacro RegisterExtension ".squashfs"
  !insertmacro RegisterExtension ".sqfs"
  !insertmacro RegisterExtension ".sfs"
  !insertmacro RegisterExtension ".dwarfs"
  !insertmacro RegisterExtension ".erofs"
  DeleteRegKey HKLM "Software\Classes\Drive\shell\WinSquashfs.Eject"
  WriteRegStr HKLM "Software\Classes\CLSID\${EJECT_CLSID}" "" "WinSquashfs Eject"
  WriteRegStr HKLM "Software\Classes\CLSID\${EJECT_CLSID}\InprocServer32" "" "$INSTDIR\app\winsqfs-shell-${PRODUCT_VERSION}.dll"
  WriteRegStr HKLM "Software\Classes\CLSID\${EJECT_CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  WriteRegStr HKLM "Software\Classes\Drive\shellex\ContextMenuHandlers\WinSquashfs.Eject" "" "${EJECT_CLSID}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${EJECT_CLSID}" "WinSquashfs Eject"
  DeleteRegKey HKLM "Software\Classes\Directory\shell\WinSquashfs.Pack"
  WriteRegStr HKLM "Software\Classes\CLSID\${PACK_CLSID}" "" "WinSquashfs Create Archive"
  WriteRegStr HKLM "Software\Classes\CLSID\${PACK_CLSID}\InprocServer32" "" "$INSTDIR\app\winsqfs-shell-${PRODUCT_VERSION}.dll"
  WriteRegStr HKLM "Software\Classes\CLSID\${PACK_CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  WriteRegStr HKLM "Software\Classes\AllFilesystemObjects\shellex\ContextMenuHandlers\WinSquashfs.Pack" "" "${PACK_CLSID}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${PACK_CLSID}" "WinSquashfs Create Archive"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "WinSquashfs"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\app\WinSquashfs.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Function un.onInit
  SetRegView 64
  ReadRegDWORD $0 HKLM "Software\WinSquashfs" "InstallerLanguage"
  ${If} $0 == 2052
    StrCpy $LANGUAGE 2052
  ${Else}
    StrCpy $LANGUAGE 1033
  ${EndIf}
FunctionEnd

Section "Uninstall"
  IfFileExists "$INSTDIR\app\WinSquashfs.Unc.exe" 0 +2
    ExecWait '"$INSTDIR\app\WinSquashfs.Unc.exe" --stop'
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WinSquashfsUNC"
  ${If} $0 == '"$INSTDIR\app\WinSquashfs.Unc.exe"'
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WinSquashfsUNC"
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  !insertmacro UnregisterExtension ".squashfs"
  !insertmacro UnregisterExtension ".sqfs"
  !insertmacro UnregisterExtension ".sfs"
  !insertmacro UnregisterExtension ".dwarfs"
  !insertmacro UnregisterExtension ".erofs"
  DeleteRegKey HKLM "Software\Classes\${PROGID}"
  DeleteRegKey HKLM "Software\Classes\Drive\shell\WinSquashfs.Eject"
  DeleteRegKey HKLM "Software\Classes\Drive\shellex\ContextMenuHandlers\WinSquashfs.Eject"
  DeleteRegKey HKLM "Software\Classes\CLSID\${EJECT_CLSID}"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${EJECT_CLSID}"
  DeleteRegKey HKLM "Software\Classes\Directory\shell\WinSquashfs.Pack"
  DeleteRegKey HKLM "Software\Classes\AllFilesystemObjects\shellex\ContextMenuHandlers\WinSquashfs.Pack"
  DeleteRegKey HKLM "Software\Classes\CLSID\${PACK_CLSID}"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${PACK_CLSID}"
  DeleteRegValue HKLM "Software\RegisteredApplications" "WinSquashfs"
  DeleteRegKey HKLM "Software\WinSquashfs"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  RMDir /r "$SMPROGRAMS\WinSquashfs"
  RMDir /r /REBOOTOK "$INSTDIR\app"
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
