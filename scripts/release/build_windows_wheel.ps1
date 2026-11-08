param([string] $Python = 'python')

$details = & $Python "$PSScriptRoot/package.py" interpreter
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the selected Python interpreter.' }
$interpreter = $details | ConvertFrom-Json
. "$PSScriptRoot/windows_environment.ps1" -Architecture $interpreter.architecture
Push-Location $repositoryRoot
try {
    & "$PSScriptRoot/build_boost_python.ps1" -Python $Python

    $environment = Join-Path $layout.dependencies "python/release/$($interpreter.tag)"
    Invoke-Checked $Python @('-m', 'venv', $environment)
    $buildPython = Join-Path $environment 'Scripts/python.exe'
    Invoke-Checked $buildPython @('-m', 'pip', 'install', 'build==1.6.1', 'scikit-build-core==1.1.1')
    $boostPrefix = Join-Path $layout.dependencies "release/$($interpreter.boost_name)"
    $env:CMAKE_GENERATOR = 'Ninja'

    # Use one explicitly selected interpreter/Boost.Python pair. The vcpkg Python port has
    # its own ABI; it must not silently supply this wheel's Python dependency instead.
    Invoke-Checked $buildPython @(
        '-m', 'build', '--wheel', '--no-isolation', '--outdir', $interpreter.wheel_directory,
        '-Ccmake.define.LOCKD_DEPENDENCY_PROVIDER=SYSTEM',
        "-Ccmake.define.Boost_ROOT=$boostPrefix", '-Ccmake.define.LOCKD_CLANG_TIDY=OFF',
        '-Ccmake.define.LOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON',
        '-Ccmake.define.CMAKE_BUILD_TYPE=Release', "-Cbuild-dir=$($interpreter.build_directory)"
    )

}
finally {
    Pop-Location
}
