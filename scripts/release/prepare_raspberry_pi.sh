#!/bin/bash
# Import the official Raspberry Pi OS image on a disposable ARM64 CI runner.
set -euo pipefail
if [ "${GITHUB_ACTIONS:-}" != true ] || [ "$(uname -m)" != aarch64 ]; then
    echo 'This script requires a disposable ARM64 GitHub Actions runner.' >&2
    exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work="$root/output/obj/arm64/opt/shared/dependencies/raspberry_pi"
mkdir -p "$work/rootfs"
cd "$work"

# Pin the official Lite image and its published checksum; do not substitute Debian.
image=2026-10-06-raspios-trixie-arm64-lite.img
url="https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2026-10-06/$image.xz"
curl --fail --location --retry 3 "$url" --output "$image.xz"
printf '%s  %s\n' \
    483db18a48da399b5b7022ffae9a07bc6d99daab30e593aedac09b69ff422843 \
    "$image.xz" | sha256sum --check
xz --decompress --force "$image.xz"

loop_device=''
cleanup()
{
    if mountpoint -q "$work/rootfs"; then sudo umount "$work/rootfs"; fi
    if [ -n "$loop_device" ]; then sudo losetup --detach "$loop_device"; fi
}
trap cleanup EXIT

# Import only the root partition. Docker supplies its own /proc, /dev and DNS;
# the Pi boot partition and kernel are not used by this user-space test.
loop_device=$(sudo losetup --find --show --partscan --read-only "$image")
sudo udevadm settle
sudo mount -o ro,noload "${loop_device}p2" "$work/rootfs"
sudo tar -C "$work/rootfs" -c . | docker import - lockd-raspberry-pi-ci
sudo umount "$work/rootfs"
sudo losetup --detach "$loop_device"
loop_device=''
rm -- "$image"
