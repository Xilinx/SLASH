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

if [ $# -ne 1 ]; then
    echo "Usage: <ubuntu|rocky>" 2>&1
    exit 1
fi

DOCKER_RUN_ARGS=" "
DOCKER_RUN_ARGS+="--rm "

# Using the current working directory in the container
DOCKER_RUN_ARGS+="-v $PWD:$PWD "
DOCKER_RUN_ARGS+="-w $PWD "

# Mounting the Xilinx toolchain in the container
if [ -z $SLASH_XILINX_PATH ]; then
    echo "Please set SLASH_XILINX_PATH to the path of your Xilinx tools installation (e.g. /opt/Xilinx)" 2&1
    exit 1
fi

if [ -z $SLASH_XILINX_ROOT ]; then
    SLASH_XILINX_ROOT=$SLASH_XILINX_PATH
fi

DOCKER_RUN_ARGS+="-v $SLASH_XILINX_ROOT:$SLASH_XILINX_ROOT "

# Mounting the license file for synthesis and implementation
if [ -z $SLASH_LICENSE_PATH ]; then
    echo "Please set SLASH_LICENSE_PATH to the path of your licenses (.e.g. /proj/xbuilds/licenses)" 2>&2
    exit 1
fi

DOCKER_RUN_ARGS+="-v $SLASH_LICENSE_PATH:$SLASH_LICENSE_PATH "
DOCKER_RUN_ARGS+="-e XILINXD_LICENSE_FILE=$SLASH_LICENSE_PATH "

# If set, add the skip-root-build flag
if [ -n $SLASH_PKG_SKIP_ROOT_DESIGN_BUILD ]; then
    DOCKER_RUN_ARGS+="-e SLASH_PKG_SKIP_ROOT_DESIGN_BUILD=$SLASH_PKG_SKIP_ROOT_DESIGN_BUILD "
fi

DISTRO=$1

if [ $DISTRO = "ubuntu" ]; then
    BUILD_SCRIPT="./scripts/package-deb.sh"
elif [ $DISTRO = "rocky" ]; then
    BUILD_SCRIPT="./scripts/package-rpm.sh"
else
    echo "Unknown Linux distro $DISTRO" 2>&1
    exit 1
fi

docker build --build-arg USER_ID=$(id -u) -t "slash-build-$DISTRO" -f "scripts/Dockerfile.build-$DISTRO" .
docker run $DOCKER_RUN_ARGS \
    "slash-build-$DISTRO" \
    bash -c "source $SLASH_XILINX_PATH/2025.1/Vitis/settings64.sh && $BUILD_SCRIPT"
