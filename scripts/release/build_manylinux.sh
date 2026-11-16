#!/bin/bash
# Run inside the official manylinux image, on a runner matching its architecture.
set -euo pipefail
tag="cp${PYTHON_VERSION//./}"
export PATH="/opt/python/$tag-$tag/bin:$PATH"
export CMAKE_GENERATOR=Ninja
export CMAKE_BUILD_PARALLEL_LEVEL=2
case "$WHEEL_ARCH" in
    aarch64) arch=arm64 ;;
    i686) arch=x86 ;;
    x86_64) arch=x86_64 ;;
    *) echo "Unsupported wheel architecture: $WHEEL_ARCH" >&2; exit 1 ;;
esac
export PIP_CACHE_DIR="/src/output/obj/$arch/opt/shared/dependencies/python/pip-cache"

python scripts/release/build_linux_wheel.py --manylinux "$WHEEL_ARCH"

# One matrix job supplies the platform-independent source distribution for publication.
if [ "$WHEEL_ARCH" = x86_64 ] && [ "$PYTHON_VERSION" = 3.14 ]; then
    "output/obj/$arch/opt/shared/python-release/$tag/build_env/bin/python" -m build \
        --sdist --no-isolation --outdir "lib/pylockd/output/$arch/opt/static"
fi
