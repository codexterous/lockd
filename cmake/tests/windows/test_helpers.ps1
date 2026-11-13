# Shared assertions and bounded child processes for Windows packaging regressions.
$ErrorActionPreference = 'Stop'

function Assert-Equal($Actual, $Expected, [string] $Context) {
    # Keep JSON strings distinct from numbers and booleans despite PowerShell's coercion.
    $differentKind = ($Actual -is [string]) -ne ($Expected -is [string])
    if ($differentKind -or $Actual -cne $Expected) {
        throw "$Context - expected [$Expected], received [$Actual]"
    }
}

function Assert-True([bool] $Condition, [string] $Context) {
    if (!$Condition) { throw $Context }
}

function Assert-Sequence([object[]] $Actual, [object[]] $Expected, [string] $Context) {
    Assert-Equal $Actual.Count $Expected.Count "$Context count"
    for ($index = 0; $index -lt $Expected.Count; ++$index) {
        Assert-Equal $Actual[$index] $Expected[$index] "$Context at $index"
    }
}

function Assert-FileBytes([string] $Path, [byte[]] $Expected, [string] $Context) {
    Assert-Sequence ([IO.File]::ReadAllBytes($Path)) $Expected $Context
}

function Quote-NativeArgument([string] $Value) {
    # CreateProcess receives a command line. Preserve quotes and trailing backslashes
    # so paths containing spaces reach Windows PowerShell as one exact argument.
    $quoted = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $quoted = [regex]::Replace($quoted, '(\\+)$', '$1$1')
    return '"' + $quoted + '"'
}

function Invoke-TestProcess([string] $Command, [string[]] $Arguments,
        [string] $Directory = $WorkDirectory, [hashtable] $Environment = @{}) {
    $process = [Diagnostics.Process]::new()
    $process.StartInfo.FileName = $Command
    $process.StartInfo.Arguments = ($Arguments | ForEach-Object { Quote-NativeArgument $_ }) -join ' '
    $process.StartInfo.WorkingDirectory = $Directory
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    foreach ($name in $Environment.Keys) {
        if ($null -eq $Environment[$name]) {
            $process.StartInfo.EnvironmentVariables.Remove($name)
        }
        else { $process.StartInfo.EnvironmentVariables[$name] = [string] $Environment[$name] }
    }
    try {
        [void] $process.Start()
        $output = $process.StandardOutput.ReadToEndAsync()
        $errorOutput = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(30000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Timed out running $Command $Arguments"
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = $output.Result
            Error = $errorOutput.Result
        }
    }
    finally { $process.Dispose() }
}

function Invoke-TestScript([string] $Script, [string[]] $Arguments = @(),
        [string] $Directory = $WorkDirectory, [hashtable] $Environment = @{}) {
    return Invoke-TestProcess (Join-Path $PSHOME 'powershell.exe') `
        (@('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $Script) +
            $Arguments) $Directory $Environment
}

function Assert-Exit($Result, [int] $Expected) {
    Assert-Equal $Result.ExitCode $Expected ($Result.Output + $Result.Error)
}

function New-FixtureDirectory([string] $Path) {
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Write-ServiceOptions([string] $Path, [System.Collections.IDictionary] $Values) {
    $lines = @('[Service]') + @($Values.Keys | ForEach-Object { "$_=$($Values[$_])" })
    [IO.File]::WriteAllLines($Path, $lines, [Text.Encoding]::Unicode)
}
