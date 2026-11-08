# Build only Boost.Python for the interpreter selected by the caller. Native release
# executables continue to use the normal vcpkg manifest and static preset.
param(
    [string] $Python = 'python',
    [ValidateSet('release', 'debug', 'debug,release')][string] $Variant = 'release'
)

$details = & $Python "$PSScriptRoot/package.py" interpreter
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the selected Python interpreter.' }
$interpreter = $details | ConvertFrom-Json
. "$PSScriptRoot/windows_environment.ps1" -Architecture $interpreter.architecture
$dependencyDirectory = Join-Path $layout.dependencies 'release'
New-Item -ItemType Directory -Force $dependencyDirectory | Out-Null
$archive = Join-Path $dependencyDirectory 'boost_1_88_0.tar.gz'
$source = Join-Path $dependencyDirectory 'boost_1_88_0'
$prefix = Join-Path $dependencyDirectory $interpreter.boost_name

if ($env:LOCKD_BOOST_CACHE_HIT -eq 'true') {
    $header = Join-Path $prefix 'include/boost-1_88/boost/version.hpp'
    $libraries = @(Get-ChildItem -LiteralPath (Join-Path $prefix 'lib') -Filter '*boost_python*.lib')
    $dlls = @(Get-ChildItem -LiteralPath (Join-Path $prefix 'lib') -Filter '*boost_python*.dll')
    if (!(Test-Path -LiteralPath $header) -or !$libraries.Count -or !$dlls.Count) {
        throw 'The restored Boost.Python cache is incomplete.'
    }
    Write-Output 'Using cached Boost.Python wheel dependencies'
    return
}

# Keep verified downloads after build failures, but never cache a partial installation.
$verified = Join-Path $dependencyDirectory '.boost-source-verified'
$ready = Join-Path $dependencyDirectory '.boost-cache-ready'
foreach ($marker in @($verified, $ready)) {
    if (Test-Path -LiteralPath $marker) {
        Remove-Item -LiteralPath $marker -Force
    }
}

# This is the upstream archive's published SHA256, not a checksum fetched alongside it.
$expectedHash = '3621533e820dcab1e8012afd583c0c73cf0f77694952b81352bf38c1488f9cb4'
if (!(Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest 'https://archives.boost.io/release/1.88.0/source/boost_1_88_0.tar.gz' `
        -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
    throw "Boost archive checksum mismatch: $archive"
}
Set-Content -LiteralPath $verified -Value 'verified'
if (!(Test-Path -LiteralPath $source)) {
    Invoke-Checked tar @('-xzf', $archive, '-C', $dependencyDirectory)
}

$pythonExecutable = $interpreter.executable.Replace('\', '/')
$pythonInclude = $interpreter.include.Replace('\', '/')
$pythonLibraries = (Join-Path $interpreter.prefix 'libs').Replace('\', '/')
$configuration = Join-Path $dependencyDirectory "$($interpreter.boost_name).jam"
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source.Replace('\', '/')
$linker = (Get-Command link.exe -ErrorAction Stop).Source.Replace('\', '/')
if ((Split-Path -Parent $compiler) -ne (Split-Path -Parent $linker)) {
    throw 'The active MSVC compiler and linker must come from the same toolset.'
}
$compilerVersion = (Get-Item -LiteralPath $compiler).VersionInfo
if ($compilerVersion.FileMajorPart -ne 19 -or $compilerVersion.FileMinorPart -lt 20 -or
        $compilerVersion.FileMinorPart -ge 50) {
    throw "Unsupported MSVC compiler version: $($compilerVersion.FileVersion)"
}
$msvcVersion = if ($compilerVersion.FileMinorPart -ge 30) { '14.3' } else { '14.2' }

# Do not let Boost rediscover a registered compiler or load another project's
# toolset configuration. A missing compiler command can make Windows open source
# files in their associated editor instead of compiling them.
$pythonConfiguration = "using msvc : $msvcVersion : `"$compiler`" ;`n" +
    "using python : $($interpreter.version) : `"$pythonExecutable`" : `"$pythonInclude`" : `"$pythonLibraries`" ;`n"
[System.IO.File]::WriteAllText($configuration, $pythonConfiguration, [System.Text.UTF8Encoding]::new($false))

Push-Location $source
try {
    if (!(Test-Path -LiteralPath 'b2.exe')) {
        # windows_environment.ps1 already selected MSVC. Guessing again can hang
        # on portable compiler installations that aren't registered with vswhere.
        Invoke-Checked cmd @('/c', 'bootstrap.bat', 'msvc')
    }

    # Boost's toolset, architecture, Python ABI and configuration directories can
    # reach Windows' legacy MAX_PATH limit (260 characters including the terminator).
    # Hash only those internal property directories; keep our build/install roots
    # and the installed SDK names unchanged for wheel discovery and caching.
    Invoke-Checked './b2.exe' @(
        '--with-python', '--hash', '--ignore-site-config', '--project-config=',
        "--user-config=$configuration", "toolset=msvc-$msvcVersion",
        "python=$($interpreter.version)",
        $(if ($interpreter.architecture -eq 'arm64') { 'architecture=arm' } else { 'architecture=x86' }),
        $(if ($interpreter.architecture -eq 'x86') { 'address-model=32' } else { 'address-model=64' }),
        "variant=$Variant", 'link=shared', 'runtime-link=shared',
        'threading=multi', '-j2', "--build-dir=$dependencyDirectory/boost_python_build/$($interpreter.boost_name)",
        "--prefix=$prefix", 'install'
    )
}
finally {
    Pop-Location
}
Set-Content -LiteralPath $ready -Value 'ready'
