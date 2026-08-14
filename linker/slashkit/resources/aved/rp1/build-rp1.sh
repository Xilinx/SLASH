#!/usr/bin/env bash
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build_dir=${RP1_BUILD_DIR:-"$root/build"}
template="$root/config/rp1_platform_config.h.in"
generator="$root/tools/generate_platform_config.py"
platform_header=${RP1_PLATFORM_CONFIG_HEADER:-}

# Run $py, optionally with $libdir prepended to LD_LIBRARY_PATH for that call
# alone. A prefix assignment does not persist, which is the point.
run_python() {
    local py=$1 libdir=$2
    shift 2
    if [[ -n $libdir ]]; then
        LD_LIBRARY_PATH="$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$py" "$@"
    else
        "$py" "$@"
    fi
}

cmake_args=(
    -S "$root"
    -B "$build_dir"
    -G Ninja
)
if [[ -n ${SLASH_LIBSLASH_INCLUDE:-} ]]; then
    cmake_args+=(
        "-DSLASH_LIBSLASH_INCLUDE=$SLASH_LIBSLASH_INCLUDE"
    )
fi

# A caller-supplied header is already an explicit contract. Otherwise derive
# one from direct BSP metadata or generate that metadata from an XSA; never
# guess a physical IPI channel because a plausible wrong channel can target
# another processor rather than fail locally.
if [[ -z $platform_header ]]; then
    platform_header="$build_dir/generated-platform/rp1_platform_config.h"
    generator_args=(
        "$generator"
        --template "$template"
        --output "$platform_header"
    )

    if [[ -n ${RP1_BSP_XPARAMETERS:-} ]]; then
        generator_args+=(--xparameters "$RP1_BSP_XPARAMETERS")
    elif [[ -n ${XSA:-} ]]; then
        bsp_dir="$build_dir/r5-bsp-metadata"
        sdt_dir="$bsp_dir/versal_sdt"
        rm -rf "$bsp_dir"
        mkdir -p "$bsp_dir"

        for tool in sdtgen empyro; do
            if ! command -v "$tool" >/dev/null 2>&1; then
                echo "ERROR: $tool is required to derive the R5_1 BSP from XSA=$XSA" >&2
                exit 1
            fi
        done
        if [[ -z ${XILINX_VITIS:-} ]]; then
            echo "ERROR: XILINX_VITIS must name the sourced Vitis installation" >&2
            exit 1
        fi

        # Match AMC's headless SDT/Empyro flow, but select the standalone
        # domain owned by R5-1 so xparameters.h describes RP1's physical IPI.
        (
            cd "$bsp_dir"
            sdtgen -eval \
                "sdtgen set_dt_param -xsa {$XSA} -dir {$sdt_dir}; generate_sdt"
            empyro repo -st "$XILINX_VITIS/data/embeddedsw"
            empyro create_bsp \
                -t empty_application \
                -w rp1_bsp \
                -s "$sdt_dir/system-top.dts" \
                -p psv_cortexr5_1 \
                -o standalone
            empyro build_bsp -d rp1_bsp
        )
        generator_args+=(--bsp-metadata "$bsp_dir")
    else
        echo "ERROR: hardware RP1 build needs XSA, RP1_BSP_XPARAMETERS, or" >&2
        echo "       RP1_PLATFORM_CONFIG_HEADER; no physical IPI is assumed." >&2
        exit 1
    fi

    if [[ -n ${RP1_SOURCE_INSTANCE:-} ]]; then
        generator_args+=(--source-instance "$RP1_SOURCE_INSTANCE")
    fi

    # Prefer the interpreter Vitis ships over whatever the host calls python3.
    # This script already depends on Vitis, so its 3.13 is the one interpreter
    # guaranteed to be present wherever the build legitimately runs -- whereas
    # the host's may be far older: RHEL 8 still ships 3.6.8, which predates the
    # `from __future__ import annotations` the generator opens with, so it dies
    # with a SyntaxError rather than anything that names the real problem.
    python=""
    python_libdir=""
    for candidate in "${XILINX_VITIS:-}"/tps/lnx64/python-*/bin/python3 \
                     "$(command -v python3 2>/dev/null)"; do
        [[ -x $candidate ]] || continue
        # Vitis's interpreter carries no RPATH to its own libpython and
        # settings64.sh does not add one, so it runs only with its sibling
        # lib/ on LD_LIBRARY_PATH. Try without first, and keep the variable
        # scoped to this one call either way -- exporting it would put Vitis's
        # libstdc++ ahead of the cross compiler's for the rest of the build.
        for libdir in "" "${candidate%/bin/*}/lib"; do
            if run_python "$candidate" "$libdir" \
                   -c 'import sys; sys.exit(sys.version_info < (3, 7))' 2>/dev/null; then
                python="$candidate"
                python_libdir="$libdir"
                break 2
            fi
        done
    done
    if [[ -z $python ]]; then
        echo "ERROR: no Python >= 3.7 found to run $(basename "$generator")." >&2
        echo "       Looked in \$XILINX_VITIS/tps/lnx64/python-*/bin and on PATH." >&2
        echo "       Source the Vitis settings script, or put a newer python3 first." >&2
        exit 1
    fi
    run_python "$python" "$python_libdir" "${generator_args[@]}"
fi
cmake_args+=("-DRP1_PLATFORM_CONFIG_HEADER=$platform_header")

cmake "${cmake_args[@]}"
cmake --build "$build_dir" --target rp1.elf
