#!/bin/sh
# Run the same native compatibility checks from a checkout or extracted release.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"

cmake --version
"${CXX:-c++}" --version

build_jobs=2
cmake -DLOCKD_BUILD_JOBS="$build_jobs" -P cmake/lockd_report_build_cpus.cmake

case "$(uname -m)" in
    x86_64|amd64) arch=x86_64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) echo 'Native CI requires x86_64 or arm64.' >&2; exit 1 ;;
esac

# Shared Release builds are standard on Linux and FreeBSD. Exercise an unchanged
# second build too; dedicated analysis jobs run the clang-tidy integration fixtures.
preset="$arch-opt-shared"
build="output/obj/$arch/opt/shared"
cmake --preset "$preset" -DLOCKD_STATIC_RUNTIME=OFF \
    -DLOCKD_BUILD_PYTHON=OFF -DLOCKD_CLANG_TIDY=OFF \
    -DLOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON
if [ "${1:-}" = --daemon-only ]; then
    cmake --build "$build" --target lockd --parallel "$build_jobs"
    exit 0
fi
cmake --build "$build" --parallel "$build_jobs"
cmake --build "$build" --parallel "$build_jobs"
ctest --test-dir "$build" --output-on-failure --no-tests=error --parallel 2 -E '^lockd_clang_tidy_'
