#!/bin/sh
# Use the distro Python and Boost.Python together; no wheel needs to be built here.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
build=output/obj/x86_64/opt/shared/analysis
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DLOCKD_TARGET_ARCH=x86_64 -DLOCKD_DEPENDENCY_PROVIDER=SYSTEM \
    -DBUILD_SHARED_LIBS=ON -DLOCKD_STATIC_RUNTIME=OFF -DBUILD_TESTING=ON \
    -DLOCKD_BUILD_PYTHON=ON -DPython3_EXECUTABLE=/usr/bin/python3 \
    -DLOCKD_CLANG_TIDY=OFF -DLOCKD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON \
    -DLOCKD_CLANG_TIDY_EXECUTABLE=/usr/bin/clang-tidy-23
cmake --build "$build" --target clang_tidy_full
ctest --test-dir "$build" -R '^lockd_clang_tidy_' --output-on-failure --no-tests=error
