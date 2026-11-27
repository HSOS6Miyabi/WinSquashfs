Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!define PROGID "WinSquashfs.Image"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\WinSquashfs"
Name "WinSquashfs"
OutFile "..\artifacts\WinSquashfs-Setup-x64.exe"
InstallDir "$PROGRAMFILES64\WinSquashfs"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetRegView 64
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
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Browse" "" "Browse with WinSquashfs"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Browse\command" "" '"$INSTDIR\app\WinSquashfs.exe" --open "%1"'
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Mount" "" "Mount with WinSquashfs"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Mount\command" "" '"$INSTDIR\app\WinSquashfs.exe" --mount "%1"'
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Extract" "" "Extract with WinSquashfs"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Extract\command" "" '"$INSTDIR\app\WinSquashfs.exe" --extract "%1"'
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\shell\WinSquashfs.Unc" "" "Open via SquashFS UNC path"
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

Section "WinFsp runtime"
  SetRegView 32
  ReadRegStr $0 HKLM "SOFTWARE\WinFsp" "InstallDir"
  SetRegView 64
  ${If} $0 != ""
    ${If} ${FileExists} "$0\bin\winfsp-x64.dll"
      Goto winfsp_done
    ${EndIf}
  ${EndIf}
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File /oname=winfsp.msi "..\artifacts\winfsp.msi"
  ExecWait 'msiexec.exe /i "$PLUGINSDIR\winfsp.msi" /qn /norestart ADDLOCAL=ALL' $1
  ${If} $1 == 3010
    SetRebootFlag true
  ${ElseIf} $1 != 0
    MessageBox MB_ICONSTOP "WinFsp installation failed (exit code $1)."
    Abort
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
  CreateShortCut "$SMPROGRAMS\WinSquashfs\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\WinSquashfs" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\WinSquashfs\Capabilities" "ApplicationName" "WinSquashfs"
  WriteRegStr HKLM "Software\WinSquashfs\Capabilities" "ApplicationDescription" "Browse, create and mount SquashFS images."
  WriteRegStr HKLM "Software\RegisteredApplications" "WinSquashfs" "Software\WinSquashfs\Capabilities"
  WriteRegStr HKLM "Software\Classes\${PROGID}" "" "SquashFS image"
  WriteRegStr HKLM "Software\Classes\${PROGID}\DefaultIcon" "" "$INSTDIR\app\WinSquashfs.exe,0"
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\app\WinSquashfs.exe" --open "%1"'
  !insertmacro RegisterExtension ".squashfs"
  !insertmacro RegisterExtension ".sqfs"
  !insertmacro RegisterExtension ".sfs"
  WriteRegStr HKLM "Software\Classes\Directory\shell\WinSquashfs.Pack" "" "Create SquashFS image"
  WriteRegStr HKLM "Software\Classes\Directory\shell\WinSquashfs.Pack\command" "" '"$INSTDIR\app\WinSquashfs.exe" --pack "%1"'
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "WinSquashfs"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "0.1.0"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\app\WinSquashfs.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

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
  DeleteRegKey HKLM "Software\Classes\${PROGID}"
  DeleteRegKey HKLM "Software\Classes\Directory\shell\WinSquashfs.Pack"
  DeleteRegValue HKLM "Software\RegisteredApplications" "WinSquashfs"
  DeleteRegKey HKLM "Software\WinSquashfs"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  RMDir /r "$SMPROGRAMS\WinSquashfs"
  RMDir /r /REBOOTOK "$INSTDIR\app"
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
