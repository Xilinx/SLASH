#!/usr/bin/env bash
# Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Build Xilinx QEMU and run RP1 firmware on a Cortex-R5F core.
#
# Uses the arm-generic-fdt machine with ZynqMP device trees from the
# Xilinx qemu-devicetrees repo. The R5F cores start halted; two register
# pokes via -device loader release them:
#   CRL_APB.RST_LPD_TOP (0xFF5E023C) - release R5 from reset
#   RPU.RPU_GLBL_CNTL    (0xFF9A0000) - configure split mode
#
# Requirements:
#   - armr5-none-eabi-gcc cross-compiler (from Vitis or ARM GNU toolchain)
#   - cmake
#   - QEMU build deps: libglib2.0-dev libgcrypt20-dev zlib1g-dev libpixman-1-dev
#                       libfdt-dev device-tree-compiler ninja-build meson python3
#
# Usage: ./scripts/build-and-test-rp1-qemu.sh [--rebuild-qemu]
set -Eeuo pipefail

REPO_ROOT=$(realpath "$(dirname "$0")/..")
QEMU_SRC="${REPO_ROOT}/submodules/xilinx-qemu"
QEMU_BUILD="${QEMU_SRC}/build"
QEMU_BIN="${QEMU_BUILD}/qemu-system-aarch64"
DT_SRC="${REPO_ROOT}/submodules/qemu-devicetrees"
DT_FILE="${DT_SRC}/LATEST/SINGLE_ARCH/board-zynqmp-zcu102.dtb"
RP1_DIR="${REPO_ROOT}/linker/slashkit/resources/aved/rp1"
RP1_BUILD="${REPO_ROOT}/linker/build/rp1-qemu"
EXPECTED_PASS_LINE="ALL TESTS PASSED"
REBUILD_QEMU=false

# ZynqMP CPU numbering: 0-3 = A53 APU, 4 = R5-0, 5 = R5-1.
# RP1 targets R5 core 1 => cpu-num=5.
R5_CPU_NUM=5

# Register addresses for R5 release from reset (ZynqMP).
CRL_RST_LPD_TOP=0xff5e023c
RPU_GLBL_CNTL=0xff9a0000
# 0x80008fdc: clear RESET_CPU0 and RESET_CPU1 bits to release both R5 cores.
CRL_RST_VALUE=0x80008fdc
# 0x80000218: set SLSPLIT + SLCLAMP for split mode (each R5 runs independently).
RPU_GLBL_VALUE=0x80000218

for arg in "$@"; do
    case "${arg}" in
        --rebuild-qemu) REBUILD_QEMU=true ;;
        *) echo "Unknown argument: ${arg}"; exit 1 ;;
    esac
done

# ============================================================================
# Phase 1: Build Xilinx QEMU (if needed)
# ============================================================================
build_qemu() {
    echo "=== Building Xilinx QEMU (aarch64-softmmu) ==="

    # Ensure submodule is initialised
    if [ ! -f "${QEMU_SRC}/configure" ]; then
        echo "Initialising xilinx-qemu submodule..."
        git -C "${REPO_ROOT}" submodule update --init submodules/xilinx-qemu
    fi

    # Check build dependencies
    local missing=()
    for pkg in glib-2.0 libgcrypt pixman-1; do
        if ! pkg-config --exists "${pkg}" 2>/dev/null; then
            missing+=("${pkg}")
        fi
    done
    if ! command -v dtc &>/dev/null; then
        missing+=("dtc (device-tree-compiler)")
    fi
    if ! command -v meson &>/dev/null; then
        missing+=("meson")
    fi
    if ! command -v ninja &>/dev/null; then
        missing+=("ninja")
    fi
    if [ ${#missing[@]} -gt 0 ]; then
        echo "ERROR: Missing build dependencies: ${missing[*]}"
        echo "On Ubuntu/Debian, install with:"
        echo "  sudo apt install libglib2.0-dev libgcrypt20-dev zlib1g-dev \\"
        echo "    libpixman-1-dev libfdt-dev device-tree-compiler ninja-build \\"
        echo "    meson python3 python3-venv pkg-config"
        exit 1
    fi

    mkdir -p "${QEMU_BUILD}"
    cd "${QEMU_BUILD}"

    if [ ! -f "${QEMU_BUILD}/build.ninja" ]; then
        "${QEMU_SRC}/configure" \
            --target-list="aarch64-softmmu" \
            --enable-fdt \
            --disable-kvm \
            --disable-xen \
            --enable-gcrypt
    fi

    make -j"$(nproc)"
    cd "${REPO_ROOT}"

    if [ ! -x "${QEMU_BIN}" ]; then
        echo "ERROR: QEMU build did not produce ${QEMU_BIN}"
        exit 1
    fi
    echo "QEMU built: ${QEMU_BIN}"
}

if [ "${REBUILD_QEMU}" = true ]; then
    rm -rf "${QEMU_BUILD}"
    build_qemu
elif [ ! -x "${QEMU_BIN}" ]; then
    build_qemu
else
    echo "=== Xilinx QEMU already built: ${QEMU_BIN} ==="
fi

# ============================================================================
# Phase 1b: Build ZynqMP device trees (if needed)
# ============================================================================
if [ ! -f "${DT_FILE}" ]; then
    echo "=== Building ZynqMP device trees ==="
    if [ ! -f "${DT_SRC}/Makefile" ]; then
        echo "ERROR: qemu-devicetrees not found at ${DT_SRC}"
        echo "Clone it with: git clone https://github.com/Xilinx/qemu-devicetrees.git ${DT_SRC}"
        exit 1
    fi
    make -C "${DT_SRC}"
fi

# ============================================================================
# Phase 2: Build RP1 firmware with semihosting
# ============================================================================
echo ""
echo "=== Building RP1 firmware (semihosting enabled) ==="

for cmd in arm-none-eabi-gcc cmake; do
    if ! command -v "${cmd}" &>/dev/null; then
        echo "ERROR: ${cmd} not found in PATH"
        exit 1
    fi
done

rm -rf "${RP1_BUILD}"
mkdir -p "${RP1_BUILD}"

cmake -S "${RP1_DIR}" -B "${RP1_BUILD}" \
    -DQEMU_SEMIHOSTING=ON

cmake --build "${RP1_BUILD}"

ELF="${RP1_BUILD}/rp1.elf"
if [ ! -f "${ELF}" ]; then
    echo "FAIL: rp1.elf not found at ${ELF}"
    exit 1
fi
echo "Built: ${ELF}"

# ============================================================================
# Phase 3: Run under Xilinx QEMU (arm-generic-fdt, ZynqMP, Cortex-R5F)
# ============================================================================
echo ""
echo "=== Running under Xilinx QEMU (arm-generic-fdt, ZynqMP R5F core 1) ==="

# Semihosting SYS_EXIT returns non-zero; capture output regardless.
OUTPUT=$(timeout 10 "${QEMU_BIN}" \
    -M arm-generic-fdt \
    -serial null \
    -serial null \
    -display none \
    -nographic \
    -m 4096 \
    -hw-dtb "${DT_FILE}" \
    -device loader,file="${ELF}",cpu-num=${R5_CPU_NUM} \
    -device loader,addr=${CRL_RST_LPD_TOP},data=${CRL_RST_VALUE},data-len=4 \
    -device loader,addr=${RPU_GLBL_CNTL},data=${RPU_GLBL_VALUE},data-len=4 \
    -semihosting-config enable=on,target=native \
    2>&1) || {
    RC=$?
    if [ $RC -eq 124 ]; then
        echo "FAIL: QEMU timed out (firmware may not have called semi_exit)"
        exit 1
    fi
    # Non-zero exit from semihosting SYS_EXIT is normal; check output below.
}

# Strip QEMU monitor prompt and banner from output
OUTPUT=$(echo "${OUTPUT}" | sed 's/^(qemu) //' | grep -v '^QEMU .* monitor')

echo "${OUTPUT}"

# ============================================================================
# Phase 4: Verify output
# ============================================================================
echo ""
echo "=== Verifying results ==="

if echo "${OUTPUT}" | grep -qF "${EXPECTED_PASS_LINE}"; then
    echo "=== RP1 QEMU test PASSED ==="
    exit 0
else
    echo "FAIL: Expected '${EXPECTED_PASS_LINE}' in output"
    echo "=== RP1 QEMU test FAILED ==="
    exit 1
fi
