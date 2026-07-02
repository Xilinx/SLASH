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

# Ensure directories created during packaging have standard permissions.
# dpkg-deb requires the control directory to be >=0755 and <=0775.
umask 0022

# SLASH root
cd "$(dirname "$0")/.."

ARTIFACTS_DIR="${ARTIFACTS_DIR:-$(pwd)/ami}"
AMI_BUILD_DIR="$(pwd)/ami-build"
AVED_DIR="$(pwd)/submodules/AVED"
AMI_DIR="${AVED_DIR}/sw/AMI"
AMI_SRC_DIR="${AMI_BUILD_DIR}/src/AMI"
AMI_OUTPUT_DIR="${AMI_BUILD_DIR}/pkg"
PKG_PY="${AMI_SRC_DIR}/scripts/package_data/pkg.py"
GEN_PKG_PY="${AMI_SRC_DIR}/scripts/gen_package.py"

rm -rf "${AMI_BUILD_DIR}"
mkdir -p "${ARTIFACTS_DIR}"
mkdir -p "$(dirname "${AMI_SRC_DIR}")"
cp -a "${AMI_DIR}" "${AMI_SRC_DIR}"

# Clean up build directory on exit. Packaging patches a disposable AMI copy so
# this also works from source trees copied without usable submodule gitdirs.
trap 'rm -rf "${AMI_BUILD_DIR}"' EXIT

# Avoid stale generated headers from copied build trees. gen_package.py will
# otherwise prefer api/build/ami_version.h over the checked-in version header.
rm -f "${AMI_SRC_DIR}/api/build/ami_version.h"

# Patch in Rocky Linux support (RHEL-compatible, RPM-based)
sed -i "/^DIST_ID_RHEL /a DIST_ID_ROCKY   = 'rocky'" "${PKG_PY}"
sed -i "/^    DIST_ID_RHEL,$/a\\    DIST_ID_ROCKY," "${PKG_PY}"
sed -i "s/DIST_RPM = \[DIST_ID_CENTOS, DIST_ID_REDHAT, DIST_ID_REDHAT2, DIST_ID_SLES, DIST_ID_RHEL\]/DIST_RPM = [DIST_ID_CENTOS, DIST_ID_REDHAT, DIST_ID_REDHAT2, DIST_ID_SLES, DIST_ID_RHEL, DIST_ID_ROCKY]/" "${PKG_PY}"
sed -i "s/DIST_ID_CENTOS, DIST_ID_REDHAT, DIST_ID_REDHAT2, DIST_ID_RHEL\]/DIST_ID_CENTOS, DIST_ID_REDHAT, DIST_ID_REDHAT2, DIST_ID_RHEL, DIST_ID_ROCKY]/" "${GEN_PKG_PY}"

cd "${AMI_SRC_DIR}"
# --no_driver skips a pre-flight driver compilation check (build+clean) only;
# it does NOT affect which files are included in the package.
# We skip it here so the packaging can run in environments (eg. containers)
# that may not have linux-headers available to compile the driver.
#
# --no_gen_version skips AVED's git-based version regeneration. This wrapper is
# often run from copied worktrees where the submodule .git file points back to a
# non-existent source checkout, causing an empty hash and an invalid RPM Release.
python3 scripts/gen_package.py --no_driver --no_gen_version -o "${AMI_OUTPUT_DIR}"

# Copy only the package files to the artifacts directory
cp "${AMI_OUTPUT_DIR}"/*.rpm "${ARTIFACTS_DIR}/" 2>/dev/null || \
cp "${AMI_OUTPUT_DIR}"/*.deb "${ARTIFACTS_DIR}/" 2>/dev/null || true
