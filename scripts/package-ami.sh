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
PKG_PY="${AMI_DIR}/scripts/package_data/pkg.py"
GEN_PKG_PY="${AMI_DIR}/scripts/gen_package.py"
AMI_PROGRAM_C="${AMI_DIR}/driver/ami_program.c"

rm -rf "${AMI_BUILD_DIR}"
mkdir -p "${ARTIFACTS_DIR}"

# Restore submodule files and clean up build directory on exit
trap 'git -C "${AVED_DIR}" checkout -- sw/AMI/scripts/package_data/pkg.py sw/AMI/scripts/gen_package.py sw/AMI/driver/ami_program.c; rm -rf "${AMI_BUILD_DIR}"' EXIT

# Patch in Rocky Linux support (RHEL-compatible, RPM-based)
sed -i "/^DIST_ID_RHEL /a DIST_ID_ROCKY   = 'rocky'" "${PKG_PY}"
sed -i "/^    DIST_ID_RHEL,$/a\\    DIST_ID_ROCKY," "${PKG_PY}"
sed -i "s/DIST_RPM = \[DIST_ID_CENTOS, DIST_ID_REDHAT, DIST_ID_REDHAT2, DIST_ID_SLES, DIST_ID_RHEL\]/DIST_RPM = [DIST_ID_CENTOS, DIST_ID_REDHAT, DIST_ID_REDHAT2, DIST_ID_SLES, DIST_ID_RHEL, DIST_ID_ROCKY]/" "${PKG_PY}"
sed -i "s/DIST_ID_CENTOS, DIST_ID_REDHAT, DIST_ID_REDHAT2, DIST_ID_RHEL\]/DIST_ID_CENTOS, DIST_ID_REDHAT, DIST_ID_REDHAT2, DIST_ID_RHEL, DIST_ID_ROCKY]/" "${GEN_PKG_PY}"

# Extend the eventfd_signal() version gate in ami_program.c so the
# void-arg form is also picked up on RHEL 9.5+, which is when Red Hat
# backported the upstream 6.8 eventfd_signal() simplification into the
# 5.14-based kernel.
#
# Stopgap: revert once upstream AVED carries an equivalent fix.
patch --no-backup-if-mismatch -p1 -d "${AVED_DIR}" <<'EOF'
--- a/sw/AMI/driver/ami_program.c
+++ b/sw/AMI/driver/ami_program.c
@@ -94,7 +94,15 @@
 				#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
 					eventfd_signal(efd_ctx);
 				#else
+				# ifdef RHEL_RELEASE_CODE
+				#  if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 5)
+					eventfd_signal(efd_ctx);
+				#  else
+					eventfd_signal(efd_ctx, bytes_to_write);
+				#  endif
+				# else
 					eventfd_signal(efd_ctx, bytes_to_write);
+				# endif
 				#endif
 			}
 		} else {
@@ -153,7 +161,15 @@
 		#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
 			eventfd_signal(efd_ctx);
 		#else
+		# ifdef RHEL_RELEASE_CODE
+		#  if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 5)
+			eventfd_signal(efd_ctx);
+		#  else
+			eventfd_signal(efd_ctx, (PDI_CHUNK_SIZE * PDI_CHUNK_MULTIPLIER));
+		#  endif
+		# else
 			eventfd_signal(efd_ctx, (PDI_CHUNK_SIZE * PDI_CHUNK_MULTIPLIER));
+		# endif
 		#endif
 	}
 
EOF

cd "${AMI_DIR}"
# --no_driver skips a pre-flight driver compilation check (build+clean) only;
# it does NOT affect which files are included in the package.
# We skip it here so the packaging can run in environments (eg. containers)
# that may not have linux-headers available to compile the driver.
python3 scripts/gen_package.py --no_driver -o "${AMI_BUILD_DIR}"

# Copy only the package files to the artifacts directory
cp "${AMI_BUILD_DIR}"/*.rpm "${ARTIFACTS_DIR}/" 2>/dev/null || \
cp "${AMI_BUILD_DIR}"/*.deb "${ARTIFACTS_DIR}/" 2>/dev/null || true
