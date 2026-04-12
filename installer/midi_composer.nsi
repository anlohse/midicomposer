; Windows installer for MIDI Composer (NSIS 3).
;
; The release build produces exactly two files: the executable and ui.pak, the
; packed UI it serves to its webview. The C runtime is linked statically (see
; core/CMakeLists.txt), so there is no redistributable to carry and nothing to
; bootstrap here except the WebView2 runtime.
;
; Normally invoked through the build:
;   cmake --build build-release --config Release --target midi_composer_installer
;
; By hand:
;   makensis /DSOURCE_DIR=..\core\build-release\Release /DAPP_VERSION=0.1.0 midi_composer.nsi

!ifndef SOURCE_DIR
  !define SOURCE_DIR "..\core\build-release\Release"
!endif
!ifndef APP_VERSION
  !define APP_VERSION "0.0.0"
!endif
!ifndef OUTPUT_DIR
  !define OUTPUT_DIR "output"
!endif

!define APP_NAME      "MIDI Composer"
!define APP_EXE       "midi_composer_app.exe"
!define APP_PUBLISHER "MIDI Composer"
!define UNINST_KEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\MIDIComposer"

; ── Install mode ─────────────────────────────────────────────────────────────
;
; MultiUser offers "everyone" or "just me". A self-contained 1.4 MB application
; has no business demanding administrator rights, and a per-user install needs
; none; it also keeps the uninstall entry and the shortcuts in the matching hive.

!define MULTIUSER_EXECUTIONLEVEL Highest
!define MULTIUSER_MUI
!define MULTIUSER_INSTALLMODE_COMMANDLINE
!define MULTIUSER_USE_PROGRAMFILES64
!define MULTIUSER_INSTALLMODE_INSTDIR "${APP_NAME}"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_KEY "${UNINST_KEY}"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_VALUENAME "InstallLocation"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_KEY "${UNINST_KEY}"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_VALUENAME "UninstallString"

!include MultiUser.nsh
!include MUI2.nsh
!include x64.nsh
!include LogicLib.nsh
!include FileFunc.nsh

Name "${APP_NAME}"
OutFile "${OUTPUT_DIR}\MIDIComposer-${APP_VERSION}-setup.exe"
Unicode true
SetCompressor /SOLID lzma

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName"     "${APP_NAME}"
VIAddVersionKey "ProductVersion"  "${APP_VERSION}"
VIAddVersionKey "FileVersion"     "${APP_VERSION}"
VIAddVersionKey "CompanyName"     "${APP_PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey "LegalCopyright"  "${APP_PUBLISHER}"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MULTIUSER_PAGE_INSTALLMODE
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "PortugueseBR"

; ── WebView2 runtime ─────────────────────────────────────────────────────────
;
; The UI is a webview, so without the Evergreen runtime the application opens a
; window and nothing else — no error, no hint. It ships with Windows 11 and with
; current Windows 10, but that cannot be assumed.
;
; The Evergreen client registers under a fixed GUID, machine-wide in the 32-bit
; registry view even on x64, or per-user in the native view.

!define WV2_GUID "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
!define WV2_BOOTSTRAPPER_URL "https://go.microsoft.com/fwlink/p/?LinkId=2124703"

; Sets $0 to a non-empty version string when the runtime is present.
Function DetectWebView2
    SetRegView 32
    ReadRegStr $0 HKLM "SOFTWARE\Microsoft\EdgeUpdate\Clients\${WV2_GUID}" "pv"
    SetRegView lastused
    ${If} $0 == ""
        ReadRegStr $0 HKCU "SOFTWARE\Microsoft\EdgeUpdate\Clients\${WV2_GUID}" "pv"
    ${EndIf}
    ${If} $0 == "0.0.0.0"
        StrCpy $0 ""
    ${EndIf}
FunctionEnd

Function InstallWebView2
    ; Downloaded rather than bundled: the bootstrapper is a stub that always
    ; pulls the current runtime, so a copy frozen into this installer would go
    ; stale. curl ships with Windows 10 1803 and later, which avoids depending
    ; on an NSIS download plugin that is not part of a stock install.
    DetailPrint "Downloading the Microsoft Edge WebView2 runtime..."
    nsExec::ExecToLog '"$SYSDIR\curl.exe" -L -f -s -S --retry 2 -o "$PLUGINSDIR\webview2.exe" "${WV2_BOOTSTRAPPER_URL}"'
    Pop $1
    ${If} $1 != 0
    ${OrIfNot} ${FileExists} "$PLUGINSDIR\webview2.exe"
        MessageBox MB_OK|MB_ICONEXCLAMATION \
            "The WebView2 runtime could not be downloaded.$\n$\n\
             ${APP_NAME} is installed, but will not display its interface until the \
             runtime is present. Install it from:$\n\
             https://developer.microsoft.com/microsoft-edge/webview2/"
        Return
    ${EndIf}

    DetailPrint "Installing the Microsoft Edge WebView2 runtime..."
    ; The bootstrapper picks per-machine or per-user by the rights it has, which
    ; matches however this installer was started.
    nsExec::ExecToLog '"$PLUGINSDIR\webview2.exe" /silent /install'
    Pop $1
    ${If} $1 != 0
        MessageBox MB_OK|MB_ICONEXCLAMATION \
            "The WebView2 runtime installer reported an error ($1).$\n$\n\
             ${APP_NAME} will not display its interface until the runtime is present."
    ${EndIf}
FunctionEnd

; ── Running instance ─────────────────────────────────────────────────────────
;
; Overwriting a locked executable otherwise ends in NSIS's bare retry dialog.
; Asked rather than killed: the running copy may hold unsaved work.
Function EnsureNotRunning
    retry:
    nsExec::ExecToStack 'cmd /c tasklist /FI "IMAGENAME eq ${APP_EXE}" /NH | find /I "${APP_EXE}"'
    Pop $0
    Pop $1
    ${If} $0 == 0
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
            "${APP_NAME} is running. Close it and try again." IDRETRY retry
        Abort
    ${EndIf}
FunctionEnd

Function .onInit
    ; The webview is 64-bit only, so refuse rather than install something that
    ; cannot run.
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} requires 64-bit Windows."
        Abort
    ${EndIf}

    ; MULTIUSER_INIT overwrites $INSTDIR with the location for the chosen install
    ; mode, which silently discards a /D= given on the command line. Only /D can
    ; have set it at this point, so put it back afterwards.
    StrCpy $R9 "$INSTDIR"
    !insertmacro MULTIUSER_INIT
    ${If} $R9 != ""
        StrCpy $INSTDIR "$R9"
    ${EndIf}

    Call EnsureNotRunning
FunctionEnd

Function un.onInit
    !insertmacro MULTIUSER_UNINIT
FunctionEnd

Section "Install"
    SetOutPath "$INSTDIR"

    File "${SOURCE_DIR}\${APP_EXE}"
    ; Without this the application starts and reports that it has no UI.
    File "${SOURCE_DIR}\ui.pak"

    CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"

    WriteUninstaller "$INSTDIR\uninstall.exe"

    ; SHCTX is HKLM or HKCU, whichever install mode was chosen.
    WriteRegStr   SHCTX "${UNINST_KEY}" "DisplayName"     "${APP_NAME}"
    WriteRegStr   SHCTX "${UNINST_KEY}" "DisplayVersion"  "${APP_VERSION}"
    WriteRegStr   SHCTX "${UNINST_KEY}" "Publisher"       "${APP_PUBLISHER}"
    WriteRegStr   SHCTX "${UNINST_KEY}" "DisplayIcon"     "$INSTDIR\${APP_EXE}"
    WriteRegStr   SHCTX "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr   SHCTX "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr   SHCTX "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD SHCTX "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD SHCTX "${UNINST_KEY}" "NoRepair" 1
    ; Reported in Apps & Features, in KB.
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD SHCTX "${UNINST_KEY}" "EstimatedSize" $0

    Call DetectWebView2
    ${If} $0 == ""
        Call InstallWebView2
    ${Else}
        DetailPrint "WebView2 runtime found (version $0)"
    ${EndIf}
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\${APP_EXE}"
    Delete "$INSTDIR\ui.pak"
    Delete "$INSTDIR\uninstall.exe"
    ; Only if empty: anything the user put here is theirs.
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\${APP_NAME}.lnk"

    DeleteRegKey SHCTX "${UNINST_KEY}"
SectionEnd
