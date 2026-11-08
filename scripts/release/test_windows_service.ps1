param([Parameter(Mandatory)][string] $Directory)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true') {
    throw 'Run service lifecycle tests only on a disposable GitHub Actions runner.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try {
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Service lifecycle tests require an elevated runner.'
    }
}
finally { $identity.Dispose() }

$Directory = (Resolve-Path -LiteralPath $Directory).Path
# Exercise the actual service lifecycle from a Unicode installation directory as well as
# retaining a Unicode service name and log filename. These operations stay CI-only.
$unicode = [char]0x798f
$nativeDirectory = Join-Path $Directory ("service-$unicode")
New-Item -ItemType Directory -Path $nativeDirectory -ErrorAction Stop | Out-Null
Copy-Item -LiteralPath (Join-Path $Directory 'lockd.exe') -Destination $nativeDirectory
Get-ChildItem -LiteralPath $Directory -Filter '*.dll' | Copy-Item -Destination $nativeDirectory
$executable = Join-Path $nativeDirectory 'lockd.exe'
$log = Join-Path $nativeDirectory "service $unicode.log"
$serviceName = 'lockd_cli_test_' + $unicode + '_' + [Guid]::NewGuid().ToString('N')
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()
$expectedCommand = '"' + $executable + '" --as-service -i "127.0.0.1" -p ' + $port +
    ' -t 2 -n "' + $serviceName + '" -l debug --log-file "' + $log + '" --no-event-log'

function Run-Daemon([string] $Label, [string] $Arguments, [int] $Expected) {
    $stdout = Join-Path $Directory "$Label.stdout.log"
    $stderr = Join-Path $Directory "$Label.stderr.log"
    $process = Start-Process -FilePath $executable -ArgumentList $Arguments -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    try {
        if (!$process.WaitForExit(120000)) {
            $process.Kill()
            throw "Daemon command timed out: $Label"
        }
        $process.WaitForExit()
        Get-Content -LiteralPath $stdout, $stderr | Write-Host
        if ($process.ExitCode -ne $Expected) {
            throw "$Label returned $($process.ExitCode), expected $Expected."
        }
    }
    finally { $process.Dispose() }
}

function Stop-TestService {
    $controller = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
    if (!$controller) { return }
    try {
        if ($controller.Status -ne 'Stopped') {
            $controller.Stop()
            $controller.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(45))
        }
    }
    finally { $controller.Dispose() }
}

try {
    # Help must display the same default eventually persisted in the SCM command.
    # Requesting it must not install a service or require a writable log directory.
    $serviceLog = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'lockd/lockd.log'
    foreach ($mode in @('--as-service', '-A', '--install-service', '-S')) {
        Run-Daemon 'service-help' "$mode --help" 0
        if (!(Get-Content -LiteralPath (Join-Path $Directory 'service-help.stdout.log') -Raw).Contains($serviceLog)) {
            throw "Service help did not show the common-data log path for $mode."
        }
    }
    Run-Daemon 'console-help' '--help' 0
    $consoleLog = Join-Path $PWD 'lockd.log'
    if (!(Get-Content -LiteralPath (Join-Path $Directory 'console-help.stdout.log') -Raw).Contains($consoleLog)) {
        throw 'Console help did not retain the working-directory log default.'
    }

    Run-Daemon 'install' ("--install-service --service-name $serviceName --interface 127.0.0.1 " +
        "--port $port --threads 2 --log-level debug --log-file `"$log`" --no-event-log") 0
    $service = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    if (!$service -or $service.PathName -ne $expectedCommand -or
            $service.StartMode -ne 'Manual' -or $service.StartName -ne 'LocalSystem') {
        throw 'Installed service did not retain the selected command, account and startup mode.'
    }
    $description = "lockd, an interprocess locking daemon, running on '127.0.0.1' port $port " +
        "with 2 acquisition threads and log level 'debug'. Windows Event Log output is disabled."
    if ($service.Description -ne $description) { throw 'Service description does not match its options.' }

    # Reinstallation replaces daemon options and uses fresh-install SCM defaults,
    # including when an administrator changed the old startup mode.
    Set-Service -Name $serviceName -StartupType Automatic
    Run-Daemon 'replace-stopped' ("-S -n $serviceName -i 127.0.0.1 -p $port " +
        "-t 3 -l warning -f `"$log`" --no-event-log") 0
    $service = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    $expectedCommand = $expectedCommand.Replace(' -t 2 ', ' -t 3 ').Replace(' -l debug ', ' -l warning ')
    $description = $description.Replace('with 2 acquisition threads', 'with 3 acquisition threads').Replace("'debug'", "'warning'")
    if ($service.PathName -ne $expectedCommand -or $service.Description -ne $description -or
            $service.StartMode -ne 'Manual' -or $service.StartName -ne 'LocalSystem' -or
            $service.State -ne 'Stopped') {
        throw 'Stopped service replacement did not apply the selected options and fresh-install defaults.'
    }
    Start-Service -Name $serviceName
    $controller = Get-Service -Name $serviceName
    try { $controller.WaitForStatus('Running', [TimeSpan]::FromSeconds(30)) }
    finally { $controller.Dispose() }
    $client = [Net.Sockets.TcpClient]::new()
    try {
        if (!$client.ConnectAsync('127.0.0.1', $port).Wait(10000)) {
            throw 'The installed service did not accept a connection.'
        }
    }
    finally { $client.Dispose() }
    # Replacing a running service must stop its process before creating the new
    # registration. The replacement stays stopped until explicitly started.
    Run-Daemon 'replace-running' ("-S -n $serviceName -i 127.0.0.1 -p $port " +
        "-t 2 -l debug -f `"$log`" --no-event-log") 0
    $service = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    if ($service.State -ne 'Stopped' -or $service.PathName -ne
            $expectedCommand.Replace(' -t 3 ', ' -t 2 ').Replace(' -l warning ', ' -l debug ')) {
        throw 'Running service replacement did not stop the service and apply its new command.'
    }
    Start-Service -Name $serviceName
    $controller = Get-Service -Name $serviceName
    try { $controller.WaitForStatus('Running', [TimeSpan]::FromSeconds(30)) }
    finally { $controller.Dispose() }
    Stop-TestService
    if (!(Test-Path -LiteralPath $log)) { throw 'The service did not create the selected log file.' }

    # Uninstallation identifies the service by executable path, not by its name option.
    Run-Daemon 'uninstall' '-U' 0
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $remaining = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
        if (!$remaining) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($remaining) { throw 'Uninstallation left the service registered.' }
    Run-Daemon 'missing' '--uninstall-service' 8

    # Omitted --log-file must create and persist the same common-data path shown
    # by help, rather than recording the installer's current working directory.
    Run-Daemon 'install-default-log' ("-S -n $serviceName -i 127.0.0.1 " +
        "-p $port -t 1 --no-event-log") 0
    $service = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    if (!$service.PathName.Contains('--log-file "' + $serviceLog + '"') -or
            !(Test-Path -LiteralPath $serviceLog)) {
        throw 'Service installation did not create and persist its common-data log default.'
    }
    Start-Service -Name $serviceName
    $controller = Get-Service -Name $serviceName
    try { $controller.WaitForStatus('Running', [TimeSpan]::FromSeconds(30)) }
    finally { $controller.Dispose() }
    Stop-TestService
    Run-Daemon 'uninstall-default-log' '-U' 0
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $remaining = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
        if (!$remaining) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($remaining) { throw 'Removal left the default-log service registered.' }

    # A name collision must not authorize deleting another executable's service.
    $foreignCommand = '"' + (Join-Path $env:SystemRoot 'System32/cmd.exe') + '" /c exit'
    $foreign = New-Service -Name $serviceName -BinaryPathName $foreignCommand -StartupType Manual
    $foreign.Dispose()
    Run-Daemon 'foreign' "-S -n $serviceName" 7
    $service = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    if (!$service -or $service.PathName -ne $foreignCommand -or $service.State -ne 'Stopped') {
        throw 'Replacement changed a service belonging to another executable.'
    }
}
finally {
    # Recover only this test's uniquely named service if an assertion failed.
    $remaining = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
    if ($remaining) {
        Stop-TestService
        $result = Invoke-CimMethod -InputObject $remaining -MethodName Delete
        if ($result.ReturnValue -ne 0) { throw "Test service cleanup failed: $($result.ReturnValue)" }
    }
}
