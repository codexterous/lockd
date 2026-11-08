# Dot-source this file from a PowerShell session before running the release builds.
param(
    [ValidateSet('x86', 'x86_64', 'arm64')][string] $Architecture = 'x86_64',
    [string] $ToolsetVersion = $env:LOCKD_MSVC_TOOLSET_VERSION
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path

# GitHub's Windows runner already provides vcpkg. Honor an explicit selection
# first; don't clone a project-private copy of the package manager.
if (!$env:VCPKG_ROOT -and $env:VCPKG_INSTALLATION_ROOT) {
    $env:VCPKG_ROOT = $env:VCPKG_INSTALLATION_ROOT
}

# Resolve PATH before VsDevCmd changes PATH and replaces VCPKG_ROOT.
# Keep that user selection even when the compiler comes from another installation.
if (!$env:VCPKG_ROOT) {
    $selectedVcpkg = Get-Command vcpkg.exe -ErrorAction SilentlyContinue
    if ($selectedVcpkg) { $env:VCPKG_ROOT = Split-Path -Parent $selectedVcpkg.Source }
}
$selectedVcpkgRoot = $env:VCPKG_ROOT

# Use the host architecture even from an x86 developer shell. Build tools and
# downloads are shared; selecting another target must not select an empty cache.
$hostMachine = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
$hostArchitecture = switch ($hostMachine.ToLowerInvariant()) {
    'amd64' { 'x86_64' }
    'arm64' { 'arm64' }
    'x86' { 'x86' }
    default { throw "Unsupported build host architecture: $hostMachine" }
}
$hostBuild = Join-Path $repositoryRoot "output/obj/$hostArchitecture/opt/shared"
if ($env:LOCKD_HOST_BUILD_DIR) {
    $hostBuild = [System.IO.Path]::GetFullPath($env:LOCKD_HOST_BUILD_DIR)
}
$layout = @{
    build = $hostBuild
    dependencies = Join-Path $hostBuild 'dependencies'
    staging = Join-Path $hostBuild 'release'
}
$temporaryDirectory = Join-Path $layout.staging 'tmp'
New-Item -ItemType Directory -Force $temporaryDirectory | Out-Null
$env:TEMP = $temporaryDirectory
$env:TMP = $temporaryDirectory
$env:PIP_CACHE_DIR = Join-Path $layout.dependencies 'python/pip-cache'
$env:PYTHONDONTWRITEBYTECODE = '1'

# Discover the installed compiler; no developer-specific Visual Studio path is recorded.
$compilerArchitecture = if ($Architecture -eq 'x86_64') { 'x64' } else { $Architecture }
$matchingToolset = !$ToolsetVersion -or ([string] $env:VCToolsVersion).StartsWith("$ToolsetVersion.")
if ($env:VSCMD_ARG_TGT_ARCH -ne $compilerArchitecture -or !$matchingToolset) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $component = if ($Architecture -eq 'arm64') { 'ARM64' } else { 'x86.x64' }
    $installation = & $vswhere -latest -products '*' -requires "Microsoft.VisualStudio.Component.VC.Tools.$component" -property installationPath
    if ($LASTEXITCODE -ne 0 -or !$installation) {
        throw "Visual Studio with the $compilerArchitecture C++ tools is required."
    }
    Import-Module (Join-Path $installation 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    $compilerHost = if ($hostArchitecture -eq 'arm64') { 'arm64' } else { 'x64' }
    $developerArguments = "-arch=$compilerArchitecture -host_arch=$compilerHost"
    if ($ToolsetVersion) {
        $developerArguments += " -vcvars_ver=$ToolsetVersion"
    }
    Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation `
        -DevCmdArguments $developerArguments
}

# VsDevCmd replaces VCPKG_ROOT with Visual Studio's bundled read-only installation.
# Restore the caller/runner choice so CMake and CI cache transfers use the same root.
if ($selectedVcpkgRoot) {
    $env:VCPKG_ROOT = $selectedVcpkgRoot
}

# Windows PowerShell does not turn a native nonzero exit into a terminating error.
function Invoke-Checked {
    param([Parameter(Mandatory)][string] $Command, [string[]] $Arguments)
    Get-Command $Command -ErrorAction Stop | Out-Null
    # Windows PowerShell represents redirected native stderr as errors, even on success.
    # Preserve the diagnostic text and use the process exit status to decide failure.
    $ErrorActionPreference = 'Continue'
    & $Command @Arguments 2>&1 | ForEach-Object { Write-Output "$_" }
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}
