# Prepare isolated unsigned archives and verify rejection before installer rebuilding.
param([string] $Repository, [string] $WorkDirectory)
. "$PSScriptRoot/test_helpers.ps1"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$tar = (Get-Command tar.exe -ErrorAction Stop).Source

foreach ($case in @('unsigned', 'unexpected-entry', 'mismatched-source')) {
    $root = Join-Path $WorkDirectory $case
    $scripts = Join-Path $root 'scripts/release'
    $assets = Join-Path $root 'output/x86_64/opt/static'
    New-FixtureDirectory $scripts
    New-FixtureDirectory $assets
    foreach ($name in @('prepare_windows_signing.ps1', 'verify_windows_signature.ps1')) {
        Copy-Item -LiteralPath (Join-Path $Repository "scripts/release/$name") -Destination $scripts
    }
    $source = Join-Path $root 'source/lockd-1.0.4/cmake'
    New-FixtureDirectory $source
    $version = if ($case -eq 'mismatched-source') { '1.0.5' } else { '1.0.4' }
    [IO.File]::WriteAllText((Join-Path $source 'version.txt'), $version)
    Assert-Exit (Invoke-TestProcess $tar @('-czf', (Join-Path $assets 'lockd-1.0.4.tar.gz'),
        '-C', (Join-Path $root 'source'), 'lockd-1.0.4')) 0
    foreach ($arch in @('x86', 'x86_64', 'arm64')) {
        $zip = Join-Path $assets "lockd-1.0.4-windows-$arch-portable.zip"
        $archive = [IO.Compression.ZipFile]::Open($zip, 'Create')
        try {
            $names = @('lockd.exe', 'lockd_test.exe', 'license.txt', 'boost_license.txt')
            if ($case -eq 'unexpected-entry' -and $arch -eq 'x86') {
                $names += '../unexpected.exe'
            }
            foreach ($name in $names) {
                $entry = $archive.CreateEntry($name)
                $writer = [IO.StreamWriter]::new($entry.Open())
                try { $writer.Write('unsigned fixture') }
                finally { $writer.Dispose() }
            }
        }
        finally { $archive.Dispose() }
    }
    $prepare = Join-Path $scripts 'prepare_windows_signing.ps1'
    $result = Invoke-TestScript $prepare @('-Operation', 'Prepare', '-Version', '1.0.4')
    if ($case -eq 'unsigned') {
        Assert-Exit $result 0
        $payload = Join-Path $root 'output/obj/x86_64/opt/shared/signing/payload'
        Assert-Equal @(Get-ChildItem -LiteralPath $payload -Recurse -Filter '*.exe').Count 6 `
            'All architectures must supply both binaries'
        $result = Invoke-TestScript $prepare @('-Operation', 'Rebuild', '-Version', '1.0.4')
        Assert-True ($result.ExitCode -ne 0) 'Unsigned executables must prevent rebuilding'
        Assert-True $result.Error.Contains('Signature verification failed') 'Signature failure diagnostic'
        Assert-Equal @(Get-ChildItem -LiteralPath $root -Recurse -Filter '*-setup.exe').Count 0 `
            'Failed signature verification must not produce an installer'
    }
    elseif ($case -eq 'unexpected-entry') {
        Assert-True ($result.ExitCode -ne 0) 'Unexpected archive entries must be rejected'
        Assert-True $result.Error.Contains('Unexpected portable payload') 'Payload rejection diagnostic'
        Assert-Equal @(Get-ChildItem -LiteralPath $root -Recurse -Filter 'unexpected.exe').Count 0 `
            'The rejected entry must not be extracted'
    }
    else {
        Assert-True ($result.ExitCode -ne 0) 'Source metadata must match the binary release'
        Assert-True $result.Error.Contains('source version does not match') 'Source mismatch diagnostic'
    }
}
Write-Output 'Windows signing preparation and rejection checks passed.'
