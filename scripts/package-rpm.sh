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

set -euxo pipefail

# SLASH root
cd "$(dirname "$0")/.."

VERSION="$(tr -d '[:space:]' < packaging/version)"
TOPDIR="$(pwd)/rpmbuild"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$(pwd)/rpm}"

rm -rf "${TOPDIR}" "${ARTIFACTS_DIR}"
mkdir -p "${TOPDIR}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
mkdir -p "${ARTIFACTS_DIR}"

# Create source tarball (rpmbuild expects name-version/ inside)
tar czf "${TOPDIR}/SOURCES/slash-${VERSION}.tar.gz" \
    --transform="s,^\.,slash-${VERSION}," \
    --exclude='.git' \
    --exclude='rpmbuild' \
    --exclude='rpm' \
    --exclude='deb' \
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
