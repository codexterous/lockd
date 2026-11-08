param([ValidateSet('x86', 'x86_64', 'arm64')][string] $Architecture = 'x86_64')

$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true') {
    throw 'Run installer lifecycle tests only on a disposable GitHub Actions runner.'
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$installers = @(Get-ChildItem (Join-Path $root "output/$Architecture/opt/static") -Filter "*-windows-$Architecture-setup.exe")
if ($installers.Count -ne 1) { throw 'Expected exactly one installer for this architecture.' }
$installer = $installers[0].FullName
$directory = Join-Path $env:ProgramFiles "lockd installer test $Architecture"
$serviceName = "lockd_installer_test_$Architecture"
$log = Join-Path $env:ProgramData "lockd installer test $Architecture.log"
$view = if ($Architecture -eq 'x86') { 'Registry32' } else { 'Registry64' }
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $view)
$keyName = "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\lockd-$Architecture"
$diagnostics = Join-Path $root "output/obj/$Architecture/opt/static/verifications/installer_lifecycle"
New-Item -ItemType Directory -Force $diagnostics | Out-Null
$script:installerAttempt = 0

function Run-Installer([string] $File, [string] $Arguments, [int] $Expected = 0) {
    ++$script:installerAttempt
    $label = '{0:D2}-{1}' -f $script:installerAttempt, [System.IO.Path]::GetFileNameWithoutExtension($File)
    $helperLog = Join-Path $diagnostics "$label.log"
    Set-Content -LiteralPath $helperLog -Value '' -Encoding Unicode
    Set-Content -LiteralPath (Join-Path $diagnostics "$label-command.txt") -Value "$File $Arguments"
    Write-Host "Installer invocation $($script:installerAttempt): $File $Arguments"
    $previousLog = $env:LOCKD_INSTALLER_DIAGNOSTIC_LOG
    $process = $null
    try {
        # Pass an independent log path to each installer process. NSIS normally keeps
        # helper failures in its detail pane, which silent CI runs cannot inspect.
        $env:LOCKD_INSTALLER_DIAGNOSTIC_LOG = $helperLog
        $process = Start-Process -FilePath $File -ArgumentList $Arguments -PassThru -WindowStyle Hidden
        if (!$process.WaitForExit(180000)) { throw 'Installer did not exit within three minutes.' }
        [string] $detail = Get-Content -LiteralPath $helperLog -Raw
        if (!$detail.Trim()) {
            if ($Expected -ne 0) { throw 'The expected installer rejection did not record its diagnostic.' }
            $detail = 'The installer exited before recording a helper diagnostic.'
        }
        if ($process.ExitCode -ne $Expected) {
            throw "Installer invocation $($script:installerAttempt) exited $($process.ExitCode), expected $Expected.`n$detail`nCommand: $File $Arguments"
        }
    }
    finally {
        $env:LOCKD_INSTALLER_DIAGNOSTIC_LOG = $previousLog
        if ($process) { $process.Dispose() }
    }
}

function Read-Registration {
    $key = $base.OpenSubKey($keyName)
    if (!$key) { return $null }
    try { return $key.GetValue('InstallLocation') }
    finally { $key.Dispose() }
}

function Read-FirewallRuleName {
    $key = $base.OpenSubKey($keyName)
    if (!$key) { return $null }
    try { return $key.GetValue('FirewallRule') }
    finally { $key.Dispose() }
}

function Run-Uninstaller {
    param([switch] $RemoveLogs)
    # Running a copy keeps the installed uninstaller removable and lets us wait
    # for completion, instead of NSIS spawning another temporary process.
    $copy = Join-Path $root "output/obj/$Architecture/opt/static/verifications/uninstall.exe"
    New-Item -ItemType Directory -Force (Split-Path -Parent $copy) | Out-Null
    Copy-Item -LiteralPath (Join-Path $directory 'uninstall.exe') -Destination $copy -Force
    $logOption = if ($RemoveLogs) { ' /RemoveLogs=1' } else { '' }
    Run-Installer $copy "/S$logOption _?=$directory"
}

try {
    if ((Read-Registration) -or (Test-Path -LiteralPath $directory) -or
            (Get-Service -Name $serviceName -ErrorAction SilentlyContinue)) {
        throw 'Installer test requires a clean runner.'
    }

    # Invalid silent options must fail before copying files or registering a service.
    Run-Installer $installer "/S /Service=1 /Port=0 /D=$directory" 1
    foreach ($address in @('localhost', '127.0.0.2', '::1')) {
        Run-Installer $installer "/S /Service=1 /Address=$address /Firewall=1 /D=$directory" 1
    }
    if ((Read-Registration) -or (Test-Path -LiteralPath $directory)) {
        throw 'Invalid options changed the machine.'
    }
    Run-Installer $installer "/S /D=$directory"
    if ((Read-Registration) -ne $directory) { throw 'Installation directory was not recorded.' }
    & (Join-Path $directory 'lockd.exe') --version
    if ($LASTEXITCODE -ne 0) { throw 'Installed daemon does not run.' }
    Run-Installer $installer "/S /D=$directory"
    Run-Uninstaller
    if (Read-Registration) { throw 'Files-only uninstall left its registration behind.' }

    # A failed service start rolls back the new service and copied package files.
    $rulesBefore = @(Get-NetFirewallRule -PolicyStore PersistentStore | Select-Object -ExpandProperty Name)
    $failureOptions = "/S /Service=1 /ServiceName=$serviceName /Address=192.0.2.1 /Firewall=1 /EventLog=0 /LogFile=`"$log`" /D=$directory"
    Run-Installer $installer $failureOptions 1
    if ((Read-Registration) -or (Test-Path -LiteralPath (Join-Path $directory 'lockd.exe')) -or
            (Get-Service -Name $serviceName -ErrorAction SilentlyContinue)) {
        throw 'Failed service startup was not rolled back.'
    }
    $rulesAfter = @(Get-NetFirewallRule -PolicyStore PersistentStore | Select-Object -ExpandProperty Name)
    if (@($rulesAfter | Where-Object { $_ -notin $rulesBefore -and $_ -like "lockd-$Architecture-*" }).Count) {
        throw 'Failed service startup left an installer firewall rule behind.'
    }

    # Select an unused loopback port and verify real SCM startup, upgrade and removal.
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    # An explicit worker count may exceed the automatic default's eight-worker cap.
    $options = "/S /Service=1 /ServiceName=$serviceName /Port=$port /Threads=9 /Automatic=0 /EventLog=0 /LogFile=`"$log`" /D=$directory"
    Run-Installer $installer $options
    if (Read-FirewallRuleName) { throw 'Loopback installation recorded a firewall rule.' }
    if ((Get-Service -Name $serviceName).Status -ne 'Running') { throw 'Installed service did not start.' }
    $before = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    $description = "lockd, an interprocess locking daemon, running on 'localhost' port $port " +
        "with 9 acquisition threads and log level 'info'. Windows Event Log output is disabled."
    if ($before.Description -ne $description) {
        throw ("Installed service description does not match the selected options.`n" +
            "Expected: $description`nActual: $($before.Description)`n" +
            "Command: $($before.PathName)")
    }
    Run-Installer $installer "/S /D=$directory"
    $after = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    if ($after.State -ne 'Running' -or $before.PathName -ne $after.PathName -or
            $before.StartMode -ne $after.StartMode -or $before.Description -ne $after.Description) {
        throw 'Upgrade changed the existing service configuration or running state.'
    }
    Run-Uninstaller
    if ((Get-Service -Name $serviceName -ErrorAction SilentlyContinue) -or (Read-Registration)) {
        throw 'Uninstall left its service or registration behind.'
    }
    if (!(Test-Path -LiteralPath $log)) { throw 'Uninstall did not preserve the log.' }

    # Empty worker selection must use the daemon's resolved default in both its SCM
    # command and description, while retaining the installer's startup choices.
    Run-Installer $installer ("/S /Service=1 /ServiceName=$serviceName /Port=$port " +
        "/Automatic=1 /Start=0 /EventLog=0 /LogFile=`"$log`" /D=$directory")
    $automatic = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    if ($automatic.State -ne 'Stopped' -or $automatic.StartMode -ne 'Auto') {
        throw 'Automatic startup without immediate service start was not retained.'
    }
    if ($automatic.PathName -notmatch ' -t ([1-9][0-9]*) -n ') {
        throw 'The installed service command does not contain the resolved worker count.'
    }
    $workers = $Matches[1]
    if ([int]$workers -gt 8) {
        throw 'Automatic service worker selection exceeded eight workers.'
    }
    $description = "lockd, an interprocess locking daemon, running on 'localhost' port $port " +
        "with $workers acquisition threads and log level 'info'. Windows Event Log output is disabled."
    if ($automatic.Description -ne $description) {
        throw ("The service description does not match the daemon worker default.`n" +
            "Expected: $description`nActual: $($automatic.Description)`n" +
            "Command: $($automatic.PathName)")
    }
    # Repeating the daemon's -S command legitimately changes the recorded options.
    # An installer upgrade must still recognize its executable and retain those
    # new options; uninstall must recognize the same registration afterwards.
    $installedDaemon = Join-Path $directory 'lockd.exe'
    $replacement = Start-Process -FilePath $installedDaemon -ArgumentList (
        "-S -n $serviceName -i localhost -p $port -t 3 -l warning -f `"$log`" --no-event-log"
    ) -PassThru -WindowStyle Hidden
    try {
        if (!$replacement.WaitForExit(120000) -or $replacement.ExitCode -ne 0) {
            throw 'CLI replacement of the installer-created service failed.'
        }
    }
    finally { $replacement.Dispose() }
    $reconfigured = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    if ($reconfigured.State -ne 'Stopped' -or $reconfigured.StartMode -ne 'Manual' -or
            $reconfigured.PathName -notmatch ' -t 3 .* -l warning ') {
        throw 'CLI replacement did not apply its options and fresh-install defaults.'
    }
    Run-Installer $installer "/S /D=$directory"
    $upgraded = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    if ($upgraded.PathName -ne $reconfigured.PathName -or $upgraded.State -ne 'Stopped' -or
            $upgraded.StartMode -ne 'Manual') {
        throw 'Installer upgrade did not retain the CLI-reconfigured service.'
    }
    Run-Uninstaller
    if ((Get-Service -Name $serviceName -ErrorAction SilentlyContinue) -or (Read-Registration)) {
        throw 'Uninstall left its automatic-startup service or registration behind.'
    }

    # Use a wildcard listener without starting it. Check actual NetSecurity filters,
    # preservation during upgrade, and removal by the recorded unique identifier.
    Run-Installer $installer ("/S /Service=1 /ServiceName=$serviceName /Address=0.0.0.0 " +
        "/Port=$port /Firewall=1 /Start=0 /EventLog=0 /LogFile=`"$log`" /D=$directory")
    $ruleName = Read-FirewallRuleName
    if (!$ruleName) { throw 'Firewall selection was not recorded.' }
    $rule = Get-NetFirewallRule -PolicyStore PersistentStore -Name $ruleName
    $application = Get-NetFirewallApplicationFilter -AssociatedNetFirewallRule $rule
    $filter = Get-NetFirewallPortFilter -AssociatedNetFirewallRule $rule
    if ($rule.Profile -ne 'Private' -or $rule.Direction -ne 'Inbound' -or
            $rule.Action -ne 'Allow' -or $rule.Enabled -ne 'True' -or
            $application.Program -ne (Join-Path $directory 'lockd.exe') -or
            $filter.Protocol -ne 'TCP' -or $filter.LocalPort -ne "$port") {
        throw 'The installed firewall rule does not match the selected program, port and profile.'
    }
    Run-Installer $installer "/S /D=$directory"
    if ((Read-FirewallRuleName) -ne $ruleName -or
            !(Get-NetFirewallRule -PolicyStore PersistentStore -Name $ruleName)) {
        throw 'Upgrade did not preserve the installer firewall rule.'
    }
    Run-Uninstaller
    if (Get-NetFirewallRule -PolicyStore PersistentStore | Where-Object { $_.Name -eq $ruleName }) {
        throw 'Uninstall left its firewall rule behind.'
    }

    # Exercise real Event Log registration and opt-in log removal on the disposable
    # runner. The helper must preserve its ownership records across an upgrade.
    $ownedLogs = Join-Path $env:ProgramData "lockd cleanup test $Architecture"
    $ownedLog = Join-Path $ownedLogs 'service.log'
    $eventSource = "HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\lockd.$serviceName"
    if ((Test-Path -LiteralPath $ownedLogs) -or (Test-Path -LiteralPath $eventSource)) {
        throw 'Cleanup verification requires an unused log directory and Event Log source.'
    }
    Run-Installer $installer ("/S /Service=1 /ServiceName=$serviceName /Port=$port " +
        "/Automatic=0 /EventLog=1 /LogFile=`"$ownedLog`" /D=$directory")
    if (!(Test-Path -LiteralPath $ownedLog) -or !(Test-Path -LiteralPath $eventSource)) {
        throw 'The service did not create its log and Event Log source.'
    }
    Set-Content -LiteralPath "$ownedLog.00001" -Value 'Numbered archive fixture'
    Run-Installer $installer "/S /D=$directory"
    Run-Uninstaller -RemoveLogs
    if ((Read-Registration) -or (Test-Path -LiteralPath $directory) -or
            (Test-Path -LiteralPath $ownedLogs) -or (Test-Path -LiteralPath $eventSource) -or
            (Get-Service -Name $serviceName -ErrorAction SilentlyContinue)) {
        throw 'Uninstall left installer-owned files, directories, service or Event Log registration behind.'
    }
}
finally {
    $base.Dispose()
}
