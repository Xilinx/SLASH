#!/usr/bin/env bash
# Copyright (c) 2024 - 2025 Advanced Micro Devices, Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
############################################################
set -Eeuo pipefail

# Init
DESIGN="amd_v80_gen5x8_25.1"
HW_DIR=$(realpath ./)
FW_DIR=$(realpath ./../../fw/AMC)
XSA=${XSA:-$(realpath ${HW_DIR})/build/${DESIGN}.xsa}

# Step FW
# Suppress zero-length-bounds (submodule MCTP code uses zero-length arrays as
# a flexible-array-member idiom inside a union).
#
# Spelled -Wno-... rather than -Wno-error=..., because CFLAGS seeds
# CMAKE_C_FLAGS at the AMC project() call, which happens before the cross
# compiler is selected and so tests the *host* compiler. GCC gained
# -Wzero-length-bounds in 10; older host compilers reject -Wno-error= for a
# warning they do not have, failing that check and with it the whole build,
# whereas an unknown -Wno- is silently ignored. The cross compiler, which is
# the one that actually needs the exemption, treats the two the same.
export CFLAGS="${CFLAGS:-} -Wno-zero-length-bounds"

pushd ${FW_DIR}
 ./scripts/build.sh -os freertos10_xilinx -profile v80 -xsa $XSA
  cp -a ${FW_DIR}/build/amc.elf ${HW_DIR}/build
  # Takes in fpt.json and produces fpt.bin
popd

# Step RP1 FW
RP1_DIR=$(realpath ./../../fw/RP1)
pushd ${RP1_DIR}
  XSA="$XSA" bash ./build-rp1.sh
  cp -a ${RP1_DIR}/build/rp1.elf ${HW_DIR}/build
popd

# Step FPT
pushd ${FW_DIR}/build
  ../scripts/gen_fpt.py -f ../scripts/fpt.json
  cp -a ${FW_DIR}/build/fpt.bin ${HW_DIR}/build
popd

# Step PDI combine
# Generate PDI w/ bootgen
pushd ${HW_DIR}
  bootgen -arch versal -image ${HW_DIR}/fpt/pdi_combine.bif -w -o ${HW_DIR}/build/${DESIGN}_nofpt.pdi
popd

# final pdi generation
${HW_DIR}/fpt/fpt_pdi_gen.py --fpt ${HW_DIR}/build/fpt.bin --pdi ${HW_DIR}/build/${DESIGN}_nofpt.pdi --output ${DESIGN}.pdi

