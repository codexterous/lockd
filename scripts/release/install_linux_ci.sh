#!/bin/sh
# Package installation is confined to the disposable compatibility container.
set -eu
if [ "${GITHUB_ACTIONS:-}" != true ]; then
    echo 'This script installs system packages and is only for disposable CI containers.' >&2
    exit 1
fi

case "${1:-}" in
    apt)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends ca-certificates g++ cmake ninja-build \
            libboost-dev libboost-log-dev libboost-program-options-dev \
            libboost-filesystem-dev
        if [ -n "${2:-}" ]; then
            apt-get install -y --no-install-recommends python3-pip python3-venv
        fi
        ;;
    dnf)
        dnf install -y gcc-c++ cmake ninja-build boost-devel
        if [ -n "${2:-}" ]; then dnf install -y python3-pip; fi
        ;;
    dnf-crb)
        # Enterprise Linux ships Ninja in CRB, which is disabled by default.
        # Enable it for this transaction only; Fedora does not have this repository.
        dnf --enablerepo=crb install -y gcc-c++ cmake ninja-build boost-devel
        if [ -n "${2:-}" ]; then dnf install -y python3-pip; fi
        ;;
    zypper)
        zypper --non-interactive refresh
        zypper --non-interactive install --no-recommends gcc-c++ cmake ninja \
            boost-devel libboost_log-devel libboost_program_options-devel \
            libboost_filesystem-devel libboost_thread-devel
        if [ -n "${2:-}" ]; then zypper --non-interactive install python3-pip; fi
        ;;
    pacman)
        pacman -Syu --noconfirm --needed gcc cmake ninja boost boost-libs
        if [ -n "${2:-}" ]; then pacman -S --noconfirm --needed python-pip; fi
        ;;
    *)
        echo 'Expected package manager: apt, dnf, dnf-crb, zypper, or pacman.' >&2
        exit 1
        ;;
esac
