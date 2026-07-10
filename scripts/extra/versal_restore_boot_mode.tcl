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
# Companion to submodules/AVED/hw/*/scripts/versal_change_boot_mode.tcl.
#
# That script sets CRP.BOOT_MODE_USER (0xF1260200) bit 8 (BOOT_MODE_OVERRIDE)
# with mode field 0000 (JTAG), so PMC ROM ignores the physical boot-mode
# strap pins and waits for a JTAG-supplied PDI on the next reset.
#
# This script clears the override bit only, restoring the board to its
# pin-strapped boot mode (flash/OSPI in the normal SLASH deployment) without
# requiring a full power-on reset. Run this after XSDB "device program" so
# that later PCIe hotplug/SBR resets (e.g. from vrtd's AMI-based reset path)
# don't leave PMC ROM waiting indefinitely for a JTAG image with no debugger
# attached.
tar -set -filter {name =~ "Versal *"}

# Clear BOOT_MODE_OVERRIDE (bit 8); the mode field is irrelevant once the
# override is disabled.
mwr 0xf1260200 0x00000000

mrd 0xf1260200

# Perform reset so PMC ROM re-reads the boot-mode strap pins.
tar -set -filter {name =~ "PMC"}

rst
