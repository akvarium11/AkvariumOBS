; AkvariumOBS installer (NSIS)
; Build: makensis /DSTAGE=<path to staged AkvariumOBS folder> /DVERSION=32.2.2 packaging/installer.nsi

Unicode true
ManifestDPIAware true

!ifndef VERSION
  !define VERSION "32.2.2"
!endif
!ifndef STAGE
  !error "STAGE must point to the staged AkvariumOBS folder"
!endif

!define APPNAME "AkvariumOBS"
!define PUBLISHER "AkvariumOBS"
!define APPEXE "AkvariumOBS.exe"
!define REGKEY "Software\AkvariumOBS"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\AkvariumOBS"

Name "${APPNAME} ${VERSION}"
OutFile "AkvariumOBS-${VERSION}-Windows-x64-Installer.exe"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "${REGKEY}" "InstallLocation"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetCompressorDictSize 64
BrandingText "${APPNAME} ${VERSION}"

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!define MUI_ICON "..\frontend\cmake\windows\obs-studio.ico"
!define MUI_UNICON "..\frontend\cmake\windows\obs-studio.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\64bit\${APPEXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APPNAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\COPYING"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Russian"

; ---------------------------------------------------------------- helpers

!macro CreateShortcut link target icon
  CreateShortCut "${link}" "${target}" "" "${icon}" 0
!macroend

; ---------------------------------------------------------------- sections

Section "!AkvariumOBS (required)" SecCore
  SectionIn RO
  SetOutPath "$INSTDIR"
  SetOverwrite on

  File /r "${STAGE}\bin"
  File /r "${STAGE}\data"
  File /r "${STAGE}\obs-plugins"
  File "${STAGE}\COPYING"
  File /r "${STAGE}\README.txt"

  WriteRegStr HKLM "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${REGKEY}" "Version" "${VERSION}"

  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\bin\64bit\${APPEXE}"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1

  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Start Menu shortcuts" SecStartMenu
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  !insertmacro CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\bin\64bit\${APPEXE}" "$INSTDIR\bin\64bit\${APPEXE}"
  !insertmacro CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\uninstall.exe" "$INSTDIR\uninstall.exe"
SectionEnd

Section "Desktop shortcut" SecDesktop
  !insertmacro CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\bin\64bit\${APPEXE}" "$INSTDIR\bin\64bit\${APPEXE}"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "Application files, plugins, data and the speech recognition model."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Create Start Menu entries."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Create a desktop shortcut."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------- uninstall

Section "Uninstall"
  SetOutPath "$TEMP"
  Delete "$DESKTOP\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"

  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "${REGKEY}"

  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\data"
  RMDir /r "$INSTDIR\obs-plugins"
  RMDir "$INSTDIR\bin\64bit"
  RMDir "$INSTDIR\bin"
  RMDir "$INSTDIR\data"
  RMDir "$INSTDIR\obs-plugins"
  Delete "$INSTDIR\COPYING"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  ; User settings live outside the install directory and are kept on purpose.
SectionEnd

Function .onInit
  ; Reuse the previous install location if the user reinstalls elsewhere.
  ReadRegStr $0 HKLM "${REGKEY}" "InstallLocation"
  ${If} $0 != ""
  ${AndIf} $0 != "$INSTDIR"
    StrCpy $INSTDIR $0
  ${EndIf}
FunctionEnd
