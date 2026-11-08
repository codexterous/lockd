Unicode true
ManifestDPIAware true
RequestExecutionLevel admin
SetCompressor /SOLID lzma
!include MUI2.nsh
!include nsDialogs.nsh
!include FileFunc.nsh
!include x64.nsh
!include WinVer.nsh

Name "lockd ${VERSION} (${ARCH})"
OutFile "${DESTINATION}"
!if "${ARCH}" != "x86"
InstallDir "$PROGRAMFILES64\lockd"
!else
InstallDir "$PROGRAMFILES32\lockd"
!endif
ShowInstDetails show
ShowUninstDetails show
!ifndef NUMERIC_VERSION
    !define NUMERIC_VERSION "${VERSION}"
!endif
VIProductVersion "${NUMERIC_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "lockd"
VIAddVersionKey /LANG=1033 "FileDescription" "lockd ${ARCH} installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (C) 2026 Brett Gmoser <lockd@codexterous.com>"
!define REGISTRY "Software\Microsoft\Windows\CurrentVersion\Uninstall\lockd-${ARCH}"

Var Powershell
Var ExistingService
Var Service
Var ServiceName
Var Address
Var Port
Var Threads
Var Level
Var Log
Var EventLog
Var Automatic
Var Start
Var Firewall
Var FirewallAvailable
Var Dialog
Var ServiceControl
Var NameLabel
Var AddressLabel
Var PortLabel
Var ThreadsLabel
Var ThreadsHint
Var NameControl
Var AddressControl
Var PortControl
Var ThreadsControl
Var LevelControl
Var LogControl
Var EventControl
Var AutomaticControl
Var StartControl
Var FirewallControl
Var RemoveLogs
Var DiagnosticLog

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PAYLOAD}\license.txt"
!insertmacro MUI_PAGE_DIRECTORY
Page custom ServicePage ServiceLeave
Page custom LoggingPage LoggingLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!macro RecordDiagnostic Text
    # Opt-in diagnostics let automated lifecycle tests retain silent failures. Each
    # helper replaces the previous entry, leaving the last operation and its message.
    ${If} $DiagnosticLog != ""
        Push $2
        ClearErrors
        FileOpen $2 "$DiagnosticLog" w
        ${IfNot} ${Errors}
            FileWriteUTF16LE /BOM $2 "${Text}$\r$\n"
            FileClose $2
        ${EndIf}
        Pop $2
    ${EndIf}
!macroend

!macro Runtime
    ReadEnvStr $DiagnosticLog "LOCKD_INSTALLER_DIAGNOSTIC_LOG"
    # Serialize upgrades and uninstalls so they cannot replace each other's files.
    # The process keeps this handle until exit.
    System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Global\lockd_installer") p .r0 ?e'
    Pop $1
    ${If} $0 == 0
    ${OrIf} $1 == 183
        !insertmacro RecordDiagnostic "Another lockd installer is running, or the installer lock could not be acquired."
        MessageBox MB_OK|MB_ICONSTOP "Another lockd installer is running, or the installer lock could not be acquired." /SD IDOK
        SetErrorLevel 1
        Quit
    ${EndIf}
    SetShellVarContext all
    StrCpy $Powershell "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"
!if "${ARCH}" == "arm64"
    # NSIS runs under x86 emulation, but the payload and PowerShell helper are native ARM64.
    # Check the OS architecture before deploying binaries that an x64 machine cannot run.
    ${IfNot} ${IsNativeARM64}
        !insertmacro RecordDiagnostic "This installer requires ARM64 Windows."
        MessageBox MB_OK|MB_ICONSTOP "This installer requires ARM64 Windows." /SD IDOK
        SetErrorLevel 1
        Quit
    ${EndIf}
!endif
!if "${ARCH}" != "x86"
    ${IfNot} ${RunningX64}
        !insertmacro RecordDiagnostic "This installer requires 64-bit Windows."
        MessageBox MB_OK|MB_ICONSTOP "This installer requires 64-bit Windows." /SD IDOK
        SetErrorLevel 1
        Quit
    ${EndIf}
    SetRegView 64
    StrCpy $Powershell "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
!else
    SetRegView 32
!endif
    InitPluginsDir
    SetOutPath "$PLUGINSDIR"
    File /oname=deploy.ps1 "deploy.ps1"
!macroend

!macro Option Name Variable
    ClearErrors
    ${GetOptions} $0 "/${Name}=" $1
    ${IfNot} ${Errors}
        StrCpy ${Variable} $1
    ${EndIf}
!macroend

Function .onInit
    !insertmacro Runtime
    ${IfNot} ${AtLeastWin10}
        !insertmacro RecordDiagnostic "lockd requires Windows 10 or later."
        MessageBox MB_OK|MB_ICONSTOP "lockd requires Windows 10 or later." /SD IDOK
        SetErrorLevel 1
        Quit
    ${EndIf}
    ReadRegStr $1 HKLM "${REGISTRY}" "InstallLocation"
    ${If} $1 != ""
        StrCpy $INSTDIR $1
    ${EndIf}
    ReadRegStr $ExistingService HKLM "${REGISTRY}" "ServiceName"
    StrCpy $Service 0
    StrCpy $ServiceName "lockd"
    StrCpy $Address "localhost"
    StrCpy $Port "7198"
    StrCpy $Threads ""
    StrCpy $Level "info"
    StrCpy $Log "$APPDATA\lockd\lockd.log"
    StrCpy $EventLog 1
    StrCpy $Automatic 1
    StrCpy $Start 1
    StrCpy $Firewall 0
    StrCpy $RemoveLogs 0
    ${GetParameters} $0
    !insertmacro Option Service $Service
    !insertmacro Option ServiceName $ServiceName
    !insertmacro Option Address $Address
    !insertmacro Option Port $Port
    !insertmacro Option Threads $Threads
    !insertmacro Option LogLevel $Level
    !insertmacro Option LogFile $Log
    !insertmacro Option EventLog $EventLog
    !insertmacro Option Automatic $Automatic
    !insertmacro Option Start $Start
    !insertmacro Option Firewall $Firewall
    FileOpen $0 "$PLUGINSDIR\options.ini" w
    FileWriteUTF16LE /BOM $0 "[Service]$\r$\n"
    FileClose $0
FunctionEnd

Function WriteOptions
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "Service" $Service
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "Name" $ServiceName
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "Address" $Address
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "Port" $Port
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "Threads" $Threads
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "Level" $Level
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "Log" $Log
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "EventLog" $EventLog
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "Automatic" $Automatic
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "Start" $Start
    WriteINIStr "$PLUGINSDIR\options.ini" "Service" "Firewall" $Firewall
FunctionEnd

!macro RunHelper Operation
!if "${Operation}" == "Uninstall"
helper_retry_${Operation}:
!else if "${Operation}" == "LogFiles"
helper_retry_${Operation}:
!endif
    nsExec::ExecToStack '"$Powershell" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\deploy.ps1" -Operation ${Operation} -Directory "$INSTDIR" -Architecture ${ARCH} -Payload "$PLUGINSDIR\payload" -Options "$PLUGINSDIR\options.ini" -Version ${VERSION} -RemoveLogs "$RemoveLogs"'
    Pop $0
    Pop $1
    DetailPrint $1
    !insertmacro RecordDiagnostic "${Operation}: helper exit $0$\r$\n$1"
    ${If} $0 != 0
!if "${Operation}" == "Uninstall"
        MessageBox MB_RETRYCANCEL|MB_ICONSTOP "$1$\r$\n$\r$\nResolve the problem, then choose Retry to continue cleanup. Cancel leaves the remaining installation available for a later uninstall." /SD IDCANCEL IDRETRY helper_retry_${Operation}
!else if "${Operation}" == "LogFiles"
        MessageBox MB_RETRYCANCEL|MB_ICONSTOP "$1" /SD IDCANCEL IDRETRY helper_retry_${Operation}
!else
        MessageBox MB_OK|MB_ICONSTOP "$1" /SD IDOK
!endif
        SetErrorLevel 1
        Abort
    ${EndIf}
!macroend

Function ServicePage
    !insertmacro MUI_HEADER_TEXT "Windows service" "Choose service settings before installation."
    nsDialogs::Create 1018
    Pop $Dialog
    ${If} $ExistingService != ""
        ${NSD_CreateLabel} 0 0 100% 50u "The existing service '$ExistingService' will keep its configuration and startup mode. A running service will be stopped during the upgrade and restarted afterward."
        Pop $0
        nsDialogs::Show
        Return
    ${EndIf}
    ${NSD_CreateCheckbox} 0 0 100% 12u "Install a Windows service (runs as LocalSystem)"
    Pop $ServiceControl
    ${NSD_SetState} $ServiceControl $Service
    ${NSD_OnClick} $ServiceControl ServiceSelectionChanged
    ${NSD_CreateLabel} 0 22u 85u 12u "Service name:"
    Pop $NameLabel
    ${NSD_CreateText} 90u 20u 200u 13u $ServiceName
    Pop $NameControl
    ${NSD_CreateLabel} 0 42u 85u 12u "Listening address:"
    Pop $AddressLabel
    ${NSD_CreateText} 90u 40u 200u 13u $Address
    Pop $AddressControl
    ${NSD_CreateLabel} 0 62u 85u 12u "TCP port:"
    Pop $PortLabel
    ${NSD_CreateText} 90u 60u 65u 13u $Port
    Pop $PortControl
    ${NSD_CreateLabel} 0 82u 85u 12u "Worker threads:"
    Pop $ThreadsLabel
    ${NSD_CreateText} 90u 80u 65u 13u $Threads
    Pop $ThreadsControl
    ${NSD_CreateLabel} 160u 82u 130u 12u "Empty = automatic (up to 8)"
    Pop $ThreadsHint
    ${NSD_CreateCheckbox} 0 105u 100% 12u "Start automatically with Windows"
    Pop $AutomaticControl
    ${NSD_SetState} $AutomaticControl $Automatic
    ${NSD_CreateCheckbox} 0 125u 100% 12u "Start the service after installation"
    Pop $StartControl
    ${NSD_SetState} $StartControl $Start
    Call UpdateServiceOptions
    nsDialogs::Show
FunctionEnd

Function ServiceSelectionChanged
    Pop $0
    Call UpdateServiceOptions
FunctionEnd

Function UpdateServiceOptions
    ${NSD_GetState} $ServiceControl $Service
    StrCpy $0 ${SW_HIDE}
    ${If} $Service == ${BST_CHECKED}
        StrCpy $0 ${SW_SHOW}
    ${EndIf}

    # Keep the controls and their values when toggling service installation.
    ShowWindow $NameLabel $0
    ShowWindow $NameControl $0
    ShowWindow $AddressLabel $0
    ShowWindow $AddressControl $0
    ShowWindow $PortLabel $0
    ShowWindow $PortControl $0
    ShowWindow $ThreadsLabel $0
    ShowWindow $ThreadsControl $0
    ShowWindow $ThreadsHint $0
    ShowWindow $AutomaticControl $0
    ShowWindow $StartControl $0
FunctionEnd

Function ServiceLeave
    ${If} $ExistingService == ""
        ${NSD_GetState} $ServiceControl $Service
        ${NSD_GetText} $NameControl $ServiceName
        ${NSD_GetText} $AddressControl $Address
        ${NSD_GetText} $PortControl $Port
        ${NSD_GetText} $ThreadsControl $Threads
        ${NSD_GetState} $AutomaticControl $Automatic
        ${NSD_GetState} $StartControl $Start
        # Classify once after editing the address, using the same IP parser as silent
        # installation. The next page cannot select a rule for a loopback listener.
        Call WriteOptions
        nsExec::ExecToStack '"$Powershell" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\deploy.ps1" -Operation FirewallAvailable -Directory "$INSTDIR" -Architecture ${ARCH} -Options "$PLUGINSDIR\options.ini"'
        Pop $0
        Pop $1
        StrCpy $FirewallAvailable 1
        ${If} $0 == 2
            StrCpy $FirewallAvailable 0
            StrCpy $Firewall 0
        ${ElseIf} $0 != 0
            MessageBox MB_OK|MB_ICONSTOP "$1" /SD IDOK
            Abort
        ${EndIf}
    ${EndIf}
    Call WriteOptions
    !insertmacro RunHelper Validate
FunctionEnd

Function LoggingPage
    ${If} $ExistingService != ""
    ${OrIf} $Service == 0
        Abort
    ${EndIf}
    !insertmacro MUI_HEADER_TEXT "Service logging" "Uninstall preserves logs unless you choose to remove them."
    nsDialogs::Create 1018
    Pop $Dialog
    ${NSD_CreateLabel} 0 0 100% 12u "Log file (absolute path):"
    Pop $0
    ${NSD_CreateText} 0 17u 100% 13u $Log
    Pop $LogControl
    ${NSD_CreateLabel} 0 43u 80u 12u "Log level:"
    Pop $0
    ${NSD_CreateDropList} 85u 40u 100u 80u ""
    Pop $LevelControl
    ${NSD_CB_AddString} $LevelControl "fatal"
    ${NSD_CB_AddString} $LevelControl "error"
    ${NSD_CB_AddString} $LevelControl "warning"
    ${NSD_CB_AddString} $LevelControl "info"
    ${NSD_CB_AddString} $LevelControl "debug"
    ${NSD_CB_AddString} $LevelControl "trace"
    ${NSD_CB_SelectString} $LevelControl $Level
    ${NSD_CreateCheckbox} 0 72u 100% 12u "Also write to Windows Event Log"
    Pop $EventControl
    ${NSD_SetState} $EventControl $EventLog
    ${NSD_CreateCheckbox} 0 100u 100% 12u "Allow connections on private networks"
    Pop $FirewallControl
    ${NSD_SetState} $FirewallControl $Firewall
    ${NSD_OnClick} $FirewallControl FirewallSelectionChanged
    EnableWindow $FirewallControl $FirewallAvailable
    ${NSD_CreateLabel} 0 118u 100% 30u "Allows inbound TCP for lockd.exe on the selected port, on Windows Private networks only. Unavailable for localhost and loopback addresses."
    Pop $0
    nsDialogs::Show
FunctionEnd

Function LoggingLeave
    ${NSD_GetText} $LogControl $Log
    ${NSD_GetText} $LevelControl $Level
    ${NSD_GetState} $EventControl $EventLog
    ${NSD_GetState} $FirewallControl $Firewall
    Call WriteOptions
    !insertmacro RunHelper Validate
FunctionEnd

Function FirewallSelectionChanged
    Pop $0
    # Retain the choice on Back, before returning to edit the listening address.
    ${NSD_GetState} $FirewallControl $Firewall
FunctionEnd

Section "lockd"
    Call WriteOptions
    !insertmacro RunHelper Validate
    SetOutPath "$PLUGINSDIR\payload"
    File "${PAYLOAD}\lockd.exe"
    File "${PAYLOAD}\lockd_test.exe"
    File "${PAYLOAD}\license.txt"
    File "${PAYLOAD}\boost_license.txt"
    WriteUninstaller "$PLUGINSDIR\payload\uninstall.exe"
    !insertmacro RunHelper Install
    SetErrorLevel 0
SectionEnd

Function un.onInit
    !insertmacro Runtime
    StrCpy $RemoveLogs 0
    ${GetParameters} $0
    !insertmacro Option RemoveLogs $RemoveLogs
FunctionEnd

Section "Uninstall"
    !insertmacro RunHelper LogFiles
    ${IfNot} ${Silent}
    ${AndIf} $RemoveLogs == 0
    ${AndIf} $1 != ""
        MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also delete these installer-recorded log files and their numbered archives?$\r$\n$\r$\n$1$\r$\n$\r$\nOther files and Windows Event Log history will be preserved." /SD IDNO IDNO keep_logs
        StrCpy $RemoveLogs 1
    ${EndIf}
keep_logs:
    !insertmacro RunHelper Uninstall
    SetErrorLevel 0
SectionEnd
