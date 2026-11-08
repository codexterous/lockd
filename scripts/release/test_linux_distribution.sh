#!/bin/sh
# Exercise the published wheel with the distro's own supported Python interpreter.
set -eu
case "$(uname -m)" in
    x86_64) arch=x86_64 ;;
    aarch64) arch=arm64 ;;
    *) echo 'Unsupported Linux wheel architecture.' >&2; exit 1 ;;
esac
export PYTHONDONTWRITEBYTECODE=1
python3 scripts/release/test_linux_wheel.py --distributions /wheels \
    --daemon "output/$arch/opt/shared/lockd"
