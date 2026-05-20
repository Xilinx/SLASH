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

if [[ $# -ne 1 ]]; then
    LIBDIR=lib
else
    LIBDIR="$1"
fi

COMMON_CMAKE_OPTIONS=(
    "-DCMAKE_INSTALL_PREFIX=/usr"
    "-DCMAKE_INSTALL_BINDIR=bin"
    "-DCMAKE_INSTALL_LIBDIR=${LIBDIR}"
    "-DCMAKE_INSTALL_SYSCONF=/etc"
    "-DCMAKE_INSTALL_LOCALSTATEDIR=/var"
    # These get stripped to separate debug symbol deb files
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
)

cmake -B pbuild/smi -S smi -G Ninja -DSMI_INCLUDE_VRT=ON -DVRT_INCLUDE_VRTD=ON -DVRTD_INCLUDE_LIBSLASH=ON "${COMMON_CMAKE_OPTIONS[@]}"
cmake -B pbuild/cmake-tools -S cmake -G Ninja "${COMMON_CMAKE_OPTIONS[@]}"
