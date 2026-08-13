#!/usr/bin/env bash
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build_dir=${RP1_BUILD_DIR:-"$root/build"}
template="$root/config/rp1_platform_config.h.in"
generator="$root/tools/generate_platform_config.py"
platform_header=${RP1_PLATFORM_CONFIG_HEADER:-}

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
    python3 "${generator_args[@]}"
fi
cmake_args+=("-DRP1_PLATFORM_CONFIG_HEADER=$platform_header")

cmake "${cmake_args[@]}"
cmake --build "$build_dir" --target rp1.elf
