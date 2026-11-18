#!/usr/bin/env bash
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Build a Versal PLM with SLASH's sparse-IPI dispatch fix.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build_dir=${PLM_BUILD_DIR:-"$root/build"}
bsp_dir="$build_dir/plm_bsp"
app_dir="$build_dir/plm"
patcher="$root/tools/patch_xilplmi.py"
sdt=${PLM_SDT:-}

# Reuse the AMC-generated SDT in the normal AVED flow. Direct builds may
# generate an equivalent SDT from the static-shell XSA.
if [[ -z $sdt ]]; then
    if [[ -z ${XSA:-} ]]; then
        echo "ERROR: PLM build needs PLM_SDT or XSA" >&2
        exit 1
    fi
    sdt_dir="$build_dir/versal_sdt"
    rm -rf "$sdt_dir"
    mkdir -p "$sdt_dir"
    sdtgen -eval \
        "sdtgen set_dt_param -xsa {$XSA} -dir {$sdt_dir}; generate_sdt"
    sdt="$sdt_dir/system-top.dts"
fi
if [[ ! -f $sdt ]]; then
    echo "ERROR: PLM SDT does not exist: $sdt" >&2
    exit 1
fi
if [[ -z ${XILINX_VITIS:-} ]]; then
    echo "ERROR: XILINX_VITIS must name the sourced Vitis installation" >&2
    exit 1
fi

rm -rf "$bsp_dir" "$app_dir"
mkdir -p "$build_dir"
(
    cd "$build_dir"
    empyro repo -st "$XILINX_VITIS/data/embeddedsw"
    empyro create_bsp \
        -t versal_plm \
        -w "$bsp_dir" \
        -s "$sdt" \
        -p psv_pmc_0 \
        -o standalone
)

# Empyro copies XilPLMI into the BSP before compiling its libraries. Patch that
# private copy so the installed Vitis tree and concurrent builds stay pristine.
python3 "$patcher" --bsp-root "$bsp_dir"
(
    cd "$build_dir"
    empyro build_bsp -d "$bsp_dir"
    empyro create_app \
        -d "$bsp_dir" \
        -s "$app_dir/src" \
        -t versal_plm \
        -n plm \
        --no_clangd True
    empyro build_app \
        -s "$app_dir/src" \
        -b "$bsp_dir/versal_plm/build"
)

# Empyro's template controls the final subdirectory. Resolve the one PLM ELF
# without baking that generated layout into SLASH.
python3 - "$build_dir" "$build_dir/plm.elf" <<'PY'
from pathlib import Path
import shutil
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
candidates = [
    path for path in root.rglob("plm.elf")
    if path != output
]
if len(candidates) != 1:
    rendered = ", ".join(str(path) for path in candidates) or "none"
    raise SystemExit(
        f"expected one Empyro PLM ELF, found {len(candidates)}: {rendered}")
shutil.copy2(candidates[0], output)
print(f"staged {output} from {candidates[0]}")
PY
