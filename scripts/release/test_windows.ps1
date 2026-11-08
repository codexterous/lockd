param([Parameter(Mandatory)][string] $DistributionDirectory, [string] $Python = 'python',
      [string] $WheelDirectory = $DistributionDirectory,
      [switch] $SkipNativeTests)

$ErrorActionPreference = 'Stop'
$details = & $Python "$PSScriptRoot/package.py" interpreter
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the selected Python interpreter.' }
$interpreter = $details | ConvertFrom-Json
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$paths = & $Python "$PSScriptRoot/output_layout.py" --architecture $interpreter.architecture
if ($LASTEXITCODE -ne 0) { throw 'Could not select the wheel verification directory.' }
$layout = $paths | ConvertFrom-Json
$work = Join-Path $layout.verifications "release_distributions/$($interpreter.tag)"
New-Item -ItemType Directory -Force $work | Out-Null
$env:TEMP = $work
$env:TMP = $work
$env:PYTHONDONTWRITEBYTECODE = '1'

function Invoke-Checked {
    param([Parameter(Mandatory)][string] $Command, [string[]] $Arguments)
    Get-Command $Command -ErrorAction Stop | Out-Null
    # Boost.Test and unittest write successful reports to stderr too.
    $ErrorActionPreference = 'Continue'
    & $Command @Arguments 2>&1 | ForEach-Object { Write-Output "$_" }
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}

$wheels = @(Get-ChildItem -LiteralPath $WheelDirectory -Filter "*-$($interpreter.tag)-$($interpreter.tag)-$($interpreter.wheel_platform).whl")
if ($wheels.Count -ne 1) {
    throw "Expected one ordinary CPython $($interpreter.version) $($interpreter.architecture) wheel."
}

# ARM64 wheels use the native daemon. The x64 interpreter also checks the x86
# portable package once, since Windows x64 can execute both architectures.
$daemonArchitecture = if ($interpreter.architecture -eq 'arm64') { 'arm64' } else { 'x86_64' }
$architectures = if ($SkipNativeTests -or $daemonArchitecture -eq 'arm64') {
    @($daemonArchitecture)
} else { @('x86', 'x86_64') }
foreach ($architecture in $architectures) {
    $archives = @(Get-ChildItem -LiteralPath $DistributionDirectory -Filter "*-windows-$architecture-portable.zip")
    if ($archives.Count -ne 1) { throw "Expected one Windows $architecture archive." }
    $executables = Join-Path $work "portable/$architecture"
    Expand-Archive -LiteralPath $archives[0].FullName -DestinationPath $executables -Force
}
$env:LOCKD_TEST_DAEMON = Join-Path $work "portable/$daemonArchitecture/lockd.exe"
Invoke-Checked $Python @('-m', 'venv', (Join-Path $work 'env'))
$testPython = Join-Path $work 'env/Scripts/python.exe'
Invoke-Checked $testPython @('-m', 'pip', 'install', '--no-index', '--no-deps', '--force-reinstall', $wheels[0].FullName)

# Run away from both the executables and source. This catches build-tree path assumptions,
# missing bundled DLLs, and accidental imports from a locally built extension.
Push-Location $work
try {
    # The portable executables don't vary with Python. CI checks them separately;
    # every wheel still runs all Python tests against its selected packaged daemon.
    if (!$SkipNativeTests) {
        foreach ($architecture in $architectures) {
            $executables = Join-Path $work "portable/$architecture"
            Invoke-Checked (Join-Path $executables 'lockd.exe') @('--version')
            Invoke-Checked (Join-Path $executables 'lockd_test.exe') @('--report_level=short')

            # cmd passes a bare argv[0], as when running the extracted archive by hand.
            # Keep the existing absolute-path run above to cover a different working directory.
            Push-Location $executables
            try {
                Invoke-Checked cmd.exe @('/d', '/c', 'lockd_test.exe --report_level=short')
            }
            finally {
                Pop-Location
            }
        }
    }

    Invoke-Checked $testPython @('-I', '-c', 'import pylockd; print(pylockd.__file__)')
    Invoke-Checked $testPython @(
        '-m', 'unittest', 'discover', '-s', (Join-Path $repositoryRoot 'lib/pylockd/tests'), '-v'
    )
}
finally {
    Pop-Location
}
