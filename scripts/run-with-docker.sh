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

set -exo pipefail

# Usage: scripts/run-with-docker.sh <run|package> <ubuntu|rocky>
#
# Builds (if necessary) and runs one of the SLASH Docker containers defined by
# scripts/Dockerfile.<run|package>-<ubuntu|rocky>. The current working
# directory and the Xilinx tools install (plus optionally a separate license
# path) are mounted into the container at the same paths they have on the
# host so that paths generated inside the container are also valid outside
# of it.
#
# Modes:
#   package   Run the matching distro's packaging script
#             (scripts/package-deb.sh on Ubuntu, scripts/package-rpm.sh on
#             Rocky) inside a clean container that only has the build
#             dependencies installed.
#   run       Drop into an interactive bash shell inside a container that has
#             the freshly built SLASH packages already installed.
#
# Required environment variables:
#   SLASH_XILINX_PATH   Path to the Xilinx tools install on the host
#                       (e.g. /opt/Xilinx). Vivado is sourced from
#                       $SLASH_XILINX_PATH/2025.1/Vivado/settings64.sh inside
#                       the container.
#
# Optional environment variables:
#   SLASH_XILINX_ROOT              Mount point for the Xilinx tools inside the
#                                  container. Defaults to SLASH_XILINX_PATH so
#                                  paths match host and container.
#   SLASH_LICENSE_PATH             Path to the Xilinx license file (or
#                                  directory) on the host. When set, it is
#                                  mounted into the container and exported as
#                                  XILINXD_LICENSE_FILE. When unset (typical
#                                  for installs under /opt/Xilinx or
#                                  /tools/Xilinx where the license lives
#                                  inside the Xilinx tree already mounted via
#                                  SLASH_XILINX_PATH), Vivado's default
#                                  license discovery is used.
#   SLASH_PKG_SKIP_ROOT_DESIGN_BUILD  If set, forwarded into the container so
#                                     that pbuild.sh skips the (expensive)
#                                     root-design build step.
#
# Examples:
#   scripts/run-with-docker.sh package ubuntu   # build .deb packages
#   scripts/run-with-docker.sh package rocky    # build .rpm packages
#   scripts/run-with-docker.sh run     ubuntu   # interactive shell with
#                                               # the .debs preinstalled

if [ $# -ne 2 ]; then
    echo "Usage: <run|package> <ubuntu|rocky>" 2>&1
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

# Mounting the license file for synthesis and implementation, if provided.
# When unset, the license is assumed to be reachable via SLASH_XILINX_PATH
# (already mounted above) and Vivado's default license discovery.
if [ -n "$SLASH_LICENSE_PATH" ]; then
    DOCKER_RUN_ARGS+="-v $SLASH_LICENSE_PATH:$SLASH_LICENSE_PATH "
    DOCKER_RUN_ARGS+="-e XILINXD_LICENSE_FILE=$SLASH_LICENSE_PATH "
fi

# If set, add the skip-root-build flag
if [ -n $SLASH_PKG_SKIP_ROOT_DESIGN_BUILD ]; then
    DOCKER_RUN_ARGS+="-e SLASH_PKG_SKIP_ROOT_DESIGN_BUILD=$SLASH_PKG_SKIP_ROOT_DESIGN_BUILD "
fi

CONTAINER=$1
DISTRO=$2

# Check the distro argument and set the relevant packaging script
if [ $DISTRO = "ubuntu" ]; then
    PACKAGE_SCRIPT="./scripts/package-deb.sh"
elif [ $DISTRO = "rocky" ]; then
    PACKAGE_SCRIPT="./scripts/package-rpm.sh"
else
    echo "Unknown Linux distro $DISTRO" 2>&1
    exit 1
fi

# Build the script to run inside the container.
# This script will load Vivado, set the LD_LIBRARY_PATH for simulation,
# and then either run bash or the packaging script
# This block also cks the container argument.
DOCKER_COMMAND="source $SLASH_XILINX_PATH/2025.1/Vivado/settings64.sh "
DOCKER_COMMAND+="&& export LD_LIBRARY_PATH=\$LD_LIBRARY_PATH:$SLASH_XILINX_PATH/2025.1/Vivado/lib/lnx64.o "
if [ $CONTAINER = "package" ]; then
    DOCKER_COMMAND+="&& $PACKAGE_SCRIPT "
elif [ $CONTAINER = "run" ]; then
    DOCKER_COMMAND+="&& bash"
    DOCKER_RUN_ARGS+="-it "
else
    echo "Unknown container definition $CONTAINER" 2>&1
    exit 1
fi

# Build and run the container.
docker build --build-arg USER_ID=$(id -u) -t "slash-$CONTAINER-$DISTRO" -f "scripts/Dockerfile.$CONTAINER-$DISTRO" .
docker run $DOCKER_RUN_ARGS \
    "slash-$CONTAINER-$DISTRO" \
    bash -c "$DOCKER_COMMAND"
