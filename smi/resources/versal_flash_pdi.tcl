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
#   3. Restores the normal pin-strapped boot mode without resetting again. The
#      board keeps running from the JTAG-loaded image, while the next reset
#      boots from flash instead of waiting for another JTAG-supplied image.
#
# PDI_PATH must point to a JTAG-bootable boot PDI (boot header at offset 0),
# not a flash image. In particular the SLASH static-shell PDI
# (amd_v80_gen5x8_25.1.pdi) has a 32 KiB FPT prepended for OSPI boot-search and
# must have that header stripped before use here, or the PMC ROM rejects it
# with "ROM failed to handle config data" (ROM State 0xA).
#
# Usage:
#   PDI_PATH=/path/to/boot.pdi xsdb versal_flash_pdi.tcl
#
# Optional:
#   V80_TARGET_ID=<xsdb-target-id> PDI_PATH=/path/to/boot.pdi xsdb versal_flash_pdi.tcl
#   V80_CABLE_SERIAL=<jtag-cable-serial-or-prefix> PDI_PATH=/path/to/boot.pdi xsdb versal_flash_pdi.tcl

if {![info exists ::env(PDI_PATH)]} {
    error "PDI_PATH environment variable must be set to the PDI file to program"
}
set pdi_path $::env(PDI_PATH)

if {![file exists $pdi_path]} {
    error "PDI_PATH does not exist: $pdi_path"
}

proc describe_targets {targets} {
    set descriptions {}
    foreach target $targets {
        set fields {}
        foreach field {target_id target_ctx jtag_cable_name jtag_cable_serial} {
            if {[dict exists $target $field]} {
                lappend fields "$field=[dict get $target $field]"
            }
        }
        lappend descriptions [join $fields ", "]
    }
    return [join $descriptions "; "]
}

# Return true if $target is a Versal xcv80 that matches the caller's selection:
# V80_CABLE_SERIAL (exact jtag_cable_serial or its prefix, e.g. the board serial
# without the FTDI channel suffix), V80_TARGET_ID, or any Versal when neither is set.
proc versal_target_selected {target} {
    if {![dict exists $target name] || [dict get $target name] ne "Versal xcv80"} {
        return 0
    }
    if {![dict exists $target target_id]} {
        return 0
    }
    if {[info exists ::env(V80_CABLE_SERIAL)]} {
        if {![dict exists $target jtag_cable_serial]} {
            return 0
        }
        set serial [dict get $target jtag_cable_serial]
        set want $::env(V80_CABLE_SERIAL)
        if {$serial ne $want && ![string match "${want}*" $serial]} {
            return 0
        }
    }
    if {[info exists ::env(V80_TARGET_ID)] && [dict get $target target_id] ne $::env(V80_TARGET_ID)} {
        return 0
    }
    return 1
}

# hw_server presents an empty (or partial) target list to a client that connects
# within a fraction of a second after the previous client disconnected (measured
# <0.6 s on a two-board host, 2026-10-21), and briefly after the server was just
# launched.  Poll until the selected Versal target is present instead of trusting
# the first answer; on timeout return the last list so the caller reports it.
proc wait_for_versal_targets {{timeout_ms 5000} {poll_ms 250}} {
    set waited 0
    while {1} {
        set target_properties [targets -target-properties]
        foreach target $target_properties {
            if {[versal_target_selected $target]} {
                return $target_properties
            }
        }
        if {$waited >= $timeout_ms} {
            return $target_properties
        }
        after $poll_ms
        incr waited $poll_ms
    }
}

proc selection_description {} {
    set parts {}
    if {[info exists ::env(V80_CABLE_SERIAL)]} {
        lappend parts "jtag_cable_serial=$::env(V80_CABLE_SERIAL)"
    }
    if {[info exists ::env(V80_TARGET_ID)]} {
        lappend parts "target_id=$::env(V80_TARGET_ID)"
    }
    return [join $parts ", "]
}

proc resolve_v80_targets {} {
    if {[info exists ::env(V80_TARGET_ID)] && ![string is integer -strict $::env(V80_TARGET_ID)]} {
        error "V80_TARGET_ID must be a numeric XSDB target_id"
    }
    if {[info exists ::env(V80_CABLE_SERIAL)] && $::env(V80_CABLE_SERIAL) eq ""} {
        error "V80_CABLE_SERIAL must not be empty"
    }

    set target_properties [wait_for_versal_targets]

    set versal_targets {}
    set all_versal_targets {}
    foreach target $target_properties {
        if {![dict exists $target name] || [dict get $target name] ne "Versal xcv80"} {
            continue
        }
        lappend all_versal_targets $target
        if {[versal_target_selected $target]} {
            lappend versal_targets $target
        }
    }

    set selection [selection_description]
    if {[llength $versal_targets] == 0} {
        if {$selection ne ""} {
            error "no Versal xcv80 target found with $selection; present: [describe_targets $all_versal_targets]"
        }
        error "no Versal xcv80 target found"
    }
    if {$selection ne "" && [llength $versal_targets] != 1} {
        error "multiple Versal xcv80 targets found with $selection: [describe_targets $versal_targets]"
    }
    if {[llength $versal_targets] != 1} {
        error "multiple Versal xcv80 targets found: [describe_targets $versal_targets]"
    }

    set versal_target [lindex $versal_targets 0]
    set versal_target_id [dict get $versal_target target_id]
    set versal_target_ctx [dict get $versal_target target_ctx]

    set pmc_targets {}
    foreach target $target_properties {
        if {![dict exists $target name] || [dict get $target name] ne "PMC"} {
            continue
        }
        if {![dict exists $target parent_ctx] || [dict get $target parent_ctx] ne $versal_target_ctx} {
            continue
        }
        lappend pmc_targets $target
    }

    if {[llength $pmc_targets] == 0} {
        error "no PMC target found under Versal xcv80 target_ctx=$versal_target_ctx"
    }
    if {[llength $pmc_targets] != 1} {
        error "multiple PMC targets found under Versal xcv80 target_ctx=$versal_target_ctx: [describe_targets $pmc_targets]"
    }

    set pmc_target [lindex $pmc_targets 0]
    set pmc_target_id [dict get $pmc_target target_id]

    return [list $versal_target_id $pmc_target_id]
}

proc select_target_id {target_id} {
    targets -set -filter "target_id == $target_id"
}

connect
set target_error [catch {
    lassign [resolve_v80_targets] versal_target_id pmc_target_id
} target_error_message]
if {$target_error} {
    disconnect
    error "versal_flash_pdi: target discovery failed: $target_error_message"
}

# Steps 1-3 are wrapped in catch so failure can restore the boot-mode override.
# On success, clear the override without resetting again: users of this flow
# want the board to keep running from the JTAG-loaded image, but later AMI/SBR
# resets must boot from flash rather than waiting for another JTAG image.
set flash_error [catch {
    # 1. Force JTAG boot mode.
    select_target_id $versal_target_id
    mwr 0xf1260200 0x0100
    mwr -force 0xF1110004 0x0

    select_target_id $pmc_target_id
    rst

    # 2. Program the PDI over JTAG.
    select_target_id $pmc_target_id
    device program $pdi_path

    # 3. Restore normal boot-mode strap sampling for later resets.
    select_target_id $versal_target_id
    mwr 0xf1260200 0x00000000
} flash_error_message]

if {$flash_error} {
    # Restore the board's normal (pin-strapped) boot mode on failure.
    select_target_id $versal_target_id
    mwr 0xf1260200 0x00000000

    select_target_id $pmc_target_id
    rst
}

disconnect

if {$flash_error} {
    error "versal_flash_pdi: programming failed; boot mode override was restored: $flash_error_message"
}
