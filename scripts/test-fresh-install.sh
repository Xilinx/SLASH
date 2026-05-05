#!/bin/bash

# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

set -euo pipefail

CLEAN_ONLY=false

usage() {
    echo "Usage: $0 [--clean]"
    echo ""
    echo "  --clean   Remove installed SLASH packages only (skip install and verification)"
    exit 1
}

# Parse long options
while [[ $# -gt 0 && "$1" == --* ]]; do
    case "$1" in
        --clean)
            CLEAN_ONLY=true
            shift
            ;;
        *)
            echo "ERROR: Unknown option '$1'"
            usage
            ;;
    esac
done

if [[ $# -gt 0 ]]; then
    echo "ERROR: Unexpected argument '$1'"
    usage
fi

# SLASH root
cd "$(dirname "$0")/.."

# =========================================================================
#  Detect package manager from /etc/os-release
# =========================================================================

if [[ ! -f /etc/os-release ]]; then
    echo "ERROR: /etc/os-release not found. Cannot detect distribution."
    exit 1
fi

source /etc/os-release

case "${ID_LIKE:-$ID}" in
    *debian*|*ubuntu*)
        PKG_TYPE="deb"
        ;;
    *rhel*|*fedora*|*centos*|*suse*)
        PKG_TYPE="rpm"
        ;;
    *)
        # Fallback: check ID directly
        case "${ID}" in
            debian|ubuntu|linuxmint|pop)
                PKG_TYPE="deb"
                ;;
            rhel|fedora|centos|rocky|alma|ol|sles|opensuse*)
                PKG_TYPE="rpm"
                ;;
            *)
                echo "ERROR: Unsupported distribution: ${ID} (ID_LIKE=${ID_LIKE:-unset})"
                exit 1
                ;;
        esac
        ;;
esac

echo "Detected package type: ${PKG_TYPE} (distro: ${PRETTY_NAME})"

# =========================================================================
#  Package lists
# =========================================================================

DEB_PACKAGES=(
    slash
    slash-dev
    slash-sim-emu
    slash-sim-emu-dev
    slash-dkms
    libslash
    libslash-dev
    vrtd
    libvrtd
    libvrtd-dev
    libvrt
    libvrt-dev
    v80-smi
    slashkit
    ami
)

RPM_PACKAGES=(
    slash
    slash-devel
    slash-sim-emu
    slash-sim-emu-devel
    slash-dkms
    libslash
    libslash-devel
    vrtd
    libvrtd
    libvrtd-devel
    libvrt
    libvrt-devel
    v80-smi
    slashkit
    ami
)

# =========================================================================
#  DEB workflow
# =========================================================================

if [[ "${PKG_TYPE}" == "deb" ]]; then
    echo ""
    echo "========================================================================"
    echo "  Stage 1: Purge existing SLASH packages (DEB)"
    echo "========================================================================"

    INSTALLED=()
    for pkg in "${DEB_PACKAGES[@]}"; do
        if dpkg -l "${pkg}" 2>/dev/null | grep -q '^ii'; then
            INSTALLED+=("${pkg}")
        fi
    done

    if [[ ${#INSTALLED[@]} -gt 0 ]]; then
        echo "Purging: ${INSTALLED[*]}"
        apt-get purge -y "${INSTALLED[@]}"
        apt-get autoremove --purge -y
    else
        echo "No SLASH packages currently installed."
    fi

    if [[ "${CLEAN_ONLY}" == "true" ]]; then
        echo ""
        echo "========================================================================"
        echo "  --clean enabled: stopping after Stage 1 purge"
        echo "========================================================================"
        exit 0
    fi

    ARTIFACTS_DIR="${ARTIFACTS_DIR:-$(pwd)/deb}"

    if [[ ! -d "${ARTIFACTS_DIR}" ]]; then
        echo "ERROR: DEB artifacts directory not found: ${ARTIFACTS_DIR}"
        echo "       Run scripts/package-deb.sh first."
        exit 1
    fi

    echo ""
    echo "========================================================================"
    echo "  Stage 2: Install SLASH packages from ${ARTIFACTS_DIR}"
    echo "========================================================================"

    apt-get install -y "${ARTIFACTS_DIR}"/*.deb

# =========================================================================
#  RPM workflow
# =========================================================================

elif [[ "${PKG_TYPE}" == "rpm" ]]; then
    echo ""
    echo "========================================================================"
    echo "  Stage 1: Remove existing SLASH packages (RPM)"
    echo "========================================================================"

    INSTALLED=()
    for pkg in "${RPM_PACKAGES[@]}"; do
        if rpm -q "${pkg}" &>/dev/null; then
            INSTALLED+=("${pkg}")
        fi
    done

    if [[ ${#INSTALLED[@]} -gt 0 ]]; then
        echo "Removing: ${INSTALLED[*]}"
        dnf remove -y "${INSTALLED[@]}"
    else
        echo "No SLASH packages currently installed."
    fi

    if [[ "${CLEAN_ONLY}" == "true" ]]; then
        echo ""
        echo "========================================================================"
        echo "  --clean enabled: stopping after Stage 1 removal"
        echo "========================================================================"
        exit 0
    fi

    ARTIFACTS_DIR="${ARTIFACTS_DIR:-$(pwd)/rpm}"

    if [[ ! -d "${ARTIFACTS_DIR}" ]]; then
        echo "ERROR: RPM artifacts directory not found: ${ARTIFACTS_DIR}"
        echo "       Run scripts/package-rpm.sh first."
        exit 1
    fi

    echo ""
    echo "========================================================================"
    echo "  Stage 2: Install SLASH packages from ${ARTIFACTS_DIR}"
    echo "========================================================================"

    # Exclude source, debuginfo, and debugsource RPMs
    mapfile -t RPMS < <(find "${ARTIFACTS_DIR}" -maxdepth 1 -name '*.rpm' \
        ! -name '*.src.rpm' ! -name '*-debuginfo-*' ! -name '*-debugsource-*')
    dnf install -y "${RPMS[@]}"
fi

# =========================================================================
#  Verify
# =========================================================================

echo ""
echo "========================================================================"
echo "  Stage 3: Verify installation"
echo "========================================================================"

PASS_COUNT=0
FAIL_COUNT=0
RESULTS=()

if [[ "${PKG_TYPE}" == "deb" ]]; then
    PACKAGES=("${DEB_PACKAGES[@]}")
else
    PACKAGES=("${RPM_PACKAGES[@]}")
fi

for pkg in "${PACKAGES[@]}"; do
    if [[ "${PKG_TYPE}" == "deb" ]]; then
        if dpkg -l "${pkg}" 2>/dev/null | grep -q '^ii'; then
            RESULTS+=("${pkg}: INSTALLED")
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            RESULTS+=("${pkg}: MISSING")
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    else
        if rpm -q "${pkg}" &>/dev/null; then
            RESULTS+=("${pkg}: INSTALLED")
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            RESULTS+=("${pkg}: MISSING")
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    fi
done

echo ""
echo "========================================================================"
echo "  Summary  |  Type: ${PKG_TYPE}  |  Distro: ${PRETTY_NAME}"
echo "========================================================================"
for result in "${RESULTS[@]}"; do
    echo "  ${result}"
done
echo "------------------------------------------------------------------------"
echo "  Installed: ${PASS_COUNT}  |  Missing: ${FAIL_COUNT}"
echo "========================================================================"

if [[ ${FAIL_COUNT} -gt 0 ]]; then
    echo ""
    echo "WARNING: ${FAIL_COUNT} package(s) failed to install."
    exit 1
fi

echo ""
echo "All SLASH packages installed successfully."
