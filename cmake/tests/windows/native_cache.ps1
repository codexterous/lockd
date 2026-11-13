# Preserve restored and newly completed native packages through the production cache helper.
param([string] $Repository, [string] $WorkDirectory)
. "$PSScriptRoot/test_helpers.ps1"
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$workflowRoot = $Repository
if (!(Test-Path -LiteralPath (Join-Path $workflowRoot '.github/workflows/release_windows.yml')) -and
        $env:GITHUB_WORKSPACE) {
    $workflowRoot = $env:GITHUB_WORKSPACE
}
$workflow = Join-Path $workflowRoot '.github/workflows/release_windows.yml'
$collector = $null
if (Test-Path -LiteralPath $workflow) {
    $text = (Get-Content -LiteralPath $workflow -Raw).Replace("`r`n", "`n")
    $step = @([regex]::Split($text, '(?m)^\s*- name: ') |
        Where-Object { $_.Contains("id: collect_native`n") })
    Assert-Equal $step.Count 1 'Native collector workflow step'
    $body = $step[0].Split(@("        run: |`n"), [StringSplitOptions]::None)[1]
    $lines = $body.Split("`n")
    $first = @($lines | Where-Object { $_.Trim() })[0]
    $indent = [regex]::Match($first, '^\s*').Length
    $collector = ($lines | ForEach-Object {
        if ($_.Length -ge $indent) { $_.Substring($indent) } else { $_ }
    }) -join "`n"
    $collector = $collector.Replace('${{ env.OUTPUT_ARCH }}', 'x86_64')
}

$workspace = Join-Path $WorkDirectory 'workspace'
$checkout = Join-Path $workspace 'source/lockd-test'
$helper = Join-Path $checkout 'scripts/release/windows_vcpkg_cache.ps1'
New-FixtureDirectory (Split-Path -Parent $helper)
Copy-Item -LiteralPath (Join-Path $Repository 'scripts/release/windows_vcpkg_cache.ps1') -Destination $helper
$cache = Join-Path $workspace 'output/obj/x86_64/opt/shared/cache/native'
$dependencies = Join-Path $workspace 'output/obj/x86_64/opt/shared/dependencies'
$restored = Join-Path $cache 'binary-cache/aa/old.zip'
New-FixtureDirectory (Split-Path -Parent $restored)
[IO.File]::WriteAllText($restored, 'restored package')
foreach ($pair in @(@('binary-cache/aa/old.zip', 'restored package'),
        @('binary-cache/bb/new.zip', 'new ABI package'),
        @('vcpkg-downloads/source.tar.gz', 'completed download'))) {
    $file = Join-Path $dependencies $pair[0]
    New-FixtureDirectory (Split-Path -Parent $file)
    [IO.File]::WriteAllText($file, $pair[1])
}
$environment = @{
    GITHUB_WORKSPACE = $workspace; BUILD_ROOT = $checkout
    LOCKD_HOST_BUILD_DIR = Split-Path -Parent $dependencies
    VCPKG_ROOT = Join-Path $workspace 'vcpkg'
    VCPKG_DEFAULT_BINARY_CACHE = Join-Path $dependencies 'binary-cache'
    VCPKG_DOWNLOADS = Join-Path $dependencies 'vcpkg-downloads'
}
if ($null -ne $collector) {
    $driver = Join-Path $workspace 'collect.ps1'
    [IO.File]::WriteAllText($driver, '$ErrorActionPreference = "Stop"' + "`n" + $collector)
}
foreach ($attempt in 1..2) {
    if ($null -ne $collector) {
        $result = Invoke-TestScript $driver -Environment $environment
    }
    else {
        # Without workflow metadata, still exercise the shipped helper.
        $result = Invoke-TestScript $helper @('-Operation', 'Collect', '-CacheDirectory', $cache) `
            -Environment $environment
    }
    Assert-Exit $result 0
    $files = @(Get-ChildItem -LiteralPath $cache -Recurse -File | ForEach-Object {
        $_.FullName.Substring($cache.Length + 1).Replace('\', '/')
    } | Sort-Object)
    Assert-Sequence $files @('binary-cache/aa/old.zip', 'binary-cache/bb/new.zip',
        'vcpkg-downloads/source.tar.gz') "Collected package set on attempt $attempt"
    Assert-FileBytes $restored ([Text.Encoding]::UTF8.GetBytes('restored package')) 'Restored package data'
    Assert-FileBytes (Join-Path $cache 'binary-cache/bb/new.zip') `
        ([Text.Encoding]::UTF8.GetBytes('new ABI package')) 'New ABI package data'
}
$environment.VCPKG_DEFAULT_BINARY_CACHE = $null
$environment.VCPKG_DOWNLOADS = $null
$environment.LOCALAPPDATA = Join-Path $workspace 'user-local'
Assert-Exit (Invoke-TestScript $helper @('-Operation', 'Restore', '-CacheDirectory', $cache) `
    -Environment $environment) 0
Assert-FileBytes (Join-Path $workspace 'user-local/vcpkg/archives/bb/new.zip') `
    ([Text.Encoding]::UTF8.GetBytes('new ABI package')) 'Default archive restoration'
Assert-FileBytes (Join-Path $workspace 'vcpkg/downloads/source.tar.gz') `
    ([Text.Encoding]::UTF8.GetBytes('completed download')) 'Default download restoration'
Write-Output 'Native cache collection, repeat collection and default restoration passed.'
