#!/usr/bin/env bash
# Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Install development dependencies on Ubuntu/Debian.
#
# Installs everything needed to build SLASH from source, produce .deb
# packages, and run RP1 firmware QEMU tests.
#
# Sources:
#   packaging/debian/control          (Build-Depends)
#   docs/howto/install-from-packages.rst   (build-machine dependencies)
#
# Usage: sudo ./scripts/install-dev-deps-ubuntu.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: This script must be run as root (use sudo)"
    exit 1
fi

apt-get update

# ---------------------------------------------------------------------------
# SLASH build dependencies (from packaging/debian/control Build-Depends
# and docs/howto/install-from-packages.rst)
# ---------------------------------------------------------------------------
apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    rsync \
    git \
    dkms \
    python3 \
    python3-pip \
    libcli11-dev \
    libinih-dev \
    libjsoncpp-dev \
    libsystemd-dev \
    libxml2-dev \
    libzmq3-dev \
    zlib1g-dev

# dh-dkms: split out of dkms on Ubuntu 24.04+; needed for debhelper DKMS
# integration. On older releases the package does not exist and dkms alone
# is sufficient.
apt-get install -y dh-dkms 2>/dev/null || true

# Debian/Ubuntu packaging tools
apt-get install -y \
    debhelper \
    dpkg-dev \
    apt-utils

# ---------------------------------------------------------------------------
# Xilinx QEMU build dependencies (for RP1 firmware virtual testing)
# ---------------------------------------------------------------------------
apt-get install -y \
    libglib2.0-dev \
    libgcrypt20-dev \
    libpixman-1-dev \
    libfdt-dev \
    device-tree-compiler \
    meson \
    python3-venv \
    gcc-arm-none-eabi

# ---------------------------------------------------------------------------
# Kernel headers (needed for driver / DKMS builds)
# ---------------------------------------------------------------------------
KERN="linux-headers-$(uname -r)"
if ! apt-get install -y "${KERN}" 2>/dev/null; then
    echo ""
    echo "WARNING: Could not install ${KERN}"
    echo "  This is expected in containers and WSL environments."
    echo "  Driver builds will fail, but firmware and QEMU builds will work."
fi

echo ""
echo "Done. Development dependencies installed."
