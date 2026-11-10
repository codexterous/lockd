# Windows-only packaging of already-tested binaries, using their matching release source.
param(
    [Parameter(Mandatory)][ValidateSet('Prepare', 'Rebuild', 'Collect')][string] $Operation,
    [Parameter(Mandatory)][string] $Version
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$assets = Join-Path $root 'output/x86_64/opt/static'
$work = Join-Path $root 'output/obj/x86_64/opt/shared/signing'
$source = Join-Path $work "source/lockd-$Version"
$architectures = @('x86', 'x86_64', 'arm64')
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'A release version is required.' }

if ($Operation -eq 'Prepare') {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    New-Item -ItemType Directory -Path "$work/source", "$work/installers" -Force | Out-Null
    tar -xzf "$assets/lockd-$Version.tar.gz" -C "$work/source"
    if ($LASTEXITCODE -ne 0) { throw 'Release source extraction failed.' }
    if ((Get-Content "$source/cmake/version.txt" -Raw).Trim() -ne $Version) {
        throw 'Published source version does not match the Windows packages.'
    }
    foreach ($arch in $architectures) {
        $zip = Join-Path $assets "lockd-$Version-windows-$arch-portable.zip"
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            $names = @($archive.Entries | ForEach-Object { $_.FullName } | Sort-Object)
            $expected = @('boost_license.txt', 'license.txt', 'lockd.exe', 'lockd_test.exe')
            if (@(Compare-Object $expected $names).Count -ne 0 -or $names.Count -ne 4) {
                throw "Unexpected portable payload: $zip"
            }
        }
        finally { $archive.Dispose() }
        Expand-Archive -LiteralPath $zip -DestinationPath "$work/payload/$arch"
    }
}
elseif ($Operation -eq 'Rebuild') {
    & "$PSScriptRoot/verify_windows_signature.ps1" -Directory "$work/payload"
    foreach ($arch in $architectures) {
        $name = "lockd-$Version-windows-$arch"
        $destination = Join-Path $source "output/$arch/opt/static"
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        Compress-Archive -Path "$work/payload/$arch/*" -DestinationPath "$destination/$name-portable.zip"
        # Use the published source's installer recipe, not the workflow commit's recipe.
        & "$source/scripts/release/build_windows_installer.ps1" -Architecture $arch
        if ($LASTEXITCODE -ne 0) { throw "Installer build failed: $arch" }
        Copy-Item -LiteralPath "$destination/$name-portable.zip" -Destination $assets -Force
        Copy-Item -LiteralPath "$destination/$name-setup.exe" -Destination "$work/installers"
    }
}
else {
    & "$PSScriptRoot/verify_windows_signature.ps1" -Directory "$work/installers"
    foreach ($arch in $architectures) {
        Copy-Item -LiteralPath "$work/installers/lockd-$Version-windows-$arch-setup.exe" `
            -Destination $assets -Force
    }
}
