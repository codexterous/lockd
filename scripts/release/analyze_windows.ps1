# Keep native vcpkg and the selected interpreter's Boost.Python in separate trees.
# Analysis needs their compiler commands, but doesn't need to build a wheel or daemon.
. "$PSScriptRoot/windows_environment.ps1"
Push-Location $repositoryRoot
try {
    $python = (Get-Command python).Source
    $prefix = Join-Path $layout.dependencies 'release/boost_python314'
    $pythonBuild = 'lib/pylockd/output/obj/cp314-cp314-win_amd64/opt/static/analysis'
    $nativeBuild = 'output/obj/x86_64/opt/shared'
    Invoke-Checked cmake @('-S', '.', '-B', $pythonBuild, '-G', 'Ninja',
        '-DSKBUILD=ON', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DLOCKD_DEPENDENCY_PROVIDER=SYSTEM', "-DBoost_ROOT=$prefix",
        "-DPython3_EXECUTABLE=$python", '-DBUILD_TESTING=OFF', '-DBUILD_SHARED_LIBS=OFF',
        '-DLOCKD_STATIC_RUNTIME=OFF', '-DLOCKD_BUILD_PYTHON=ON',
        "-DLOCKD_CLANG_TIDY_EXECUTABLE=$env:LOCKD_CI_CLANG_TIDY", '-DLOCKD_CLANG_TIDY=OFF', '-DLOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON')
    Invoke-Checked cmake @('--build', $nativeBuild, '--target', 'clang_tidy')
    Invoke-Checked cmake @('--build', $pythonBuild, '--target', 'clang_tidy')
    Invoke-Checked ctest @('--test-dir', $nativeBuild, '-R', '^lockd_clang_tidy_',
        '--output-on-failure', '--no-tests=error')
}
finally {
    Pop-Location
}
