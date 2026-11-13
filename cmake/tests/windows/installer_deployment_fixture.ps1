# Run the real helper with isolated files and service/registry/firewall boundaries.
# The fake daemon records native arguments without registering a Windows service.
param(
    [switch] $BuildProbe,
    [string] $DeployScript,
    [string] $Directory,
    [string] $Payload,
    [string] $Options,
    [string] $Trace,
    [string] $ServiceName,
    [ValidateSet('Install', 'Uninstall', 'Validate', 'FirewallAvailable', 'LogFiles')][string] $Operation = 'Install',
    [string] $Scenario = 'Success',
    [ValidateSet('x86', 'x86_64', 'arm64')][string] $Architecture = 'x86_64',
    [string] $CleanupStateFile,
    [ValidateSet('0', '1')][string] $RemoveLogs = '0',
    [switch] $RetryCleanup
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($BuildProbe) {
    Add-Type -OutputAssembly (Join-Path $Payload 'lockd.exe') -OutputType ConsoleApplication `
        -TypeDefinition @'
using System;
using System.IO;
public static class InstallerProbe {
    public static int Main(string[] args) {
        File.WriteAllLines(Environment.GetEnvironmentVariable("LOCKD_INSTALLER_PROBE_TRACE"), args);
        return int.Parse(Environment.GetEnvironmentVariable("LOCKD_INSTALLER_PROBE_EXIT"));
    }
}
'@
    exit 0
}

$CleanupState = if ($CleanupStateFile) { Get-Content -LiteralPath $CleanupStateFile -Raw } else { '' }
$registryPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\lockd-$Architecture"
$state = @{ Startup = @(); Started = @(); Stopped = @(); Deleted = @(); Registry = @{} }
$state.RegistryExists = $false
$state.EventSources = @{}
$state.EventSourcesDeleted = @()
$state.PruneFailuresRemaining = $(if ($Scenario -like 'Prune*') { 1 } else { 0 })
$hasPrevious = $Scenario.StartsWith('Upgrade') -or $Operation -eq 'Uninstall'
$state.RegistryExists = $hasPrevious
$eventPath = "HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\lockd.$ServiceName"
if ($Scenario -match 'EventSource') {
    $state.EventSources[$eventPath] = [pscustomobject]@{
        EventMessageFile = $(if ($Scenario -eq 'ForeignEventSource') {
            'C:\unrelated.exe'
        } else { Join-Path $Directory 'lockd.exe' })
    }
}
$firewallName = "lockd-$Architecture-11111111-2222-3333-4444-555555555555"
$state.FirewallCreated = @()
$state.FirewallDeleted = @()
$state.FirewallRules = @([pscustomobject]@{
    Name = 'unrelated-rule'; Group = 'unrelated'; Program = 'C:\unrelated.exe'
})
if ($Scenario -match 'Firewall' -and $hasPrevious -and $Scenario -ne 'UpgradeFirewallMissing') {
    $state.FirewallRules += [pscustomobject]@{
        Name = $firewallName; Group = "lockd installer ($Architecture)"
        Program = $(if ($Scenario -eq 'UpgradeFirewallForeign') {
            'C:\unrelated.exe'
        } else { Join-Path $Directory 'lockd.exe' })
    }
}
$registered = [pscustomobject]@{
    Name = $ServiceName
    PathName = '"command supplied by lockd.exe" --daemon-owned-options'
    State = $(if ($hasPrevious) { 'Running' } else { 'Stopped' })
}
if ($Scenario -eq 'UpgradeReconfigured') {
    $registered.PathName = '"' + (Join-Path $Directory 'lockd.exe') + '" --as-service --updated-options'
}
elseif ($Scenario -eq 'UpgradeForeignCommand') {
    $registered.PathName = '"C:\Windows\System32\cmd.exe" /c exit'
}

function Get-ItemProperty($LiteralPath, $ErrorAction) {
    if ($LiteralPath -eq $eventPath -and $Scenario -eq 'SourceReadFailure') {
        throw [System.UnauthorizedAccessException]::new('Simulated Event Log registry access denial.')
    }
    if ($LiteralPath -eq $eventPath) { return $state.EventSources[$eventPath] }
    if ($LiteralPath -ne $registryPath) { throw 'Unexpected registry lookup.' }
    if ($hasPrevious) {
        $previous = [pscustomobject]@{
            InstallLocation = $Directory; ServiceName = $ServiceName
            ServiceCommand = '"command supplied by lockd.exe" --daemon-owned-options'; DisplayVersion = '1.0.130'
            DisplayName = 'lockd'; Publisher = 'lockd'
            UninstallString = 'uninstall'; QuietUninstallString = 'uninstall /S'
        }
        if ($Scenario -match 'Firewall') {
            $previous | Add-Member NoteProperty FirewallRule $firewallName
        }
        if ($CleanupState) { $previous | Add-Member NoteProperty CleanupState $CleanupState }
        foreach ($key in $state.Registry.Keys) {
            $previous | Add-Member -Force NoteProperty $key $state.Registry[$key]
        }
        return $previous
    }
    if ($state.RegistryExists) { return [pscustomobject]$state.Registry }
}

function Test-Path($LiteralPath, $Path, $PathType) {
    if ($LiteralPath -eq $registryPath) { return $state.RegistryExists }
    if ($LiteralPath -eq "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName") { return $false }
    if ($LiteralPath -like 'HKLM:*') { throw 'Unexpected registry existence check.' }
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}

function Get-ChildItem($LiteralPath, [switch] $Force, [switch] $File) {
    if ($LiteralPath -eq $eventPath) { return }
    if ($LiteralPath -like 'HKLM:*') { throw 'Unexpected registry enumeration.' }
    Microsoft.PowerShell.Management\Get-ChildItem @PSBoundParameters
}

function Get-Item($LiteralPath, [switch] $Force) {
    if ($Scenario -eq 'RedirectedLogs' -and $CleanupState -and
            $LiteralPath -eq (Split-Path -Parent (($CleanupState | ConvertFrom-Json).LogFiles[0]))) {
        return [pscustomobject]@{ Attributes = [System.IO.FileAttributes]::ReparsePoint }
    }
    Microsoft.PowerShell.Management\Get-Item @PSBoundParameters
}

function Get-Service($Name, $ErrorAction) {
    if ($Name -ne $ServiceName) { throw 'Unexpected service lookup.' }
    if ($Scenario -eq 'Duplicate' -or $hasPrevious -or
            (Test-Path -LiteralPath $env:LOCKD_INSTALLER_PROBE_TRACE)) {
        $controller = [pscustomobject]@{ Name = $Name; Events = $state }
        $controller | Add-Member ScriptMethod Stop { $this.Events.Stopped += $this.Name }
        $controller | Add-Member ScriptMethod WaitForStatus { param($Status, $Timeout) }
        $controller | Add-Member ScriptMethod Dispose {}
        return $controller
    }
}

function Get-CimInstance($ClassName, $Filter) {
    if ($ClassName -ne 'Win32_Service' -or $Filter -ne "Name='$ServiceName'") {
        throw 'Unexpected service query.'
    }
    if ($ServiceName -in $state.Deleted) { return }
    if ($hasPrevious -or (Test-Path -LiteralPath $env:LOCKD_INSTALLER_PROBE_TRACE)) {
        return $registered
    }
}

function New-Service { throw 'Registration must use lockd.exe.' }
function Set-Service($Name, $StartupType) {
    $state.Startup += @{ Name = $Name; Mode = $StartupType }
    if ($Scenario -eq 'StartupFailure') { throw 'Simulated startup configuration failure.' }
}
function Start-Service($Name) {
    $state.Started += $Name
    $eventLogEnabled = !$Options -or (Get-Content -LiteralPath $Options) -notcontains 'EventLog=0'
    if ($eventLogEnabled -and !$state.EventSources.ContainsKey($eventPath)) {
        $state.EventSources[$eventPath] = [pscustomobject]@{
            EventMessageFile = Join-Path $Directory 'lockd.exe'
        }
    }
    if ($Scenario -eq 'StartFailure') { throw 'Simulated service start failure.' }
}
function Invoke-CimMethod($InputObject, $MethodName) {
    if ($MethodName -ne 'Delete') { throw 'Unexpected service mutation.' }
    $state.Deleted += $InputObject.Name
    return [pscustomobject]@{ ReturnValue = 0 }
}

function Get-NetFirewallRule($PolicyStore) {
    if ($PolicyStore -ne 'PersistentStore') { throw 'Unexpected firewall policy store.' }
    return $state.FirewallRules
}
function Get-NetFirewallApplicationFilter($AssociatedNetFirewallRule) {
    return [pscustomobject]@{ Program = $AssociatedNetFirewallRule.Program }
}
function New-NetFirewallRule($PolicyStore, $Name, $DisplayName, $Group, $Direction,
        $Action, $Enabled, $Profile, $Program, $Protocol, $LocalPort, $EdgeTraversalPolicy) {
    $state.FirewallCreated += @{} + $PSBoundParameters
    $state.FirewallRules += [pscustomobject]@{ Name = $Name; Group = $Group; Program = $Program }
    if ($Scenario -eq 'FirewallFailure') { throw 'Simulated firewall creation failure.' }
}
function Remove-NetFirewallRule($InputObject) {
    $state.FirewallDeleted += $InputObject.Name
    $state.FirewallRules = @($state.FirewallRules | Where-Object { $_.Name -ne $InputObject.Name })
}

function New-Item($Path, $ItemType, [switch] $Force) {
    if ($Path -eq $registryPath) { $state.RegistryExists = $true; return }
    if ($Path -like 'HKLM:*') { throw 'Unexpected registry creation.' }
    Microsoft.PowerShell.Management\New-Item @PSBoundParameters
}
function New-ItemProperty($LiteralPath, $Name, $Value, $PropertyType, [switch] $Force) {
    if ($LiteralPath -ne $registryPath) { throw 'Unexpected registry write.' }
    $state.Registry[$Name] = $Value
    if ($Scenario -eq 'RegistryFailure') { throw 'Simulated registration write failure.' }
}
function Remove-Item($LiteralPath, [switch] $Force, $ErrorAction) {
    if ($LiteralPath -eq $registryPath) {
        $state.Registry = @{}; $state.RegistryExists = $false; return
    }
    if ($LiteralPath -eq $eventPath) {
        $state.EventSources.Remove($eventPath)
        $state.EventSourcesDeleted += $eventPath
        return
    }
    if ($LiteralPath -like 'HKLM:*') { throw 'Unexpected registry deletion.' }
    if ($LiteralPath -eq $Directory -and $state.PruneFailuresRemaining -gt 0) {
        --$state.PruneFailuresRemaining
        throw 'Simulated directory cleanup failure.'
    }
    Microsoft.PowerShell.Management\Remove-Item @PSBoundParameters
}

function Remove-ItemProperty($LiteralPath, $Name) {
    if ($LiteralPath -ne $registryPath) { throw 'Unexpected registry value deletion.' }
    $state.Registry.Remove($Name)
}

# Child-script exit codes must survive fixture reporting, including native failures.
$global:LASTEXITCODE = 0
$lockedLog = $null
try {
    if ($Scenario -in @('LockedLog', 'LockedLogRetry')) {
        $log = ($CleanupState | ConvertFrom-Json).LogFiles[0]
        $lockedLog = [System.IO.File]::Open($log, 'Open', 'ReadWrite', 'None')
    }
    & $DeployScript -Operation $Operation -Directory $Directory -Architecture $Architecture `
        -Payload $Payload -Options $Options -Version 1.0.130 -RemoveLogs $RemoveLogs
    $result = $LASTEXITCODE
    $state.CleanupAttempts = @($result)
    if ($RetryCleanup -and $result -ne 0) {
        if ($lockedLog) { $lockedLog.Dispose(); $lockedLog = $null }
        & $DeployScript -Operation $Operation -Directory $Directory -Architecture $Architecture `
            -Payload $Payload -Options $Options -Version 1.0.130 -RemoveLogs $RemoveLogs
        $result = $LASTEXITCODE
        $state.CleanupAttempts += $result
    }
}
finally {
    if ($lockedLog) { $lockedLog.Dispose() }
    $state.BinaryExists = Test-Path -LiteralPath (Join-Path $Directory 'lockd.exe')
    $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Trace -Encoding UTF8
}
exit $result
