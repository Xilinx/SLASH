#!/bin/bash
# SPDX-License-Identifier: MIT

# Build the RDMA stack IP rdma_200 from the fpga-network-stack submodule and
# stage it into the slashkit package tree, where the linker finds it for a
# design with [network] rdma=eth_<n> (see emit/hw/service_region/rdma_ctx.py).
#
# Needs Vitis HLS and Vivado on PATH (the version SLASH uses), CMake and make;
# the HLS build takes about ten minutes and is skipped when nothing under the
# submodule's hls/ changed. The destination is gitignored.
#
#   FPGA_PART   part the HLS IPs are built for (default: the V80's)
#   RDMA_BUILD  build directory (default: submodules/fpga-network-stack/build)

set -euo pipefail

# SLASH root
cd "$(dirname "$0")/.."

src=submodules/fpga-network-stack
build=${RDMA_BUILD:-$src/build}
part=${FPGA_PART:-xcv80-lsva4737-2MHP-e-S}
dst=linker/slashkit/resources/rdma/iprepo

if [[ ! -f "$src/CMakeLists.txt" ]]; then
    echo "error: $src is not checked out. Run:" >&2
    echo "    git submodule update --init submodules/fpga-network-stack" >&2
    exit 1
fi

cmake -S "$src" -B "$build" -DFPGA_PART="$part"
make -C "$build" rdma_200

rm -rf "$dst"
mkdir -p "$dst"
cp -a "$build/iprepo/rdma_200" "$dst/"

echo "Staged $build/iprepo/rdma_200 -> $dst"
