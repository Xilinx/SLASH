# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Generate the standalone R5_1 BSP metadata consumed by
# generate_platform_config.py. This deliberately asks Vitis to resolve the IPI
# ownership from the XSA; it does not encode a physical IPI channel.

if {$argc != 2} {
    puts stderr "usage: xsct generate_r5_bsp.tcl <platform.xsa> <workspace>"
    exit 2
}

set xsa [file normalize [lindex $argv 0]]
set workspace [file normalize [lindex $argv 1]]
if {![file isfile $xsa]} {
    puts stderr "XSA does not exist: $xsa"
    exit 2
}

file mkdir $workspace
setws $workspace
platform create -name rp1_platform -hw $xsa -out $workspace
domain create -name rp1_r5_1 -os standalone -proc psv_cortexr5_1
platform generate
platform write
