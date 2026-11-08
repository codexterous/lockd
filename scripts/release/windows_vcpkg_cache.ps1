# Transfer Windows CI cache snapshots without changing vcpkg's storage locations.
param(
    [Parameter(Mandatory)][ValidateSet('Restore', 'Collect')][string] $Operation,
    [Parameter(Mandatory)][string] $CacheDirectory
)

$ErrorActionPreference = 'Stop'
$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { $env:VCPKG_INSTALLATION_ROOT }
if (!$vcpkgRoot) { throw 'Set VCPKG_ROOT to the installed vcpkg checkout.' }
$binaryCache = if ($env:VCPKG_DEFAULT_BINARY_CACHE) {
    $env:VCPKG_DEFAULT_BINARY_CACHE
} else {
    Join-Path $env:LOCALAPPDATA 'vcpkg/archives'
}
$downloads = if ($env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS } else { Join-Path $vcpkgRoot 'downloads' }

# Retain the existing snapshot layout and keys, so previous CI runs' compiled
# archives remain usable. Only the live package-manager locations have changed.
$locations = @{ 'binary-cache' = $binaryCache; 'vcpkg-downloads' = $downloads }
foreach ($name in $locations.Keys) {
    $snapshot = Join-Path $CacheDirectory $name
    $source = if ($Operation -eq 'Restore') { $snapshot } else { $locations[$name] }
    $destination = if ($Operation -eq 'Restore') { $locations[$name] } else { $snapshot }
    if (Test-Path -LiteralPath $source) {
        New-Item -ItemType Directory -Force $destination | Out-Null
        Get-ChildItem -LiteralPath $source -Force | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $destination -Recurse -Force
        }
    }
}
