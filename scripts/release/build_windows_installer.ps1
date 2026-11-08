param(
    [ValidateSet('x86', 'x86_64', 'arm64')][string] $Architecture = 'x86_64',
    [string] $Python = 'python',
    [switch] $LaunchOnly
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$metadata = & $Python "$PSScriptRoot/package.py" metadata
if ($LASTEXITCODE -ne 0) { throw 'Could not read the release version.' }
$version = ($metadata | Where-Object { $_ -like 'version=*' }).Substring(8)
$numericVersion = $version -replace '\.dev0$', ''
$destination = Join-Path $root "output/$Architecture/opt/static/lockd-$version-windows-$Architecture-setup.exe"
if ($LaunchOnly) {
    $process = Start-Process -FilePath $destination -Verb RunAs -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "Installer exited with code $($process.ExitCode)." }
    return
}
$layoutJson = & $Python "$PSScriptRoot/output_layout.py"
if ($LASTEXITCODE -ne 0) { throw 'Could not select build output directories.' }
$layout = $layoutJson | ConvertFrom-Json
$tools = Join-Path $layout.dependencies 'nsis'
$archive = Join-Path $tools 'nsis-3.11.zip'
New-Item -ItemType Directory -Force $tools | Out-Null
if (!(Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest 'https://github.com/tauri-apps/binary-releases/releases/download/nsis-3.11/nsis-3.11.zip' -OutFile $archive
}

# Verify the published archive digest before executing its compiler or plug-ins.
$expected = 'c7d27f780ddb6cffb4730138cd1591e841f4b7edb155856901cdf5f214394fa1'
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
    throw 'NSIS archive checksum mismatch.'
}
Expand-Archive -LiteralPath $archive -DestinationPath $tools -Force
$compiler = Join-Path $tools 'nsis-3.11/makensis.exe'
$payload = Join-Path $root "output/obj/$Architecture/opt/static/installer/payload"
New-Item -ItemType Directory -Force $payload | Out-Null
$portable = Join-Path $root "output/$Architecture/opt/static/lockd-$version-windows-$Architecture-portable.zip"
Expand-Archive -LiteralPath $portable -DestinationPath $payload -Force
& $compiler /NOCONFIG /V3 /WX "/DARCH=$Architecture" "/DVERSION=$version" "/DNUMERIC_VERSION=$numericVersion" `
    "/DPAYLOAD=$payload" "/DDESTINATION=$destination" `
    (Join-Path $root 'platform/win32/installer/lockd.nsi')
if ($LASTEXITCODE -ne 0) { throw 'NSIS compilation failed.' }
