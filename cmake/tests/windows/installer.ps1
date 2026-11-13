# Exercise installer options and lifecycle against isolated service/registry/firewall boundaries.
param([string] $Repository, [string] $WorkDirectory)
. "$PSScriptRoot/test_helpers.ps1"
$Repository = [IO.Path]::GetFullPath($Repository)
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$fixture = Join-Path $Repository 'cmake/tests/windows/installer_deployment_fixture.ps1'
$deploy = Join-Path $Repository 'platform/win32/installer/deploy.ps1'
$payload = Join-Path $WorkDirectory 'payload'
New-FixtureDirectory $payload
Assert-Exit (Invoke-TestScript $fixture @('-BuildProbe', '-Payload', $payload)) 0
foreach ($name in @('lockd_test.exe', 'license.txt', 'boost_license.txt', 'uninstall.exe')) {
    [IO.File]::WriteAllText((Join-Path $payload $name), 'isolated installer payload')
}

$cases = @(
    @('defaults', 'Success', @{}, 0),
    @('manual', 'Success', @{ Automatic = '0'; Start = '0'; Threads = '2'; Address = '::1'; EventLog = '0' }, 0),
    @('ipv4', 'Success', @{ Address = '127.0.0.1'; Threads = '3' }, 0),
    @('ARM64 registration', 'Success', @{ Address = '0.0.0.0'; Firewall = '1' }, 0),
    @('explicit worker override', 'Success', @{ Threads = '9' }, 0),
    @('private firewall', 'Success', @{ Address = '0.0.0.0'; Firewall = '1' }, 0),
    @('remote without firewall', 'Success', @{ Address = '::' }, 0),
    @('firewall failure', 'FirewallFailure', @{ Address = '::'; Firewall = '1' }, 1),
    @('registry failure', 'RegistryFailure', @{ Address = '::'; Firewall = '1' }, 1),
    @('firewall start rollback', 'StartFailure', @{ Address = '::'; Firewall = '1' }, 1),
    @('duplicate', 'Duplicate', @{}, 1),
    @('native failure', 'NativeFailure', @{}, 1),
    @('startup failure', 'StartupFailure', @{}, 1),
    @('start failure', 'StartFailure', @{}, 1),
    @('upgrade', 'Upgrade', @{}, 0),
    @('CLI reconfiguration', 'UpgradeReconfigured', @{}, 0),
    @('CLI reconfiguration beside native System.dll', 'UpgradeReconfigured', @{}, 0),
    @('firewall upgrade', 'UpgradeFirewall', @{}, 0),
    @('removed firewall stays removed', 'UpgradeFirewallMissing', @{}, 0),
    @('files only', 'Success', @{ Service = '0' }, 0)
)
foreach ($case in $cases) {
    $label, $scenario, $changes, $expected = $case
    Write-Output "Install: $label"
    $directory = Join-Path $WorkDirectory $label
    New-FixtureDirectory $directory
    if ($label -eq 'CLI reconfiguration beside native System.dll') {
        Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32/kernel32.dll') `
            -Destination (Join-Path $directory 'System.dll')
    }
    $target = Join-Path $directory 'installed application'
    $trace = Join-Path $directory 'state.json'
    $argumentsTrace = Join-Path $directory 'native arguments.txt'
    $options = Join-Path $directory 'options.ini'
    $settings = @{
        Service = '1'; Name = 'lockd_selected_name'; Address = 'localhost'; Port = '7199'
        Threads = ''; Level = 'debug'; Log = Join-Path $directory "log directory/a log's file.txt"
        EventLog = '1'; Automatic = '1'; Start = '1'; Firewall = '0'
    }
    foreach ($key in $changes.Keys) { $settings[$key] = $changes[$key] }
    Write-ServiceOptions $options $settings
    if ($scenario.StartsWith('Upgrade')) {
        New-FixtureDirectory $target
        Get-ChildItem -LiteralPath $payload -File | Copy-Item -Destination $target
    }
    $env:LOCKD_INSTALLER_PROBE_TRACE = $argumentsTrace
    $env:LOCKD_INSTALLER_PROBE_EXIT = if ($scenario -eq 'NativeFailure') { '7' } else { '0' }
    $architecture = if ($label -eq 'ARM64 registration') { 'arm64' } else { 'x86_64' }
    $result = Invoke-TestScript $fixture @('-DeployScript', $deploy, '-Directory', $target,
        '-Payload', $payload, '-Options', $options, '-Trace', $trace, '-ServiceName', $settings.Name,
        '-Architecture', $architecture, '-Scenario', $scenario) $directory
    Assert-Exit $result $expected
    $state = Get-Content -LiteralPath $trace -Raw | ConvertFrom-Json
    $invoked = $scenario -ne 'Duplicate' -and !$scenario.StartsWith('Upgrade') -and
        $settings.Service -eq '1'
    Assert-Equal (Test-Path -LiteralPath $argumentsTrace) $invoked "$label daemon invocation"
    $created = $settings.Firewall -eq '1' -and $invoked
    Assert-Equal @($state.FirewallCreated).Count ([int] $created) "$label firewall creation"
    Assert-Equal @($state.FirewallDeleted).Count ([int] ($created -and $expected -ne 0)) `
        "$label firewall rollback"
    if ($created) {
        $rule = $state.FirewallCreated[0]
        foreach ($pair in @(@('PolicyStore', 'PersistentStore'), @('Protocol', 'TCP'),
                @('Profile', 'Private'), @('Direction', 'Inbound'), @('Action', 'Allow'),
                @('Enabled', 'True'), @('EdgeTraversalPolicy', 'Block'))) {
            Assert-Equal $rule.($pair[0]) $pair[1] "$label firewall $($pair[0])"
        }
        Assert-Equal $rule.Program (Join-Path $target 'lockd.exe') "$label firewall program"
        Assert-Equal $rule.LocalPort $settings.Port "$label firewall port"
        if (!$expected) { Assert-Equal $state.Registry.FirewallRule $rule.Name "$label recorded rule" }
    }
    Assert-True (@($state.FirewallRules.Name) -contains 'unrelated-rule') "$label unrelated rule"
    if ($scenario -in @('UpgradeFirewall', 'UpgradeFirewallMissing')) {
        Assert-Equal $state.Registry.FirewallRule `
            'lockd-x86_64-11111111-2222-3333-4444-555555555555' "$label retained rule identity"
    }
    elseif (!$created) {
        Assert-True ($null -eq $state.Registry.PSObject.Properties['FirewallRule']) `
            "$label unexpected firewall registration"
    }
    if ($architecture -eq 'arm64') {
        Assert-Equal $state.Registry.DisplayName 'lockd (arm64)' "$label display name"
        Assert-True $state.Registry.FirewallRule.StartsWith('lockd-arm64-') "$label rule architecture"
    }
    if ($invoked) {
        $arguments = @('--install-service', '--service-name', $settings.Name,
            '--interface', $settings.Address, '--port', $settings.Port,
            '--log-level', $settings.Level, '--log-file', $settings.Log)
        if ($settings.Threads) { $arguments += @('--threads', $settings.Threads) }
        if ($settings.EventLog -eq '0') { $arguments += '--no-event-log' }
        Assert-Sequence @(Get-Content -LiteralPath $argumentsTrace -Encoding UTF8) $arguments `
            "$label native argument boundaries"
    }
    if ($expected) {
        Assert-Equal $state.BinaryExists $false "$label binary rollback"
        $deleted = if ($scenario -in @('StartupFailure', 'StartFailure', 'FirewallFailure',
                'RegistryFailure')) { @($settings.Name) } else { @() }
        Assert-Sequence $state.Deleted $deleted "$label service rollback"
        Assert-Equal @($state.Registry.PSObject.Properties).Count 0 "$label registry rollback"
        if ($scenario -eq 'Duplicate') {
            Assert-True $result.Output.Contains('service name is already in use') "$label diagnostic"
        }
        if ($scenario -eq 'NativeFailure') {
            Assert-True $result.Output.Contains('lockd.exe returned 7') "$label diagnostic"
        }
        continue
    }
    Assert-Equal $state.BinaryExists $true "$label installed binary"
    $cleanup = $state.Registry.CleanupState | ConvertFrom-Json
    Assert-Equal (@($cleanup.Directories) -contains $target) (!$scenario.StartsWith('Upgrade')) `
        "$label installation ownership"
    if (!$scenario.StartsWith('Upgrade')) {
        $logs = if ($settings.Service -eq '1') { @($settings.Log) } else { @() }
        $sources = if ($settings.Service -eq '1') { @($settings.Name) } else { @() }
        Assert-Sequence $cleanup.LogFiles $logs "$label log ownership"
        Assert-Sequence $cleanup.EventSources $sources "$label Event Log ownership"
    }
    if ($settings.Service -eq '0') {
        Assert-Equal $state.Registry.ServiceName '' "$label service name"
        Assert-Equal $state.Registry.ServiceCommand '' "$label service command"
        Assert-Sequence $state.Startup @() "$label startup"
        Assert-Sequence $state.Started @() "$label start"
    }
    else {
        Assert-Equal $state.Registry.ServiceName $settings.Name "$label service name"
        $command = if ($scenario -eq 'UpgradeReconfigured') {
            '"' + (Join-Path $target 'lockd.exe') + '" --as-service --updated-options'
        } else { '"command supplied by lockd.exe" --daemon-owned-options' }
        Assert-Equal $state.Registry.ServiceCommand $command "$label service command"
        if ($scenario.StartsWith('Upgrade')) {
            Assert-Sequence $state.Startup @() "$label retained startup"
        }
        else {
            Assert-Equal @($state.Startup).Count 1 "$label startup count"
            Assert-Equal $state.Startup[0].Name $settings.Name "$label startup service"
            $mode = if ($settings.Automatic -eq '1') { 'Automatic' } else { 'Manual' }
            Assert-Equal $state.Startup[0].Mode $mode "$label startup policy"
        }
        $started = if ($settings.Start -eq '1') { @($settings.Name) } else { @() }
        $stopped = if ($scenario.StartsWith('Upgrade')) { @($settings.Name) } else { @() }
        Assert-Sequence $state.Started $started "$label start"
        Assert-Sequence $state.Stopped $stopped "$label upgrade stop"
    }
}

# Removal must preserve unowned data and retain cleanup state when an operation fails.
foreach ($case in @(@('OwnedEventSource', '0', 0), @('OwnedEventSource', '1', 0),
        @('PreexistingEventSource', '1', 0), @('UnrelatedFiles', '1', 0),
        @('ForeignEventSource', '1', 1), @('SourceReadFailure', '1', 1),
        @('LockedLog', '1', 1), @('LockedLogRetry', '1', 0), @('RedirectedLogs', '1', 1),
        @('InvalidDirectory', '1', 1), @('PruneFailureEventSource', '1', 1),
        @('PruneRetryEventSource', '1', 0))) {
    $scenario, $removeLogs, $expected = $case
    Write-Output "Uninstall: $scenario $removeLogs"
    $directory = Join-Path $WorkDirectory ($scenario + $removeLogs)
    $target = Join-Path $directory 'app1'
    $logs = Join-Path $directory 'logs'
    New-FixtureDirectory $target
    New-FixtureDirectory $logs
    foreach ($name in @('lockd.exe', 'lockd_test.exe', 'license.txt', 'boost_license.txt', 'uninstall.exe')) {
        [IO.File]::WriteAllText((Join-Path $target $name), 'installed fixture')
    }
    $log = Join-Path $logs 'a [log].txt'
    $archive = $log + '.00001'
    [IO.File]::WriteAllText($log, 'log fixture')
    [IO.File]::WriteAllText($archive, 'log fixture')
    $extra = Join-Path $target 'notes.txt'
    $externalArchive = $log + '.old'
    if ($scenario -eq 'UnrelatedFiles') {
        [IO.File]::WriteAllText($extra, 'user data')
        [IO.File]::WriteAllText($externalArchive, 'external rotation')
    }
    $preexisting = $scenario -eq 'PreexistingEventSource'
    $cleanup = @{
        Directories = @()
        LogFiles = @($log)
        EventSources = @()
    }
    if (!$preexisting) {
        $cleanup.Directories = @($target, $logs)
        $cleanup.EventSources = @('lockd_cleanup_test')
    }
    $outside = Join-Path $directory 'unrelated-empty-directory'
    if ($scenario -eq 'InvalidDirectory') {
        New-FixtureDirectory $outside
        $cleanup.Directories += $outside
    }
    $metadata = Join-Path $directory 'cleanup.json'
    $cleanup | ConvertTo-Json | Set-Content -LiteralPath $metadata -Encoding UTF8
    $trace = Join-Path $directory 'state.json'
    $arguments = @('-DeployScript', $deploy, '-Operation', 'Uninstall', '-Directory', $target,
        '-Trace', $trace, '-ServiceName', 'lockd_cleanup_test', '-Scenario', $scenario,
        '-CleanupStateFile', $metadata, '-RemoveLogs', $removeLogs)
    if ($scenario -in @('LockedLogRetry', 'PruneRetryEventSource')) { $arguments += '-RetryCleanup' }
    $result = Invoke-TestScript $fixture $arguments
    Assert-Exit $result $expected
    $state = Get-Content -LiteralPath $trace -Raw | ConvertFrom-Json
    Assert-Equal $state.RegistryExists ([bool] $expected) "$scenario retained registration"
    if ($scenario -in @('LockedLogRetry', 'PruneRetryEventSource')) {
        Assert-Sequence $state.CleanupAttempts @(1, 0) "$scenario retry"
        Assert-Sequence $state.Deleted @('lockd_cleanup_test') "$scenario service deletion"
    }
    if ($expected) {
        if ($scenario -in @('ForeignEventSource', 'SourceReadFailure')) {
            Assert-Sequence $state.Deleted @() "$scenario protected service"
            Assert-Sequence $state.EventSourcesDeleted @() "$scenario protected Event Log"
            Assert-True (Test-Path -LiteralPath (Join-Path $target 'lockd.exe') -PathType Leaf) `
                "$scenario protected binary"
        }
        if ($scenario -eq 'InvalidDirectory') {
            Assert-True (Test-Path -LiteralPath $outside -PathType Container) "$scenario protected path"
            Assert-True $result.Output.Contains('outside this installation') "$scenario diagnostic"
        }
        elseif ($scenario -ne 'PruneFailureEventSource') {
            Assert-True (Test-Path -LiteralPath $log -PathType Leaf) "$scenario retained log"
            Assert-True (Test-Path -LiteralPath $archive -PathType Leaf) "$scenario retained archive"
        }
        $uninstaller = Join-Path $target 'uninstall.exe'
        Assert-True (Test-Path -LiteralPath $uninstaller -PathType Leaf) "$scenario retained uninstaller"
        Assert-FileBytes $uninstaller ([Text.Encoding]::UTF8.GetBytes('installed fixture')) `
            "$scenario uninstaller data"
        continue
    }
    Assert-Equal (Test-Path -LiteralPath (Join-Path $target 'uninstall.exe')) $false "$scenario uninstaller"
    Assert-Equal (Test-Path -LiteralPath $log) ($removeLogs -eq '0') "$scenario log consent"
    Assert-Equal (Test-Path -LiteralPath $archive) ($removeLogs -eq '0') "$scenario archive consent"
    if ($scenario -eq 'UnrelatedFiles') {
        Assert-FileBytes $extra ([Text.Encoding]::UTF8.GetBytes('user data')) "$scenario unowned file"
        Assert-FileBytes $externalArchive ([Text.Encoding]::UTF8.GetBytes('external rotation')) `
            "$scenario old rotation"
    }
    Assert-Equal (Test-Path -LiteralPath $target) ($preexisting -or $scenario -eq 'UnrelatedFiles') `
        "$scenario target ownership"
    Assert-Equal (Test-Path -LiteralPath $logs) `
        ($preexisting -or $scenario -eq 'UnrelatedFiles' -or $removeLogs -eq '0') "$scenario log directory"
    Assert-Equal (@($state.EventSources.PSObject.Properties).Count -gt 0) $preexisting `
        "$scenario Event Log ownership"
}

$directory = Join-Path $WorkDirectory 'validation'
New-FixtureDirectory $directory
$target = Join-Path $directory 'installation'
$options = Join-Path $directory 'options.ini'
$settings = @{
    Service = '1'; Name = 'lockd_validation_test'; Address = '127.0.0.1'; Port = '7198'
    Threads = ''; Level = 'info'; Log = Join-Path $directory 'a log.txt'
    EventLog = '1'; Automatic = '1'; Start = '1'; Firewall = '0'
}
$operations = @(@('Validate', @{}, 0), @('Validate', @{ Service = '0' }, 0),
    @('Validate', @{ Address = '::1' }, 0), @('Validate', @{ Address = 'localhost' }, 0),
    @('Validate', @{ Address = 'lockd.example.com' }, 0),
    @('Validate', @{ Address = 'host" --extra' }, 1), @('Validate', @{ Port = '0' }, 1),
    @('Validate', @{ Port = '65536' }, 1), @('Validate', @{ Threads = '-1' }, 1),
    @('Validate', @{ Address = 'not an address' }, 1), @('Validate', @{ Name = 'name" --extra' }, 1),
    @('Validate', @{ Log = 'relative.log' }, 1), @('Validate', @{ Level = 'invalid' }, 1),
    @('Validate', @{ Automatic = 'yes' }, 1), @('Validate', @{ Firewall = 'yes' }, 1),
    @('Validate', @{ Service = '0'; Firewall = '1' }, 1),
    @('Validate', @{ Address = '0.0.0.0'; Firewall = '1' }, 0),
    @('Validate', @{ Address = '::'; Firewall = '1' }, 0),
    @('Validate', @{ Address = 'lockd.example.com'; Firewall = '1' }, 0))
$loopbacks = @('localhost', 'LOCALHOST', 'localhost.', '127.0.0.1', '127.12.34.56',
    '::1', '::1%1', '0:0:0:0:0:0:0:1', '::ffff:127.0.0.1')
foreach ($address in $loopbacks) {
    $operations += ,@('Validate', @{ Address = $address; Firewall = '1' }, 1)
    $operations += ,@('FirewallAvailable', @{ Address = $address }, 2)
}
foreach ($address in @('0.0.0.0', '::', '192.0.2.1', 'lockd.example.com')) {
    $operations += ,@('FirewallAvailable', @{ Address = $address }, 0)
}
foreach ($case in $operations) {
    $operation, $changes, $expected = $case
    $values = @{} + $settings
    foreach ($key in $changes.Keys) { $values[$key] = $changes[$key] }
    Write-ServiceOptions $options $values
    $env:LOCKD_INSTALLER_PROBE_TRACE = Join-Path $directory 'unused-probe.txt'
    $result = Invoke-TestScript $fixture @('-DeployScript', $deploy, '-Operation', $operation,
        '-Directory', $target, '-Options', $options, '-Trace', (Join-Path $directory 'state.json'),
        '-ServiceName', $settings.Name)
    Assert-Exit $result $expected
    Assert-Equal (Test-Path -LiteralPath $target) $false "$operation must not install files"
}

foreach ($case in @(@('UninstallFirewall', 'Uninstall', 0), @('Success', 'Uninstall', 0),
        @('UpgradeFirewallForeign', 'Install', 1), @('UpgradeForeignCommand', 'Install', 1))) {
    $scenario, $operation, $expected = $case
    $directory = Join-Path $WorkDirectory "firewall-$scenario"
    $target = Join-Path $directory 'installation'
    New-FixtureDirectory $target
    foreach ($name in @('lockd.exe', 'lockd_test.exe', 'license.txt', 'boost_license.txt', 'uninstall.exe')) {
        [IO.File]::WriteAllText((Join-Path $target $name), 'existing installation')
    }
    $options = Join-Path $directory 'options.ini'
    Write-ServiceOptions $options @{ Service = '0' }
    $trace = Join-Path $directory 'state.json'
    $result = Invoke-TestScript $fixture @('-DeployScript', $deploy, '-Operation', $operation,
        '-Directory', $target, '-Options', $options, '-Trace', $trace,
        '-ServiceName', 'lockd_cleanup_test', '-Scenario', $scenario)
    Assert-Exit $result $expected
    $state = Get-Content -LiteralPath $trace -Raw | ConvertFrom-Json
    Assert-True (@($state.FirewallRules.Name) -contains 'unrelated-rule') "$scenario unrelated firewall"
    Assert-Sequence $state.FirewallCreated @() "$scenario firewall creation"
    $deleted = if ($scenario -eq 'UninstallFirewall') {
        @('lockd-x86_64-11111111-2222-3333-4444-555555555555')
    } else { @() }
    Assert-Sequence $state.FirewallDeleted $deleted "$scenario firewall ownership"
    Assert-Equal $state.BinaryExists ([bool] $expected) "$scenario protected binary"
    $deleted = if (!$expected) { @('lockd_cleanup_test') } else { @() }
    Assert-Sequence $state.Deleted $deleted "$scenario service ownership"
}
Write-Output 'Installer deployment, rollback, validation and cleanup scenarios passed.'
