# Verify that a completed fixture does not wait for a persistent compiler child.
param([string]$Repository, [string]$WorkDirectory)
. "$PSScriptRoot/test_helpers.ps1"
New-FixtureDirectory $WorkDirectory
$identity = [Guid]::NewGuid().ToString('N')
$releaseName = "Local\lockd_console_release_$identity"
$readyName = "Local\lockd_console_ready_$identity"
$release = [Threading.EventWaitHandle]::new($false, 'ManualReset', $releaseName)
$ready = [Threading.EventWaitHandle]::new($false, 'ManualReset', $readyName)
$pidFile = Join-Path $WorkDirectory 'child.pid'
$driverFile = Join-Path $WorkDirectory 'driver.ps1'
$child = $null
try {
    # Keep the descendant alive until the assertion releases it. The timeout bounds
    # cleanup even when the launcher incorrectly waits for this process to exit.
    $childScript = @"
`$release = [Threading.EventWaitHandle]::OpenExisting('$releaseName')
`$ready = [Threading.EventWaitHandle]::OpenExisting('$readyName')
try {
    [void]`$ready.Set()
    [void]`$release.WaitOne(20000)
}
finally { `$ready.Dispose(); `$release.Dispose() }
"@
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($childScript))
    $driver = @'
$ErrorActionPreference = 'Stop'
$child = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') `
    -ArgumentList @('-NoProfile', '-NonInteractive', '-EncodedCommand', '@ENCODED@') `
    -WindowStyle Hidden -PassThru
try {
    [IO.File]::WriteAllText('@PID@', [string]$child.Id)
    $ready = [Threading.EventWaitHandle]::OpenExisting('@READY@')
    try {
        if (!$ready.WaitOne(10000)) { throw 'Descendant did not become ready' }
    }
    finally { $ready.Dispose() }
}
finally { $child.Dispose() }
& cmd /d /c 'exit 0'
exit $LASTEXITCODE
'@
    $driver = $driver.Replace('@ENCODED@', $encoded).Replace('@READY@', $readyName).
        Replace('@PID@', $pidFile.Replace("'", "''"))
    $driver | Set-Content -LiteralPath $driverFile -Encoding UTF8
    $launcher = Join-Path $Repository 'cmake/tests/windows/utf8_console.ps1'
    $result = Invoke-TestScript $launcher @('-Script', $driverFile,
            '-Log', (Join-Path $WorkDirectory 'launcher.log'))
    Assert-Exit $result 0
    $child = Get-Process -Id ([int][IO.File]::ReadAllText($pidFile)) -ErrorAction SilentlyContinue
    Assert-True ($null -ne $child -and !$child.HasExited) `
        'Console launcher waited for the descendant after its fixture shell exited'
}
finally {
    [void]$release.Set()
    try {
        if ($child) {
            if (!$child.WaitForExit(10000)) { throw 'Released descendant did not exit' }
            $child.Dispose()
        }
    }
    finally { $ready.Dispose(); $release.Dispose() }
}
Write-Output 'Console launcher returned while its controlled descendant was still running.'
