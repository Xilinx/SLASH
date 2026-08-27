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
    echo "  --clean   Hand over to scripts/uninstall-slash.sh (skip install and verification)"
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

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# --clean is exactly what scripts/uninstall-slash.sh does, and more thoroughly: it also
# stops vrtd and unloads the kernel modules. Hand over rather than keep a second, weaker
# implementation of the same thing here.
if [[ "${CLEAN_ONLY}" == "true" ]]; then
    exec bash "${SCRIPT_DIR}/uninstall-slash.sh" --yes
fi

# SLASH root
cd "$(dirname "$0")/.."

# Package names and distro detection are shared with uninstall-slash.sh.
# shellcheck source=scripts/slash-packages.sh
source "${SCRIPT_DIR}/slash-packages.sh"

slash_detect_pkg_type || exit 1
PKG_TYPE="${SLASH_PKG_TYPE}"
PRETTY_NAME="${SLASH_PKG_DISTRO}"

echo "Detected package type: ${PKG_TYPE} (distro: ${PRETTY_NAME})"

# =========================================================================
#  DEB workflow
# =========================================================================

if [[ "${PKG_TYPE}" == "deb" ]]; then
    echo ""
    echo "========================================================================"
    echo "  Stage 1: Purge existing SLASH packages (DEB)"
    echo "========================================================================"

    mapfile -t INSTALLED < <(slash_installed_packages)

    if [[ ${#INSTALLED[@]} -gt 0 ]]; then
        echo "Purging: ${INSTALLED[*]}"
        apt-get purge -y --allow-change-held-packages "${INSTALLED[@]}"
        apt-get autoremove --purge -y
    else
        echo "No SLASH packages currently installed."
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

    apt-get install -y --allow-change-held-packages "${ARTIFACTS_DIR}"/*.deb

# =========================================================================
#  RPM workflow
# =========================================================================

elif [[ "${PKG_TYPE}" == "rpm" ]]; then
    echo ""
    echo "========================================================================"
    echo "  Stage 1: Remove existing SLASH packages (RPM)"
    echo "========================================================================"

    mapfile -t INSTALLED < <(slash_installed_packages)

    if [[ ${#INSTALLED[@]} -gt 0 ]]; then
        echo "Removing: ${INSTALLED[*]}"
        dnf remove -y --setopt='*.skip_if_unavailable=True' "${INSTALLED[@]}"
    else
        echo "No SLASH packages currently installed."
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
    dnf install -y  --setopt='*.skip_if_unavailable=True' "${RPMS[@]}"
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
    if slash_package_installed "${pkg}"; then
        RESULTS+=("${pkg}: INSTALLED")
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        RESULTS+=("${pkg}: MISSING")
        FAIL_COUNT=$((FAIL_COUNT + 1))
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
