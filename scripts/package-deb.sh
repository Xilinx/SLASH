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

NONINTERACTIVE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --noninteractive) NONINTERACTIVE=1; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# SLASH root
cd "$(dirname "$0")/.."

VERSION="$(tr -d '[:space:]' < packaging/version)"

export ARTIFACTS_DIR="${ARTIFACTS_DIR:-"$(pwd)"/deb}"
export DPKG_ARCH="$(dpkg --print-architecture)"
export DPKG_PARSED_VERSION="$(dpkg-parsechangelog -SVersion)"

# Warn before overwriting an existing build
if [[ -d "${ARTIFACTS_DIR}" ]] && [[ -t 0 ]] && [[ "${NONINTERACTIVE}" -eq 0 ]]; then
    echo "WARNING: A previous .deb build already exists." >&2
    echo "Proceeding will remove the following directories and restart the build from scratch:" >&2
    echo "  ${ARTIFACTS_DIR}  (built .deb packages)" >&2
    echo "  pbuild/           (CMake build tree)" >&2
    echo "  debian/           (generated packaging metadata)" >&2
    echo "  linker/install.prj" >&2
    echo "  linker/slashkit/resources/static_shell" >&2
    echo "This includes the static shell, which can take several hours to rebuild." >&2
    read -r -p "Overwrite existing build and start from scratch? [y/N] " _answer </dev/tty
    case "${_answer}" in
        [yY]|[yY][eE][sS]) ;;
        *) echo "Aborted." >&2; exit 1 ;;
    esac
fi

# Check build prerequisites
_prereq_ok=1

if ! command -v v++ > /dev/null 2>&1; then
    echo "ERROR: v++ not found in PATH. Source Vitis 2025.1 before building:" >&2
    echo "  source <path-to-vitis>/settings64.sh" >&2
    echo "See docs/tutorials/admin/platform-setup.rst for details." >&2
    _prereq_ok=0
fi

if ! compgen -G 'linker/slashkit/resources/base/iprepo/smbus*/' > /dev/null 2>&1; then
    echo "ERROR: SMBus IP (xilinx.com:ip:smbus:1.1) not found in linker/slashkit/resources/base/iprepo/." >&2
    echo "Download it from https://www.xilinx.com/member/v80.html and place the IP" >&2
    echo "directory into linker/slashkit/resources/base/iprepo/ before building." >&2
    echo "See docs/tutorials/admin/platform-setup.rst for details." >&2
    _prereq_ok=0
fi

if [[ "${_prereq_ok}" -eq 0 ]]; then
    exit 1
fi

set -x

# Clean build
rm -rf deb
mkdir -p deb
rm -rf pbuild
rm -rf debian

rsync -a packaging/debian/ ./debian/

sed -i "1s/(UNRELEASED) UNRELEASED;/(${VERSION}) unstable;/" debian/changelog

# Substitute the packaging version into DKMS metadata files.
sed -i "s/@VERSION@/${VERSION}/g" \
    debian/slash-dkms.dkms \
    debian/slash-dkms.install

rsync vrt/vrtd/systemd/vrtd.service debian/vrtd.service
rsync vrt/vrtd/systemd/vrtd.socket  debian/vrtd.socket
rsync vrt/vrtd/udev/99-vrtd.rules   debian/vrtd.udev

# It should be noted that while the buildinfo file and the changes file are rerouted here
# from their default location of `..`, the .dsc and .tar.xz files are rerouted by the debian/rules
# file by manual move because there apparently is no way to convince dpkg-source to write anywhere other
# than to `..`.
#
# This means that `..` has to be writable by the user building, which is inconvenient.
dpkg-buildpackage \
    --no-sign \
    --build=binary \
    --diff-ignore='.*' \
    --buildinfo-option="-u${ARTIFACTS_DIR}" \
    --buildinfo-file"=${ARTIFACTS_DIR}/slash_${DPKG_PARSED_VERSION}_${DPKG_ARCH}.buildinfo" \
    --changes-option="-u${ARTIFACTS_DIR}" \
    --changes-file="${ARTIFACTS_DIR}/slash_${DPKG_PARSED_VERSION}_${DPKG_ARCH}.changes"

# Build AMI package into the same artifacts directory
ARTIFACTS_DIR="${ARTIFACTS_DIR}" "$(dirname "$0")/package-ami.sh"

cd "${ARTIFACTS_DIR:-deb}"
apt-ftparchive packages . > Packages
apt-ftparchive release . > Release
