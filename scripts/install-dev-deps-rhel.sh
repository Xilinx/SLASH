#!/usr/bin/env bash
# Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Install development dependencies on RHEL/CentOS/Rocky/Alma/Fedora.
#
# Installs everything needed to build SLASH from source, produce .rpm
# packages, and run RP1 firmware QEMU tests.
#
# Sources:
#   packaging/rpm/slash.spec          (BuildRequires)
#   docs/howto/install-from-packages.rst   (build-machine dependencies)
#
# Usage: sudo ./scripts/install-dev-deps-rhel.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: This script must be run as root (use sudo)"
    exit 1
fi

# Detect package manager
if command -v dnf &>/dev/null; then
    PM=dnf
elif command -v yum &>/dev/null; then
    PM=yum
else
    echo "ERROR: Neither dnf nor yum found"
    exit 1
fi

# EPEL is needed on RHEL/CentOS for some -devel packages
if [ -f /etc/redhat-release ] && ! rpm -q epel-release &>/dev/null; then
    $PM install -y epel-release 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# SLASH build dependencies (from packaging/rpm/slash.spec BuildRequires
# and docs/howto/install-from-packages.rst)
# ---------------------------------------------------------------------------
$PM install -y \
    gcc \
    gcc-c++ \
    cmake \
    make \
    ninja-build \
    pkg-config \
    rsync \
    git \
    dkms \
    cli11-devel \
    cppzmq-devel \
    inih-devel \
    jsoncpp-devel \
    libxml2-devel \
    systemd-devel \
    zeromq-devel \
    zlib-devel

# Python: RHEL 9 ships python3.9 which is too old (v80++ needs >= 3.10).
# Try versioned python3.11+ first, fall back to system python3.
if $PM install -y python3.11 python3.11-pip 2>/dev/null; then
    echo "Installed python3.11"
elif $PM install -y python3.12 python3.12-pip 2>/dev/null; then
    echo "Installed python3.12"
elif $PM install -y python3.13 python3.13-pip 2>/dev/null; then
    echo "Installed python3.13"
elif $PM install -y python3 python3-pip 2>/dev/null; then
    echo "Installed system python3 (may be too old on RHEL 9)"
else
    echo "WARNING: Could not install python3. Install it manually."
fi

# RPM packaging tools
$PM install -y \
    rpm-build \
    createrepo_c \
    systemd-rpm-macros

# ---------------------------------------------------------------------------
# Xilinx QEMU build dependencies (for RP1 firmware virtual testing)
# ---------------------------------------------------------------------------
$PM install -y \
    glib2-devel \
    libgcrypt-devel \
    pixman-devel \
    libfdt-devel \
    dtc \
    meson \
    python3-devel \
    arm-none-eabi-gcc-cs

# ---------------------------------------------------------------------------
# Kernel headers (needed for driver / DKMS builds)
# ---------------------------------------------------------------------------
KERN="kernel-devel-$(uname -r)"
if ! $PM install -y "${KERN}" 2>/dev/null; then
    echo ""
    echo "WARNING: Could not install ${KERN}"
    echo "  This is expected in containers and WSL environments."
    echo "  Driver builds will fail, but firmware and QEMU builds will work."
fi

echo ""
echo "Done. Development dependencies installed."
