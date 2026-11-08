# NSIS collects options and supplies a private payload directory. lockd.exe registers
# the service; this helper handles deployment, startup choices, firewall rules and rollback.
param(
    [ValidateSet('Install', 'Uninstall', 'Validate', 'FirewallAvailable', 'LogFiles')][string] $Operation,
    [Parameter(Mandatory)][string] $Directory,
    [ValidateSet('x86', 'x86_64', 'arm64')][string] $Architecture,
    [string] $Payload,
    [string] $Options,
    [string] $Version,
    [ValidateSet('0', '1')][string] $RemoveLogs = '0'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$registry = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\lockd-$Architecture"
$files = @('lockd.exe', 'lockd_test.exe', 'license.txt', 'boost_license.txt', 'uninstall.exe')
$firewallGroup = "lockd installer ($Architecture)"

function Get-OptionalRegistration([string] $Path) {
    try { return Get-ItemProperty -LiteralPath $Path -ErrorAction Stop }
    catch [System.Management.Automation.ItemNotFoundException] { return $null }
}

function Get-CleanupState($Previous) {
    if ($Previous -and $Previous.PSObject.Properties['CleanupState']) {
        $cleanup = $Previous.CleanupState | ConvertFrom-Json
        foreach ($field in @('Directories', 'LogFiles', 'EventSources')) {
            if (!$cleanup.PSObject.Properties[$field] -or $cleanup.$field -isnot [array]) {
                throw 'Installer cleanup records are incomplete.'
            }
            foreach ($value in $cleanup.$field) {
                if ($value -isnot [string] -or !$value) { throw 'Installer cleanup records are invalid.' }
            }
        }
        return $cleanup
    }

    # Older installations did not record directory or log ownership. Preserve anything
    # that cannot be attributed to this installation rather than guessing from its name.
    return [pscustomobject]@{ Directories = @(); LogFiles = @(); EventSources = @() }
}

function Assert-CleanupPath([string] $Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if (![System.IO.Path]::IsPathRooted($Path) -or
            $full.TrimEnd('\') -eq [System.IO.Path]::GetPathRoot($full).TrimEnd('\')) {
        throw "Not an application cleanup path: $Path"
    }

    # A junction or symlink could now point somewhere other than the installation's
    # recorded destination. Never follow it while deleting files or pruning directories.
    $current = $full
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
                throw "Installer cleanup cannot follow a junction or symbolic link: $current"
            }
        }
        $parent = Split-Path -Parent $current
        if ($parent -eq $current) { break }
        $current = $parent
    }
}

function New-OwnedDirectory([string] $Path, $Cleanup) {
    Assert-CleanupPath $Path
    $missing = @()
    $current = [System.IO.Path]::GetFullPath($Path)
    while (!(Test-Path -LiteralPath $current)) {
        $missing = @($current) + $missing
        $current = Split-Path -Parent $current
    }
    foreach ($directory in $missing) {
        # Record each parent separately. A later uninstall may remove only the empty
        # directories created here, never an existing parent or a populated directory.
        New-Item -ItemType Directory -Path $directory | Out-Null
        $Cleanup.Directories += $directory
    }
}

function Remove-EmptyOwnedDirectories($Cleanup) {
    # Recorded directories may only be ancestors of the application directory or of
    # a recorded log. Validate the whole list before removing even its first entry.
    $allowed = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($leaf in @($Directory) + @($Cleanup.LogFiles | ForEach-Object { Split-Path -Parent $_ })) {
        $current = [System.IO.Path]::GetFullPath($leaf).TrimEnd('\')
        while ($current -and $current -ne [System.IO.Path]::GetPathRoot($current).TrimEnd('\')) {
            $null = $allowed.Add($current)
            $current = Split-Path -Parent $current
        }
    }
    foreach ($directory in $Cleanup.Directories) {
        if (!$allowed.Contains([System.IO.Path]::GetFullPath($directory).TrimEnd('\'))) {
            throw "The recorded directory is outside this installation: $directory"
        }
    }
    foreach ($directory in @($Cleanup.Directories | Sort-Object Length -Descending)) {
        Assert-CleanupPath $directory
        if ((Test-Path -LiteralPath $directory) -and
                @(Get-ChildItem -LiteralPath $directory -Force).Count -eq 0) {
            Remove-Item -LiteralPath $directory -Force
        }
    }
}

function Get-EventSourcePath([string] $Name) {
    if ($Name -notmatch '^[A-Za-z0-9_.-]{1,80}$') { throw 'Invalid installer Event Log source name.' }
    return "HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\lockd.$Name"
}

function Get-OwnedEventSources($Cleanup) {
    foreach ($name in $Cleanup.EventSources) {
        $path = Get-EventSourcePath $name
        $source = Get-OptionalRegistration $path
        if (!$source) { continue }

        # Registration is created on demand by the service, including after an install
        # that deliberately leaves it stopped. Check the message-file owner again now.
        if (!$source.PSObject.Properties['EventMessageFile'] -or
                ![string]::Equals([System.IO.Path]::GetFullPath($source.EventMessageFile),
                    (Join-Path $Directory 'lockd.exe'), [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "The recorded Event Log source no longer belongs to this installation: $name"
        }
        if (@(Get-ChildItem -LiteralPath $path).Count) {
            throw "The recorded Event Log source contains unexpected subkeys: $name"
        }
        Write-Output $path
    }
}

function Remove-OwnedEventSources($Cleanup) {
    foreach ($path in @(Get-OwnedEventSources $Cleanup)) { Remove-Item -LiteralPath $path }
}

function Remove-RecordedLogs($Cleanup) {
    foreach ($log in $Cleanup.LogFiles) {
        Assert-CleanupPath $log
        if (Test-Path -LiteralPath $log -PathType Leaf) { Remove-Item -LiteralPath $log -Force }
        $parent = Split-Path -Parent $log
        if (!(Test-Path -LiteralPath $parent)) { continue }
        $name = Split-Path -Leaf $log

        # Boost's own archives append a numeric counter to the full active filename.
        # Literal directory enumeration and an escaped name avoid wildcard expansion
        # in user-selected filenames. Other archive naming schemes remain user-owned.
        # Match archive names exactly, including in case-sensitive Windows directories.
        $archive = '^' + [regex]::Escape($name) + '\.[0-9]+$'
        foreach ($file in Get-ChildItem -LiteralPath $parent -File -Force) {
            if ($file.Name -cmatch $archive) {
                Assert-CleanupPath $file.FullName
                Remove-Item -LiteralPath $file.FullName -Force
            }
        }
    }
}

function Invoke-Rollback([scriptblock] $Action, $Failures) {
    try { & $Action }
    catch { $Failures.Add($_.Exception.Message) }
}

function Read-Options {
    $settings = @{ Firewall = '0' }
    foreach ($line in Get-Content -LiteralPath $Options) {
        if ($line -match '^([^=]+)=(.*)$') { $settings[$Matches[1]] = $Matches[2] }
    }
    return $settings
}

function Test-LoopbackAddress([string] $Address) {
    # No DNS lookup is needed for the conventional local hostname. Use the address
    # parser for literals, including the whole IPv4 loopback range and mapped IPv6.
    if ($Address.TrimEnd('.') -ieq 'localhost') { return $true }
    $parsed = $null
    if (![System.Net.IPAddress]::TryParse($Address, [ref]$parsed)) { return $false }
    if ($parsed.IsIPv4MappedToIPv6) { $parsed = $parsed.MapToIPv4() }
    # A zone identifier does not make ::1 remotely reachable, but .NET's loopback
    # comparison includes ScopeId. Ignore that identifier for this classification.
    if ($parsed.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetworkV6) {
        $parsed.ScopeId = 0
    }
    return [System.Net.IPAddress]::IsLoopback($parsed)
}

function Assert-Options($Settings) {
    if ($Settings.Service -notin @('0', '1')) { throw 'Service must be 0 or 1.' }
    if ($Settings.Firewall -notin @('0', '1')) { throw 'Firewall must be 0 or 1.' }
    if ($Settings.Firewall -eq '1' -and
            ($Settings.Service -eq '0' -or (Test-LoopbackAddress $Settings.Address))) {
        throw 'A firewall rule requires a service listening on a non-loopback address.'
    }
    if ($Settings.Service -eq '0') { return }
    if ($Settings.Name -notmatch '^[A-Za-z0-9_.-]{1,80}$') {
        throw 'Service name must contain 1-80 letters, digits, dots, hyphens or underscores.'
    }
    $address = $null
    if (![System.Net.IPAddress]::TryParse($Settings.Address, [ref]$address) -and
            [System.Uri]::CheckHostName($Settings.Address) -ne [System.UriHostNameType]::Dns) {
        throw 'Enter an IPv4 or IPv6 listening address, or a hostname.'
    }
    foreach ($key in @('Port', 'Threads')) {
        if ($key -eq 'Threads' -and $Settings[$key] -eq '') { continue }
        if ($Settings[$key] -notmatch '^\d{1,5}$' -or [int]$Settings[$key] -lt 1 -or [int]$Settings[$key] -gt 65535) {
            throw "$key must be between 1 and 65535. Leave threads empty for automatic selection."
        }
    }
    if ($Settings.Level -notin @('fatal', 'error', 'warning', 'info', 'debug', 'trace')) {
        throw 'Choose a valid log level.'
    }
    foreach ($key in @('EventLog', 'Automatic', 'Start')) {
        if ($Settings[$key] -notin @('0', '1')) { throw "$key must be 0 or 1." }
    }
    if ($Settings.Log -notmatch '^[A-Za-z]:\\' -or $Settings.Log -match '["\r\n]' -or
            $Settings.Log.EndsWith('\')) {
        throw 'Use an absolute local log-file path, including a filename.'
    }
    $null = [System.IO.Path]::GetFullPath($Settings.Log)
}

function Get-ServiceExecutable([string] $Command) {
    # CLI service reinstallation can legitimately change every daemon option. Parse
    # the executable with Windows instead of trusting a command prefix or argument text.
    if (!('LockdInstallerCommandLine' -as [type])) {
        # NSIS runs this helper beside its native System.dll plugin. A relative
        # compiler reference can select that plugin instead of .NET's System.dll.
        # Reference the already loaded framework assembly by its actual location.
        Add-Type -ReferencedAssemblies ([System.ComponentModel.Win32Exception].Assembly.Location) `
            -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class LockdInstallerCommandLine {
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CommandLineToArgvW(string command, out int count);
    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);
    public static string Executable(string command) {
        int count;
        IntPtr arguments = CommandLineToArgvW(command, out count);
        if (arguments == IntPtr.Zero) throw new Win32Exception();
        try {
            if (count == 0) return "";
            return Marshal.PtrToStringUni(Marshal.ReadIntPtr(arguments));
        }
        finally { LocalFree(arguments); }
    }
}
'@
    }
    if (!$Command) { throw 'The installed service command is empty.' }
    return [LockdInstallerCommandLine]::Executable($Command)
}

function Get-OwnedService($Previous) {
    if (!$Previous -or !$Previous.ServiceName) { return $null }
    $service = Get-CimInstance Win32_Service -Filter "Name='$($Previous.ServiceName)'"
    if ($service -and $service.PathName -ne $Previous.ServiceCommand) {
        $executable = Get-ServiceExecutable $service.PathName
        if (!$executable -or ![string]::Equals([System.IO.Path]::GetFullPath($executable),
                (Join-Path $Directory 'lockd.exe'), [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'The installed service belongs to another executable. Restore it or remove the service before continuing.'
        }
    }
    return $service
}

function Stop-OwnedService($Service) {
    if ($Service -and $Service.State -ne 'Stopped') {
        $controller = Get-Service -Name $Service.Name
        try {
            $controller.Stop()
            $controller.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(45))
        }
        finally { $controller.Dispose() }
    }
}

function Remove-OwnedService($Service) {
    if (!$Service) { return }
    Stop-OwnedService $Service
    $result = Invoke-CimMethod -InputObject $Service -MethodName Delete
    if ($result.ReturnValue -ne 0) { throw "Service removal failed: $($result.ReturnValue)" }

    # SCM can defer deletion while another process holds a service handle. A successful
    # uninstall must not report completion with that service registry entry still present.
    $deadline = [System.Diagnostics.Stopwatch]::StartNew()
    while (Test-Path -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Services\$($Service.Name)") {
        if ($deadline.Elapsed.TotalSeconds -ge 45) { throw 'Service deletion did not complete within 45 seconds.' }
        Start-Sleep -Milliseconds 100
    }
}

function Get-OwnedFirewall($Previous) {
    # Older installers did not record a firewall rule. Never find rules by display
    # name or port: another application or administrator may own those matches.
    if (!$Previous -or !$Previous.PSObject.Properties['FirewallRule'] -or
            !$Previous.FirewallRule) { return $null }
    $identifier = [guid]::Empty
    $prefix = "lockd-$Architecture-"
    if (!$Previous.FirewallRule.StartsWith($prefix) -or
            ![guid]::TryParseExact($Previous.FirewallRule.Substring($prefix.Length), 'D', [ref]$identifier)) {
        throw 'The recorded installer firewall rule identifier is invalid.'
    }
    # Enumerating avoids treating a missing rule as an error while still surfacing
    # genuine firewall-service failures. Restrict all writes to the local store.
    $rule = Get-NetFirewallRule -PolicyStore PersistentStore |
        Where-Object { $_.Name -eq $Previous.FirewallRule }
    if ($rule) {
        $application = Get-NetFirewallApplicationFilter -AssociatedNetFirewallRule $rule
        if ($rule.Group -ne $firewallGroup -or
                $application.Program -ne (Join-Path $Directory 'lockd.exe')) {
            throw 'The recorded firewall rule no longer belongs to this installation.'
        }
    }
    return $rule
}

try {
    if ($Operation -eq 'FirewallAvailable') {
        $settings = Read-Options
        if ($settings.Service -ne '1' -or (Test-LoopbackAddress $settings.Address)) { exit 2 }
        exit 0
    }
    $Directory = [System.IO.Path]::GetFullPath($Directory).TrimEnd('\')
    if ($Directory -match '["\r\n]' -or $Directory -eq [System.IO.Path]::GetPathRoot($Directory).TrimEnd('\')) {
        throw 'Choose an application directory, not a drive root.'
    }
    $previous = Get-OptionalRegistration $registry
    if ($previous -and $previous.InstallLocation -ne $Directory) {
        throw 'Upgrade in the existing installation directory, or uninstall first.'
    }
    $cleanup = Get-CleanupState $previous
    $originalDirectories = @($cleanup.Directories)
    $originalEventSources = @($cleanup.EventSources)
    $service = Get-OwnedService $previous
    $firewall = Get-OwnedFirewall $previous
    if ($Operation -eq 'LogFiles') {
        if (!$previous) { throw 'This directory is not a registered lockd installation.' }
        foreach ($log in $cleanup.LogFiles) { Write-Output $log }
        exit 0
    }
    if ($Operation -eq 'Uninstall') {
        if (!$previous) { throw 'This directory is not a registered lockd installation.' }
        Assert-CleanupPath $Directory
        $null = @(Get-OwnedEventSources $cleanup)
        $uninstaller = Join-Path $Directory 'uninstall.exe'
        $uninstallerBackup = $null
        if (Test-Path -LiteralPath $uninstaller) {
            $uninstallerBackup = [System.IO.File]::ReadAllBytes($uninstaller)
        }
        try {
            Remove-OwnedService $service
            if ($firewall) { Remove-NetFirewallRule -InputObject $firewall }
            Remove-OwnedEventSources $cleanup
            if ($RemoveLogs -eq '1') { Remove-RecordedLogs $cleanup }
            foreach ($file in $files | Where-Object { $_ -ne 'uninstall.exe' }) {
                $path = Join-Path $Directory $file
                if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
            }

            # NSIS runs from a temporary copy. Remove the installed uninstaller last,
            # then prune empty directories and remove registration only after success.
            if (Test-Path -LiteralPath $uninstaller) { Remove-Item -LiteralPath $uninstaller -Force }
            Remove-EmptyOwnedDirectories $cleanup
            Remove-Item -LiteralPath $registry
        }
        catch {
            $failure = $_

            # Retry can skip completed removals using the retained ownership record.
            # If the user cancels, restore the uninstaller so a later attempt still works,
            # including when directory pruning or registry removal failed at the very end.
            if ($null -ne $uninstallerBackup -and !(Test-Path -LiteralPath $uninstaller)) {
                try {
                    New-OwnedDirectory $Directory $cleanup
                    [System.IO.File]::WriteAllBytes($uninstaller, $uninstallerBackup)
                }
                catch {
                    throw ($failure.Exception.Message + ' Could not restore the uninstaller: ' + $_.Exception.Message)
                }
            }
            throw $failure
        }
        exit 0
    }

    $settings = Read-Options
    Assert-Options $settings
    if ($previous -and $previous.ServiceName -and !$service) {
        throw 'The registered service is missing. Uninstall this installation before reinstalling it.'
    }
    if (!$previous -and (Test-Path -LiteralPath $Directory) -and
            @(Get-ChildItem -LiteralPath $Directory -Force).Count) {
        throw 'Choose an empty directory for a new installation.'
    }
    if (!$service -and $settings.Service -eq '1' -and
            (Get-Service -Name $settings.Name -ErrorAction SilentlyContinue)) {
        throw 'That service name is already in use. Choose another name.'
    }
    if ($previous -and [version]($Version -replace '\.dev0$', '') -lt
            [version]($previous.DisplayVersion -replace '\.dev0$', '')) {
        throw 'Uninstall the newer version before installing an older release.'
    }
    if ($Operation -eq 'Validate') { exit 0 }

    # Preserve an existing service's command, account, startup mode and running state.
    # Upgrading binaries must not reset an administrator's service configuration.
    $running = $service -and $service.State -eq 'Running'
    $backup = Join-Path $Payload 'backup'
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    if ($previous) {
        foreach ($file in $files) {
            Copy-Item -LiteralPath (Join-Path $Directory $file) -Destination $backup
        }
    }
    $createdService = $null
    $createdFirewallName = ''
    try {
        Stop-OwnedService $service
        New-OwnedDirectory $Directory $cleanup
        foreach ($file in $files) {
            Copy-Item -LiteralPath (Join-Path $Payload $file) -Destination $Directory -Force
        }
        $serviceName = if ($previous) { $previous.ServiceName } else { '' }
        $command = if ($service) { $service.PathName } else { '' }
        if (!$service -and $settings.Service -eq '1') {
            $serviceName = $settings.Name
            $logDirectory = Split-Path -Parent $settings.Log
            New-OwnedDirectory $logDirectory $cleanup
            if ($settings.Log -notin $cleanup.LogFiles) { $cleanup.LogFiles += $settings.Log }
            $eventSource = Get-EventSourcePath $serviceName
            if (!(Get-OptionalRegistration $eventSource) -and
                    $serviceName -notin $cleanup.EventSources) {
                $cleanup.EventSources += $serviceName
            }

            # The daemon owns the SCM command and description, including defaults such as
            # the resolved worker count. Pass values separately so paths with spaces survive.
            $installArguments = @('--install-service', '--service-name', $serviceName,
                '--interface', $settings.Address, '--port', $settings.Port,
                '--log-level', $settings.Level, '--log-file', $settings.Log)
            if ($settings.Threads) { $installArguments += @('--threads', $settings.Threads) }
            if ($settings.EventLog -eq '0') { $installArguments += '--no-event-log' }
            & (Join-Path $Directory 'lockd.exe') @installArguments
            if ($LASTEXITCODE -ne 0) {
                throw "Service installation failed: lockd.exe returned $LASTEXITCODE."
            }
            $createdService = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
            if (!$createdService) { throw 'The installed service could not be read back.' }
            $command = $createdService.PathName
            $startup = if ($settings.Automatic -eq '1') { 'Automatic' } else { 'Manual' }
            Set-Service -Name $serviceName -StartupType $startup
            if ($settings.Firewall -eq '1') {
                # Keep the ID before calling NetSecurity so rollback can remove a rule
                # even if creation succeeds but the cmdlet subsequently reports failure.
                $createdFirewallName = "lockd-$Architecture-$([guid]::NewGuid().ToString('D'))"
                New-NetFirewallRule -PolicyStore PersistentStore -Name $createdFirewallName `
                    -DisplayName "lockd ($Architecture, $serviceName)" -Group $firewallGroup `
                    -Direction Inbound -Action Allow -Enabled True -Profile Private `
                    -Program (Join-Path $Directory 'lockd.exe') -Protocol TCP `
                    -LocalPort $settings.Port -EdgeTraversalPolicy Block | Out-Null
            }
            if ($settings.Start -eq '1') {
                Start-Service -Name $serviceName
                (Get-Service -Name $serviceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
            }
        }
        elseif ($running) { Start-Service -Name $service.Name }

        New-Item -Path $registry -Force | Out-Null
        $values = @{
            DisplayName = "lockd ($Architecture)"; DisplayVersion = $Version
            Publisher = 'lockd'; InstallLocation = $Directory
            UninstallString = '"' + (Join-Path $Directory 'uninstall.exe') + '"'
            QuietUninstallString = '"' + (Join-Path $Directory 'uninstall.exe') + '" /S'
            ServiceName = $serviceName; ServiceCommand = $command
            CleanupState = ConvertTo-Json -InputObject $cleanup -Compress
        }

        # Upgrades preserve the existing service and firewall configuration together.
        # Do not open a port from the wizard's defaults when upgrading an old service,
        # or recreate a rule an administrator deliberately removed.
        if ($previous -and $previous.PSObject.Properties['FirewallRule']) {
            $values.FirewallRule = $previous.FirewallRule
        }
        elseif ($createdFirewallName) { $values.FirewallRule = $createdFirewallName }
        foreach ($key in $values.Keys) {
            New-ItemProperty -LiteralPath $registry -Name $key -Value $values[$key] -PropertyType String -Force | Out-Null
        }
        foreach ($key in @('NoModify', 'NoRepair')) {
            New-ItemProperty -LiteralPath $registry -Name $key -Value 1 -PropertyType DWord -Force | Out-Null
        }
    }
    catch {
        $failure = $_
        $rollbackFailures = [System.Collections.Generic.List[string]]::new()
        $createdCleanup = [pscustomobject]@{
            Directories = @($cleanup.Directories | Where-Object { $_ -notin $originalDirectories })
            EventSources = @($cleanup.EventSources | Where-Object { $_ -notin $originalEventSources })
            LogFiles = $cleanup.LogFiles
        }
        if ($createdFirewallName) {
            Invoke-Rollback {
                $createdFirewall = Get-OwnedFirewall ([pscustomobject]@{ FirewallRule = $createdFirewallName })
                if ($createdFirewall) { Remove-NetFirewallRule -InputObject $createdFirewall }
            } $rollbackFailures
        }
        Invoke-Rollback { Remove-OwnedService $createdService } $rollbackFailures
        Invoke-Rollback { Remove-OwnedEventSources $createdCleanup } $rollbackFailures
        if ($previous) {
            Invoke-Rollback {
                foreach ($file in $files) {
                    Copy-Item -LiteralPath (Join-Path $backup $file) -Destination $Directory -Force
                }
                foreach ($key in @('DisplayName', 'DisplayVersion', 'Publisher', 'InstallLocation',
                        'UninstallString', 'QuietUninstallString', 'ServiceName', 'ServiceCommand',
                        'CleanupState', 'FirewallRule')) {
                    if ($previous.PSObject.Properties[$key]) {
                        New-ItemProperty -LiteralPath $registry -Name $key -Value $previous.$key -PropertyType String -Force | Out-Null
                    }
                    elseif ((Get-ItemProperty -LiteralPath $registry).PSObject.Properties[$key]) {
                        Remove-ItemProperty -LiteralPath $registry -Name $key
                    }
                }
                if ($running) { Start-Service -Name $service.Name }
            } $rollbackFailures
        }
        else {
            foreach ($file in $files) {
                Invoke-Rollback {
                    $path = Join-Path $Directory $file
                    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
                } $rollbackFailures
            }
        }
        Invoke-Rollback { Remove-EmptyOwnedDirectories $createdCleanup } $rollbackFailures
        if (!$previous) {
            Invoke-Rollback {
                if (!$rollbackFailures.Count -and (Test-Path -LiteralPath $registry)) {
                    Remove-Item -LiteralPath $registry
                }
            } $rollbackFailures
        }
        if ($rollbackFailures.Count) {
            throw ($failure.Exception.Message + " Rollback incomplete: " + ($rollbackFailures -join '; '))
        }
        throw $failure
    }
}
catch {
    Write-Output $_.Exception.Message
    exit 1
}
