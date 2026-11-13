# Exercise compiler initialization, tool caches and analysis using recorded external boundaries.
param([string] $Repository, [string] $WorkDirectory)
. "$PSScriptRoot/test_helpers.ps1"
$Repository = [IO.Path]::GetFullPath($Repository)
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$root = Join-Path $WorkDirectory 'environment'
$script = Join-Path $root 'scripts/release/windows_environment.ps1'
New-FixtureDirectory (Split-Path -Parent $script)
Copy-Item -LiteralPath (Join-Path $Repository 'scripts/release/windows_environment.ps1') `
    -Destination $script
$programFiles = Join-Path $root 'program-files'
$vswhere = Join-Path $programFiles 'Microsoft Visual Studio/Installer/vswhere.exe'
New-FixtureDirectory (Split-Path -Parent $vswhere)
$fixture = Join-Path $root 'developer_shell.ps1'
@'
# Load the standard commands before replacing Import-Module at the compiler boundary.
#Requires -Modules Microsoft.PowerShell.Management, Microsoft.PowerShell.Utility
param($VsWhere, $EnvironmentScript, $ProgramFiles)
$ErrorActionPreference = 'Stop'
$processMachineBefore = $env:PROCESSOR_ARCHITECTURE
$nativeMachineBefore = $env:PROCESSOR_ARCHITEW6432
# Keep the shell's startup environment and processor identity real. Only the
# installed-compiler location belongs to this fixture's external discovery boundary.
${env:ProgramFiles(x86)} = $ProgramFiles
$script:discovered = $false
function Find-FixtureVisualStudio {
    param([switch]$latest, $products, $requires, $property)
    $component = if ($env:LOCKD_TEST_ARCH -eq 'arm64') { 'ARM64' } else { 'x86.x64' }
    if (!$latest -or $products -ne '*' -or $property -ne 'installationPath' -or
            $requires -ne "Microsoft.VisualStudio.Component.VC.Tools.$component") {
        throw 'Wrong Visual Studio discovery arguments'
    }
    $script:discovered = $true
    $global:LASTEXITCODE = 0
    return $env:LOCKD_TEST_VS
}
# Record the external discovery command in this process. Compiling and executing a
# synthetic vswhere.exe adds unrelated compiler/runtime startup to host-selection tests.
Set-Alias -Name $VsWhere -Value Find-FixtureVisualStudio
$script:entered = $false
function Import-Module { param($Name) }
function Enter-VsDevShell {
    param($VsInstallPath, [switch] $SkipAutomaticLocation, $DevCmdArguments)
    if ($VsInstallPath -ne $env:LOCKD_TEST_VS) { throw 'Wrong compiler' }
    $env:VCPKG_ROOT = $env:LOCKD_TEST_BUNDLED_VCPKG
    $script:entered = $true
    $script:arguments = $DevCmdArguments
}
# Add the discovery fixture after setup has loaded the standard PowerShell commands.
# Only the production executable lookup should encounter this unrelated module.
$env:PSModulePath = "$env:LOCKD_TEST_MODULE_PATH;$env:PSModulePath"
. $EnvironmentScript -Architecture $env:LOCKD_TEST_ARCH
if ($env:PROCESSOR_ARCHITECTURE -cne $processMachineBefore -or
        $env:PROCESSOR_ARCHITEW6432 -cne $nativeMachineBefore) {
    throw 'Compiler initialization changed the process identity'
}
@{ root = $env:VCPKG_ROOT; entered = $script:entered; arguments = $script:arguments
    discovered = $script:discovered } | ConvertTo-Json
'@ | Set-Content -LiteralPath $fixture -Encoding UTF8
$user = Join-Path $root 'user-vcpkg'
$runner = Join-Path $root 'runner-vcpkg'
$bundled = Join-Path $root 'visual-studio/VC/vcpkg'
$pathRoot = Join-Path $root 'path-vcpkg'
New-FixtureDirectory $pathRoot
[IO.File]::WriteAllText((Join-Path $pathRoot 'vcpkg.exe'), '')
$decoyRoot = Join-Path $root 'decoy-vcpkg'
New-FixtureDirectory $decoyRoot
[IO.File]::WriteAllText((Join-Path $decoyRoot 'vcpkg.exe.exe'), '')

# A native executable probe must neither evaluate unrelated module manifests nor
# import their modules. Wildcard exports require metadata analysis; Write-Host makes
# that evaluation observable without waiting for a timeout or importing the module.
$moduleRoot = Join-Path $root 'modules'
$module = Join-Path $moduleRoot 'vcpkg_discovery_probe'
New-FixtureDirectory $module
@'
@{
    ModuleVersion = '1.0'
    Description = (Write-Host 'Unexpected PowerShell module discovery')
    RootModule = 'vcpkg_discovery_probe.psm1'
    FunctionsToExport = @('*')
    CmdletsToExport = @()
    AliasesToExport = @()
}
'@ | Set-Content -LiteralPath (Join-Path $module 'vcpkg_discovery_probe.psd1') -Encoding UTF8
@'
[IO.File]::WriteAllText($env:LOCKD_TEST_VCPKG_MODULE_TRACE, 'imported')
throw 'Native vcpkg discovery must not import a PowerShell module'
function vcpkg.exe {}
'@ | Set-Content -LiteralPath (Join-Path $module 'vcpkg_discovery_probe.psm1') -Encoding UTF8
$moduleTrace = Join-Path $root 'module-imported.txt'
Remove-Item -LiteralPath $moduleTrace -ErrorAction SilentlyContinue
$fixturePath = ($env:PATH.Split(';') | Where-Object {
    $_ -and !(Test-Path -LiteralPath (Join-Path $_ 'vcpkg.exe'))
}) -join ';'
# Native x64 and ARM64 CI hosts each exercise every target. Model host classification
# below using data inputs rather than changing the running PowerShell host's identity.
$hostMachine = if ($env:PROCESSOR_ARCHITEW6432) {
    $env:PROCESSOR_ARCHITEW6432
} else { $env:PROCESSOR_ARCHITECTURE }
$expectedHost = @{ AMD64 = 'x86_64'; ARM64 = 'arm64'; x86 = 'x86' }[$hostMachine]
Assert-True ($null -ne $expectedHost) "Unsupported test host: $hostMachine"
foreach ($architecture in @('x86', 'x86_64', 'arm64')) {
    $compilerTarget = if ($architecture -eq 'x86_64') { 'x64' } else { $architecture }
    foreach ($initialized in @($false, $true)) {
        foreach ($selected in @('unspecified', 'user', 'runner', 'path')) {
            $environment = @{
                VCPKG_ROOT = $null; VCPKG_INSTALLATION_ROOT = $null; LOCKD_HOST_BUILD_DIR = ''
                LOCKD_MSVC_TOOLSET_VERSION = $(if ($architecture -eq 'arm64') { '14.44' } else { '' })
                VCToolsVersion = $(if ($initialized) { '14.44.35207' } else { '14.51.36247' })
                PATH = $fixturePath
                LOCKD_TEST_MODULE_PATH = $moduleRoot
                LOCKD_TEST_VCPKG_MODULE_TRACE = $moduleTrace
                VSCMD_ARG_TGT_ARCH = $(if ($initialized) { $compilerTarget } else { 'other' })
                LOCKD_TEST_ARCH = $architecture; LOCKD_TEST_VS = Join-Path $root 'visual-studio'
                LOCKD_TEST_BUNDLED_VCPKG = $bundled
            }
            if ($selected -eq 'user') { $environment.VCPKG_ROOT = $user }
            if ($selected -ne 'unspecified') { $environment.PATH = "$pathRoot;$fixturePath" }
            if ($selected -eq 'path') { $environment.PATH = "$decoyRoot;$pathRoot;$fixturePath" }
            if ($selected -in @('user', 'runner')) { $environment.VCPKG_INSTALLATION_ROOT = $runner }
            $result = Invoke-TestScript $fixture @('-VsWhere', $vswhere,
                    '-EnvironmentScript', $script, '-ProgramFiles', $programFiles) `
                -Environment $environment
            Assert-True (!$result.Output.Contains('Unexpected PowerShell module discovery')) `
                "$architecture/$initialized/$selected native discovery evaluated a module manifest"
            Assert-Equal (Test-Path -LiteralPath $moduleTrace) $false `
                "$architecture/$initialized/$selected native discovery imported a PowerShell module"
            Assert-Exit $result 0
            $actual = $result.Output | ConvertFrom-Json
            $expected = switch ($selected) {
                user { $user }
                runner { $runner }
                path { $pathRoot }
                default { if ($initialized) { '' } else { $bundled } }
            }
            Assert-Equal ([string] $actual.root) $expected "$architecture $selected vcpkg selection"
            Assert-Equal $actual.discovered (!$initialized) "$architecture compiler discovery"
            Assert-Equal $actual.entered (!$initialized) "$architecture compiler initialization"
            if (!$initialized) {
                $compilerHost = if ($hostMachine -eq 'ARM64') { 'arm64' } else { 'x64' }
                $arguments = "-arch=$compilerTarget -host_arch=$compilerHost"
                if ($architecture -eq 'arm64') { $arguments += ' -vcvars_ver=14.44' }
                Assert-Equal $actual.arguments $arguments `
                    "$hostMachine to $architecture developer shell arguments"
            }
        }
    }
}

$source = Get-Content -LiteralPath (Join-Path $Repository 'scripts/release/windows_environment.ps1') -Raw
$prefix = $source.Split(@('# Discover the installed compiler;'), [StringSplitOptions]::None)[0]
$root = Join-Path $WorkDirectory 'layout'
$script = Join-Path $root 'scripts/release/windows_environment.ps1'
New-FixtureDirectory (Split-Path -Parent $script)
[IO.File]::WriteAllText($script, $prefix + "`n`$layout | ConvertTo-Json -Compress`n")
$hostFixture = Join-Path $WorkDirectory 'host_layout.ps1'
@'
param($Script, $Machine, $Wow64)
# Load the production function under the real process identity, then classify data.
$null = . $Script
$processMachineBefore = $env:PROCESSOR_ARCHITECTURE
$nativeMachineBefore = $env:PROCESSOR_ARCHITEW6432
try { Get-LockdHostArchitecture $Machine $Wow64 }
finally {
    if ($env:PROCESSOR_ARCHITECTURE -cne $processMachineBefore -or
            $env:PROCESSOR_ARCHITEW6432 -cne $nativeMachineBefore) {
        throw 'Host classification changed the process identity'
    }
}
'@ | Set-Content -LiteralPath $hostFixture -Encoding UTF8
# These children start without the developer-shell fixture's imports. The production
# environment must load its own built-in commands without evaluating this manifest.
$layoutEnvironment = @{
    LOCKD_HOST_BUILD_DIR = $null
    PSModulePath = "$moduleRoot;$env:PSModulePath"
}
foreach ($case in @(@('AMD64', '', 'x86_64'), @('x86', 'AMD64', 'x86_64'),
        @('ARM64', '', 'arm64'), @('AMD64', 'ARM64', 'arm64'),
        @('x86', 'ARM64', 'arm64'), @('x86', '', 'x86'), @('amd64', '', 'x86_64'))) {
    $machine, $wow64, $expected = $case
    $result = Invoke-TestScript $hostFixture @('-Script', $script, '-Machine', $machine,
            '-Wow64', $wow64) -Environment $layoutEnvironment
    Assert-Exit $result 0
    Assert-Equal $result.Output.Trim() $expected "$machine/$wow64 host classification"
}
foreach ($case in @(@('MIPS', ''), @('AMD64', 'MIPS'))) {
    $machine, $wow64 = $case
    $result = Invoke-TestScript $hostFixture @('-Script', $script, '-Machine', $machine,
            '-Wow64', $wow64) -Environment $layoutEnvironment
    Assert-Exit $result 1
    Assert-True $result.Error.Contains('Unsupported build host architecture: MIPS') `
        'Unsupported host must be rejected'
}

# Check the cache layout using real native and WOW64 processes. Both must select
# the native host's directory, independently of the compiler target.
$shells = @((Join-Path $PSHOME 'powershell.exe'))
$wow64Shell = Join-Path $env:WINDIR 'SysWOW64/WindowsPowerShell/v1.0/powershell.exe'
if (Test-Path -LiteralPath $wow64Shell) { $shells += $wow64Shell }
foreach ($shell in ($shells | Select-Object -Unique)) {
    $result = Invoke-TestProcess $shell @('-NoProfile', '-NonInteractive', '-ExecutionPolicy',
            'Bypass', '-File', $script) -Environment $layoutEnvironment
    Assert-Exit $result 0
    $layout = $result.Output | ConvertFrom-Json
    Assert-Equal $layout.dependencies (Join-Path $root "output/obj/$expectedHost/opt/shared/dependencies") `
        "$shell host cache"
}
$workspace = Join-Path $WorkDirectory 'extracted/output/obj/x86_64/opt/shared'
$script = Join-Path $workspace 'source/lockd-test/scripts/release/windows_environment.ps1'
New-FixtureDirectory (Split-Path -Parent $script)
[IO.File]::WriteAllText($script, $prefix + "`n`$layout | ConvertTo-Json -Compress`n")
$result = Invoke-TestScript $script -Environment @{
    LOCKD_HOST_BUILD_DIR = $workspace
    PSModulePath = $layoutEnvironment.PSModulePath
}
Assert-Exit $result 0
Assert-Equal (($result.Output | ConvertFrom-Json).dependencies) (Join-Path $workspace 'dependencies') `
    'Extracted sources must share the outer tool cache'

# Mock interpreter metadata before the production builder reads it. No Python or Boost build runs.
$metadataWrapper = Join-Path $WorkDirectory 'boost_wrapper.ps1'
@'
param($Script)
$ErrorActionPreference = 'Stop'
function python {
    $global:LASTEXITCODE = 0
    '{"version":"3.14","executable":"python","include":"include","prefix":"prefix","tag":"cp314","architecture":"x86_64","boost_name":"boost_python314"}'
}
& $Script
'@ | Set-Content -LiteralPath $metadataWrapper -Encoding UTF8
$root = Join-Path $WorkDirectory 'cache-hit'
New-FixtureDirectory $root
$script = Join-Path $root 'build_boost_python.ps1'
Copy-Item -LiteralPath (Join-Path $Repository 'scripts/release/build_boost_python.ps1') -Destination $script
@'
$ErrorActionPreference = 'Stop'
$repositoryRoot = $PSScriptRoot
$layout = @{ dependencies = Join-Path $repositoryRoot 'output/obj/x86_64/opt/shared/dependencies' }
$global:LASTEXITCODE = 0
function Invoke-WebRequest { throw 'Unexpected download' }
function Invoke-Checked { throw 'Unexpected external command' }
'@ | Set-Content -LiteralPath (Join-Path $root 'windows_environment.ps1') -Encoding UTF8
$prefix = Join-Path $root 'output/obj/x86_64/opt/shared/dependencies/release/boost_python314'
foreach ($name in @('include/boost-1_88/boost/version.hpp', 'lib/boost_python314.lib',
        'lib/boost_python314.dll')) {
    $path = Join-Path $prefix $name
    New-FixtureDirectory (Split-Path -Parent $path)
    [IO.File]::WriteAllText($path, '')
}
foreach ($incomplete in @($false, $true)) {
    if ($incomplete) { Remove-Item -LiteralPath (Join-Path $prefix 'lib/boost_python314.dll') }
    $result = Invoke-TestScript $metadataWrapper @('-Script', $script) `
        -Environment @{ LOCKD_BOOST_CACHE_HIT = 'true' }
    Assert-Equal ($result.ExitCode -eq 0) (!$incomplete) 'Cached Boost.Python completeness'
    if ($incomplete) { Assert-True $result.Error.Contains('cache is incomplete') 'Incomplete cache diagnostic' }
    Assert-Equal (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $prefix) 'boost_1_88_0.tar.gz')) `
        $false 'A cache hit must not prepare the source archive'
    Assert-Equal (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $prefix) 'boost_1_88_0')) `
        $false 'A cache hit must not prepare the source tree'
}

$root = Join-Path $WorkDirectory 'cache-markers'
$dependencies = Join-Path $root 'output/obj/x86_64/opt/shared/dependencies/release'
$source = Join-Path $dependencies 'boost_1_88_0'
New-FixtureDirectory $source
[IO.File]::WriteAllText((Join-Path $source 'b2.exe'), '')
[IO.File]::WriteAllText((Join-Path $dependencies 'boost_1_88_0.tar.gz'), '')
$script = Join-Path $root 'build_boost_python.ps1'
Copy-Item -LiteralPath (Join-Path $Repository 'scripts/release/build_boost_python.ps1') -Destination $script
@'
$ErrorActionPreference = 'Stop'
$repositoryRoot = $PSScriptRoot
$layout = @{ dependencies = Join-Path $repositoryRoot 'output/obj/x86_64/opt/shared/dependencies' }
$global:LASTEXITCODE = 0
Import-Module Microsoft.PowerShell.Utility
function Get-Command($Name) {
    $directory = if ($Name -eq 'link.exe') { $env:LINKER_DIRECTORY } else { 'C:/fixture' }
    @{ Source = "$directory/$Name" }
}
function Get-Item { @{ VersionInfo = @{ FileMajorPart = 19; FileMinorPart = [int] $env:COMPILER_MINOR } } }
function Get-FileHash { @{ Hash = $env:ARCHIVE_HASH } }
function Invoke-Checked {
    param($Command, $Arguments)
    ConvertTo-Json -Compress -InputObject $Arguments | Set-Content -LiteralPath $env:BUILD_TRACE
    if ($env:FAIL_BUILD -eq 'true') { throw 'Build failed' }
}
'@ | Set-Content -LiteralPath (Join-Path $root 'windows_environment.ps1') -Encoding UTF8
foreach ($case in @(@($false, $false, '36', 'C:/fixture'), @($true, $false, '36', 'C:/fixture'),
        @($false, $true, '36', 'C:/fixture'), @($false, $false, '0', 'C:/fixture'),
        @($false, $false, '36', 'C:/other'))) {
    $failBuild, $corrupt, $minor, $linker = $case
    $trace = Join-Path $root 'build.json'
    Remove-Item -LiteralPath $trace -ErrorAction SilentlyContinue
    $invalidCompiler = $minor -eq '0' -or $linker -ne 'C:/fixture'
    $environment = @{
        LOCKD_BOOST_CACHE_HIT = ''; FAIL_BUILD = $failBuild.ToString().ToLowerInvariant()
        COMPILER_MINOR = $minor; LINKER_DIRECTORY = $linker; BUILD_TRACE = $trace
        ARCHIVE_HASH = $(if ($corrupt) { 'corrupt' } else {
            '3621533e820dcab1e8012afd583c0c73cf0f77694952b81352bf38c1488f9cb4'
        })
    }
    $result = Invoke-TestScript $metadataWrapper @('-Script', $script) -Environment $environment
    $success = !($failBuild -or $corrupt -or $invalidCompiler)
    Assert-Equal ($result.ExitCode -eq 0) $success ($result.Output + $result.Error)
    Assert-Equal (Test-Path -LiteralPath (Join-Path $dependencies '.boost-source-verified')) (!$corrupt) `
        'Verified source retention'
    Assert-Equal (Test-Path -LiteralPath (Join-Path $dependencies '.boost-cache-ready')) $success `
        'Completed cache marker'
    Assert-Equal (Test-Path -LiteralPath $trace) (!($corrupt -or $invalidCompiler)) 'Build boundary'
    if (Test-Path -LiteralPath $trace) {
        $arguments = Get-Content -LiteralPath $trace -Raw | ConvertFrom-Json
        foreach ($argument in @('toolset=msvc-14.3', 'architecture=x86', 'address-model=64',
                '--project-config=', '--ignore-site-config')) {
            Assert-True ($arguments -contains $argument) "Boost build argument $argument"
        }
        $configuration = Get-Content -LiteralPath (Join-Path $dependencies 'boost_python314.jam') -Raw
        Assert-True $configuration.Contains('using msvc : 14.3 : "C:/fixture/cl.exe" ;') `
            'Explicit selected compiler'
    }
}

$root = Join-Path $WorkDirectory 'analysis'
New-FixtureDirectory $root
$script = Join-Path $root 'analyze_windows.ps1'
Copy-Item -LiteralPath (Join-Path $Repository 'scripts/release/analyze_windows.ps1') -Destination $script
@'
$ErrorActionPreference = 'Stop'
$repositoryRoot = $PSScriptRoot
$layout = @{ dependencies = Join-Path $repositoryRoot 'output/obj/x86_64/opt/shared/dependencies' }
function python {}
$script:commandCount = 0
function Invoke-Checked {
    param([string] $Command, [string[]] $Arguments)
    $script:commandCount++
    ConvertTo-Json -Compress -InputObject (@($Command) + $Arguments) |
        Add-Content -LiteralPath $env:TRACE
    if ($script:commandCount -eq [int] $env:FAIL_COMMAND) { throw 'Recorded command failure' }
}
'@ | Set-Content -LiteralPath (Join-Path $root 'windows_environment.ps1') -Encoding UTF8
foreach ($failure in 0..4) {
    $trace = Join-Path $root "trace-$failure.jsonl"
    $result = Invoke-TestScript $script -Environment @{
        TRACE = $trace; FAIL_COMMAND = $failure; LOCKD_CI_CLANG_TIDY = 'C:/analysis tools/clang-tidy.exe'
    }
    $commands = @(Get-Content -LiteralPath $trace | ForEach-Object { ,($_ | ConvertFrom-Json) })
    $expectedCount = if ($failure) { $failure } else { 4 }
    Assert-Equal $commands.Count $expectedCount "Analysis command count at failure $failure"
    Assert-Equal ($result.ExitCode -eq 0) ($failure -eq 0) ($result.Output + $result.Error)
    $pythonBuild = 'lib/pylockd/output/obj/cp314-cp314-win_amd64/opt/static/analysis'
    foreach ($option in @('-DLOCKD_DEPENDENCY_PROVIDER=SYSTEM', '-DLOCKD_BUILD_PYTHON=ON',
            '-DSKBUILD=ON', '-DLOCKD_CLANG_TIDY_EXECUTABLE=C:/analysis tools/clang-tidy.exe',
            '-DLOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON')) {
        Assert-True ($commands[0] -contains $option) "Analysis configure option $option"
    }
    Assert-Equal $commands[0][([array]::IndexOf($commands[0], '-B') + 1)] $pythonBuild `
        'Python analysis build tree'
    if (!$failure) {
        Assert-Sequence $commands[1][-2..-1] @('--target', 'clang_tidy') 'Native scan target'
        Assert-Sequence $commands[2][-2..-1] @('--target', 'clang_tidy') 'Python scan target'
        Assert-True ($commands[1][2] -ne $commands[2][2]) 'Analysis trees must remain separate'
        Assert-Equal $commands[2][2] $pythonBuild 'Python scan build tree'
        Assert-Equal $commands[3][0] 'ctest' 'Analysis regressions'
    }
}
Write-Output 'Windows environment, cache and analysis orchestration checks passed.'
