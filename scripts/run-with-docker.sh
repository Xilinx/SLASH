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

# Usage: scripts/run-with-docker.sh <run|package> <ubuntu|rocky> [version]
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
#             dependencies installed. The built packages are written to a
#             per-distro+version directory under the working tree:
#               docker-build/ubuntu-22.04/*.deb
#               docker-build/ubuntu-24.04/*.deb
#               docker-build/ubuntu-26.04/*.deb
#               docker-build/rocky-9/*.rpm
#               docker-build/rocky-10/*.rpm
#   run       Drop into an interactive bash shell inside a container that has
#             the freshly built SLASH packages already installed.
#
# Distro version (optional third argument):
#   Selects the base image the container is built from. When omitted it
#   defaults to the oldest supported release for the chosen distro.
#     ubuntu   22.04 (default), 24.04, 26.04  -> ubuntu:<version>
#     rocky    9 (default), 10                -> rockylinux:9 /
#                                                rockylinux/rockylinux:10
#   Each distro+version pair is built and tagged independently as
#   slash-<run|package>-<distro>:<version>, so different versions do not
#   clobber each other's images.
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
#   scripts/run-with-docker.sh package ubuntu         # .deb on ubuntu 22.04
#   scripts/run-with-docker.sh package ubuntu 24.04   # .deb on ubuntu 24.04
#   scripts/run-with-docker.sh package rocky          # .rpm on rocky 9
#   scripts/run-with-docker.sh package rocky 10       # .rpm on rocky 10
#   scripts/run-with-docker.sh run     ubuntu         # interactive shell with
#                                                     # the .debs preinstalled

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
    echo "Usage: <run|package> <ubuntu|rocky> [version]" >&2
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

# Mount the git directory so version-stamping scripts that shell out to git
# (e.g. AVED's getVersion.sh, which stamps GIT_HASH into the AMI package
# release) work inside the container. For a normal checkout .git lives under
# $PWD and is already mounted; for a git worktree .git is a file pointing at
# the main repo's git dir, which is outside $PWD and must be mounted at the
# same path. Without it git fails and the empty hash yields an illegal
# "0..<date>" rpm release.
if GIT_COMMON_DIR="$(git rev-parse --git-common-dir 2>/dev/null)"; then
    GIT_COMMON_DIR="$(cd "$GIT_COMMON_DIR" && pwd)"
    case "$GIT_COMMON_DIR" in
        "$PWD"/*) : ;; # already covered by the $PWD mount
        *) DOCKER_RUN_ARGS+="-v $GIT_COMMON_DIR:$GIT_COMMON_DIR " ;;
    esac
fi

CONTAINER=$1
DISTRO=$2
VERSION=${3:-}

# Check the distro argument and select the packaging script, the default
# version, and the base image for the requested (distro, version) pair.
# Rocky images are pulled from the team-maintained rockylinux/rockylinux repo
# rather than the "rockylinux" Docker Official Image: the latter is unmaintained
# and badly out of date (stuck near 9.3), and never published a 10 tag at all.
if [ "$DISTRO" = "ubuntu" ]; then
    PACKAGE_SCRIPT="./scripts/package-deb.sh"
    VERSION=${VERSION:-22.04}
    case "$VERSION" in
        22.04|24.04|26.04) BASE_IMAGE="ubuntu:$VERSION" ;;
        *)
            echo "Unsupported ubuntu version '$VERSION' (supported: 22.04, 24.04, 26.04)" >&2
            exit 1
            ;;
    esac
elif [ "$DISTRO" = "rocky" ]; then
    PACKAGE_SCRIPT="./scripts/package-rpm.sh"
    VERSION=${VERSION:-9}
    case "$VERSION" in
        9|10) BASE_IMAGE="rockylinux/rockylinux:$VERSION" ;;
        *)
            echo "Unsupported rocky version '$VERSION' (supported: 9, 10)" >&2
            exit 1
            ;;
    esac
else
    echo "Unknown Linux distro $DISTRO" >&2
    exit 1
fi

# Build the script to run inside the container.
# This script will load Vivado, set the LD_LIBRARY_PATH for simulation,
# and then either run bash or the packaging script
# This block also cks the container argument.
DOCKER_COMMAND="source $SLASH_XILINX_PATH/2025.1/Vivado/settings64.sh "
DOCKER_COMMAND+="&& export LD_LIBRARY_PATH=\$LD_LIBRARY_PATH:$SLASH_XILINX_PATH/2025.1/Vivado/lib/lnx64.o "
if [ $CONTAINER = "package" ]; then
    # Route the built packages into a per-distro+version subdirectory so the
    # outputs of different builds do not clobber each other. Both packaging
    # scripts honour ARTIFACTS_DIR. The path lives under $PWD, which is mounted
    # at the same path in the container, so the artifacts are visible on the
    # host once the container exits.
    ARTIFACTS_DIR="$PWD/docker-build/$DISTRO-$VERSION"
    DOCKER_RUN_ARGS+="-e ARTIFACTS_DIR=$ARTIFACTS_DIR "
    DOCKER_COMMAND+="&& $PACKAGE_SCRIPT "
elif [ $CONTAINER = "run" ]; then
    DOCKER_COMMAND+="&& bash"
    DOCKER_RUN_ARGS+="-it "
else
    echo "Unknown container definition $CONTAINER" >&2
    exit 1
fi

# Build and run the container. The image is tagged per distro+version so that
# different versions do not clobber each other's images.
IMAGE_TAG="slash-$CONTAINER-$DISTRO:$VERSION"
docker build \
    --build-arg USER_ID=$(id -u) \
    --build-arg BASE_IMAGE="$BASE_IMAGE" \
    -t "$IMAGE_TAG" \
    -f "scripts/Dockerfile.$CONTAINER-$DISTRO" .
docker run $DOCKER_RUN_ARGS \
    "$IMAGE_TAG" \
    bash -c "$DOCKER_COMMAND"
