# Windows-only launcher for MSVC fixtures whose dependency paths require UTF-8.
param(
    [Parameter(Mandatory = $true)][string]$Log,
    [Parameter(Mandatory = $true)][string]$Script
)
$ErrorActionPreference = 'Stop'

# ShellExecute gives the child a separate console. Changing its output encoding must
# not change an IDE's or developer's console while other tests or builds are running.
function Quote-Literal([string]$Value) {
    return "'" + $Value.Replace("'", "''") + "'"
}

$encodedScript = '[Console]::InputEncoding = [Text.UTF8Encoding]::new($false); ' +
    '[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false); & ' +
    (Quote-Literal $Script) + ' 2>&1 | Out-File -Encoding utf8 -LiteralPath ' +
    (Quote-Literal $Log) + '; if ($null -eq $LASTEXITCODE) { exit 1 }; exit $LASTEXITCODE'
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($encodedScript))
[IO.Directory]::CreateDirectory((Split-Path -Parent $Log)) | Out-Null
if (Test-Path -LiteralPath $Log) {
    Remove-Item -LiteralPath $Log
}
$process = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') `
    -ArgumentList @('-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
        '-EncodedCommand', $encoded) `
    -WindowStyle Hidden -PassThru
# Start-Process -Wait also waits for descendants, including MSVC's persistent PDB
# server. Only the fixture shell's exit determines whether its build finished.
try {
    $null = $process.Handle
    $process.WaitForExit()
    $exitCode = $process.ExitCode
}
finally { $process.Dispose() }
[Console]::Out.Write([IO.File]::ReadAllText($Log))
exit $exitCode
