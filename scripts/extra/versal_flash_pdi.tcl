# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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
#
# Flash a V80 board with a PDI over JTAG via xsdb.
#
# The board normally boots from flash (OSPI), so a JTAG "device program" only
# takes effect if PMC ROM is first told (via CRP.BOOT_MODE_USER at
# 0xF1260200) to wait for a JTAG-supplied image instead of reading the
# physical boot-mode strap pins. This script:
#
#   1. Forces JTAG boot mode and PMC-resets so PMC ROM re-enters waiting for
#      JTAG (same registers as
#      submodules/AVED/hw/*/scripts/versal_change_boot_mode.tcl).
#   2. Programs the PDI over the JTAG debug interface.
#   3. Leaves the boot-mode override set on success, so the board continues
#      to run from the JTAG-loaded image for the rest of the branch test.
#      Stress tests that use this flow must avoid AMI-triggered resets unless
#      they also re-run this JTAG programming step afterward.
#
# Usage:
#   PDI_PATH=/path/to/design.pdi xsdb scripts/extra/versal_flash_pdi.tcl

if {![info exists ::env(PDI_PATH)]} {
    error "PDI_PATH environment variable must be set to the PDI file to program"
}
set pdi_path $::env(PDI_PATH)

if {![file exists $pdi_path]} {
    error "PDI_PATH does not exist: $pdi_path"
}

connect

# Steps 1-2 are wrapped in catch so failure can restore the boot-mode override.
# On success, the override intentionally remains set to JTAG: branch-test wants
# the board to keep running from the JTAG-loaded image. On failure, restoring is
# safer because a half-programmed board left in JTAG boot mode would strand PMC
# ROM waiting for a debugger on later resets.
set flash_error [catch {
    # 1. Force JTAG boot mode.
    targets -set -filter {name =~ "Versal *"}
    mwr 0xf1260200 0x0100
    mwr -force 0xF1110004 0x0

    targets -set -filter {name =~ "PMC"}
    rst

    # 2. Program the PDI over JTAG.
    targets -set -filter {name =~ "Versal *"}
    device program $pdi_path
} flash_error_message]

if {$flash_error} {
    # Restore the board's normal (pin-strapped) boot mode on failure.
    targets -set -filter {name =~ "Versal *"}
    mwr 0xf1260200 0x00000000

    targets -set -filter {name =~ "PMC"}
    rst
}

disconnect

if {$flash_error} {
    error "versal_flash_pdi: programming failed; boot mode override was restored: $flash_error_message"
}
