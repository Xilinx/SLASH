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
TOPDIR="$(pwd)/rpmbuild"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$(pwd)/rpm}"

# Warn before overwriting an existing build
if [[ -d "${ARTIFACTS_DIR}" ]] && [[ -t 0 ]] && [[ "${NONINTERACTIVE}" -eq 0 ]]; then
    echo "WARNING: A previous .rpm build already exists." >&2
    echo "Proceeding will remove the following directories and restart the build from scratch:" >&2
    [[ -d "${TOPDIR}" ]]        && echo "  ${TOPDIR}  (rpmbuild tree)" >&2
    [[ -d "${ARTIFACTS_DIR}" ]] && echo "  ${ARTIFACTS_DIR}  (built .rpm packages)" >&2
    [[ -d pbuild ]]             && echo "  pbuild/  (CMake build tree)" >&2
    echo "  linker/install.prj" >&2
    echo "  linker/slashkit/resources/static_shell" >&2
    echo "This includes the static shell, which can take several hours to rebuild." >&2
    read -r -p "Overwrite existing build and start from scratch? [y/N] " _answer </dev/tty
    case "${_answer}" in
        [yY]|[yY][eE][sS]) ;;
        *) echo "Aborted." >&2; exit 1 ;;
    esac
fi

if [[ "${NONINTERACTIVE}" -eq 1 ]]; then
    export SLASH_NONINTERACTIVE=1
fi

# Build the FPGA base shell (Vivado synth/impl, ~5h) up-front, before the source tarball is
# created below -- rpmbuild builds from that tarball, so static_shell/ must exist first.
# Prerequisite checks (v++, SMBus IP) live inside this script. Expert users can redirect
# just this step to a build farm: either set SLASH_BASE_SHELL_LAUNCHER (see
# scripts/build-base-shell.sh) or edit the line below to wrap it with your scheduler
# (e.g. bsub/qsub). Everything after this builds on the local machine.
"$(dirname "$0")/build-base-shell.sh"

set -x

rm -rf "${TOPDIR}" "${ARTIFACTS_DIR}" pbuild
mkdir -p "${TOPDIR}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
mkdir -p "${ARTIFACTS_DIR}"

# Create source tarball (rpmbuild expects name-version/ inside)
tar czf "${TOPDIR}/SOURCES/slash-${VERSION}.tar.gz" \
    --transform="s,^\.,slash-${VERSION}," \
    --exclude='.git' \
    --exclude='rpmbuild' \
    --exclude='rpm' \
    --exclude='deb' \
    --exclude='docker-build' \
    --exclude='pbuild' \
    .

cp packaging/rpm/slash.spec "${TOPDIR}/SPECS/"

rpmbuild \
    --define "_topdir ${TOPDIR}" \
    --define "_version ${VERSION}" \
    -bb "${TOPDIR}/SPECS/slash.spec"

cp "${TOPDIR}"/RPMS/*/*.rpm "${ARTIFACTS_DIR}/"

# Build AMI package into the same artifacts directory
ARTIFACTS_DIR="${ARTIFACTS_DIR}" "$(dirname "$0")/package-ami.sh"

pushd "${ARTIFACTS_DIR}"
createrepo .
popd

echo "RPMs available in ${ARTIFACTS_DIR}/"
