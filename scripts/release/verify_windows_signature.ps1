# Windows-only Authenticode verification for the release signing gate.
param([Parameter(Mandatory)][string] $Directory)

$ErrorActionPreference = 'Stop'
$files = @(Get-ChildItem -LiteralPath $Directory -Filter '*.exe' -File -Recurse)
if ($files.Count -eq 0) { throw "No executables to verify in $Directory." }
$tools = @(Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin/*/x64/signtool.exe" |
    Sort-Object FullName -Descending)
if ($tools.Count -eq 0) { throw 'Windows SDK SignTool was not found.' }
foreach ($file in $files) {
    # /pa uses Authenticode policy; /tw makes a missing timestamp a warning.
    # Reject every nonzero exit code, including warnings, before publishing.
    & $tools[0].FullName verify /pa /all /tw /v $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "Signature verification failed: $($file.FullName)" }
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.TimeStamperCertificate) {
        throw "A valid timestamped signature is required: $($file.FullName)"
    }
}
