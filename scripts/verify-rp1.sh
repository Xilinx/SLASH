#!/usr/bin/env bash
# Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Verify that RP1 firmware is running by reading the shared control block.
# Usage: verify_rp1.sh [control_block_base] [bdf]
#   control_block_base: DDR base of RP1 control block (default: 0x30000000)
#   bdf:     PCIe BDF of the device (default: auto-detect)
set -euo pipefail

CTRL_BASE=${1:-0x30000000}
HEARTBEAT_ADDR=$((CTRL_BASE + 0x3C))
STATE_ADDR=$((CTRL_BASE + 0x30))

BDF_ARGS=""
if [ -n "${2:-}" ]; then
    BDF_ARGS="--device $2"
fi

echo "Checking RP1 control block at ${CTRL_BASE} (heartbeat ${HEARTBEAT_ADDR})..."

# Read the first four control words: magic, version, node_count, cq_size.
RESULT=$(v80-smi debug mem-poke ${BDF_ARGS} --region RAW --read --hex "${CTRL_BASE}" --word-size 4 --count 4 2>&1) || {
    echo "ERROR: Failed to read control block at ${CTRL_BASE}"
    echo "${RESULT}"
    exit 1
}

echo "Control block[0:4]: ${RESULT}"

STATE=$(v80-smi debug mem-poke ${BDF_ARGS} --region RAW --read --hex "${STATE_ADDR}" --word-size 4 --count 1 2>&1) || true
echo "RP1 state @ ${STATE_ADDR}: ${STATE}"

# Read heartbeat, wait, read again to check liveness.
C1=$(v80-smi debug mem-poke ${BDF_ARGS} --region RAW --read --hex "${HEARTBEAT_ADDR}" --word-size 4 --count 1 2>&1) || true
sleep 1
C2=$(v80-smi debug mem-poke ${BDF_ARGS} --region RAW --read --hex "${HEARTBEAT_ADDR}" --word-size 4 --count 1 2>&1) || true

if [ "${C1}" != "${C2}" ]; then
    echo "PASS: RP1 counter is incrementing (${C1} -> ${C2})"
else
    echo "WARN: RP1 counter unchanged (${C1}). Firmware may be stuck or not running."
fi
