# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################
#
# 400 MHz HBM boundary pipelining.
#
# Every per-channel SmartConnect exit (hbm_sc_NN/M00_AXI) drives an HBM_AXI_NN
# port of the reconfigurable partition, and on the other side of that port sits a
# hardened NoC master unit in a different SLR. The wire between them is fixed: the
# NMU cannot move, so no amount of placement, clock-root or directive tuning
# shortens it. Measured on the service shell it is ~1.53 ns of a 2.5 ns budget,
# and it is what holds the 400 MHz datapath at WNS -0.147 (~378 MHz).
#
# A wire to a fixed endpoint cannot be shortened, so it is pipelined instead: an
# AXI register slice at the boundary splits it into two shorter hops. Only the
# address channels are registered. AW and AR carry the paths that actually fail
# (both the forward payload into the NMU and the ready returning from it); W, R
# and B are left in bypass so no latency is added to the data channels.
#
# The register slice needs a reset, and which reset it gets decides whether the
# design is CDC-clean. The partition's global reset (ilreduced_logic_0/Res, via
# util_ds_buf_0) lives in the kernel clock domain, so wiring it straight to a
# slice clocked at 400 MHz creates 64 reset-pin crossings. Those show up in
# report_cdc as "unknown" and, worse, are timed as real setup paths, which is
# what makes a naively inserted slice far worse than no slice at all. The fix is
# to give the slices a reset that is already in their own domain: one
# proc_sys_reset clocked by the HBM clock, synchronising the partition reset
# once, feeding all the slices. Then there is no crossing left to classify.
#
# Cost: one extra AW/AR handshake cycle per channel, which AXI absorbs.

# Pipeline the SmartConnect -> HBM port boundary with AXI register slices.
#
# clk_port_name  BD clock port carrying the HBM/NoC clock (the slice aclk).
# ext_reset_pin  Active-low partition reset to synchronise into that clock
#                domain; a block-design pin or port.
# psr_name       Name for the generated proc_sys_reset instance.
#
# Operates on the block design that is currently open, and is a no-op when no
# SmartConnect drives an HBM port (a design that touches no HBM channel, or one
# that has already been through this proc). Returns the number of slices added.
proc slash_add_hbm_boundary_slices {{clk_port_name static_region_clk} \
                                    {ext_reset_pin ilreduced_logic_0/Res} \
                                    {psr_name psr_hbm}} {

    set clk_port [get_bd_ports -quiet $clk_port_name]
    if {[llength $clk_port] != 1} {
        error "slash_add_hbm_boundary_slices: no block-design clock port '$clk_port_name'"
    }
    set ext_reset [get_bd_pins -quiet $ext_reset_pin]
    if {[llength $ext_reset] != 1} {
        set ext_reset [get_bd_ports -quiet $ext_reset_pin]
    }
    if {[llength $ext_reset] != 1} {
        error "slash_add_hbm_boundary_slices: no block-design reset pin or port\
               '$ext_reset_pin'"
    }

    # Collect the channels to pipeline before touching anything, so the netlist
    # is not half-modified if something below turns out not to match.
    set work [list]
    foreach port [lsort [get_bd_intf_ports -quiet HBM_AXI_*]] {
        set port_name [string trimleft $port "/"]
        set net [get_bd_intf_nets -quiet -of_objects $port]
        if {[llength $net] != 1} {
            continue
        }
        set src [get_bd_intf_pins -quiet -of_objects $net]
        if {[llength $src] != 1} {
            continue
        }
        # Only SmartConnect roots are pipelined. Unused channels are driven by
        # terminator slices through an M_AXI pin; they carry no traffic, so
        # pipelining them would cost area for nothing.
        if {![string match {*/M00_AXI} $src]} {
            continue
        }
        if {![regexp {HBM_AXI_(\w+)$} $port_name -> idx]} {
            continue
        }
        lappend work [list $port_name [get_property NAME $net] \
                           [string trimleft $src "/"] $idx]
    }
    if {[llength $work] == 0} {
        puts "INFO: \[slash\] no HBM SmartConnect exits found; boundary slices not inserted"
        return 0
    }

    # One reset synchroniser in the HBM clock domain, shared by every slice.
    if {[llength [get_bd_cells -quiet $psr_name]] == 0} {
        create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 $psr_name
        connect_bd_net [get_bd_pins $psr_name/slowest_sync_clk] $clk_port
        # ext_reset_in and aux_reset_in are active low, matching the partition
        # reset, so it connects straight through with nothing in between.
        #
        # Do not try to read CONFIG.C_EXT_RESET_HIGH here to check that. IP
        # integrator reports it as 1 while the design is being built and only
        # re-resolves it to 0 when the IP is generated, so a check at this point
        # reads a value that is not the one the hardware ends up with. An
        # earlier version of this proc trusted that reading, inverted the reset
        # to suit it, and produced slices that were held in reset for the whole
        # of normal operation. The polarity is asserted after generation
        # instead; see slash_check_hbm_boundary_reset below.
        connect_bd_net [get_bd_pins $psr_name/ext_reset_in] $ext_reset

        # Tie off the inputs this design does not use, rather than leaving them
        # dangling for validate_bd_design to complain about. aux_reset_in is
        # active low so it is tied high to stay inactive; mb_debug_sys_rst is
        # active high and tied low; dcm_locked is asserted high.
        set tie_hi ${psr_name}_tie_hi
        set tie_lo ${psr_name}_tie_lo
        create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 $tie_hi
        set_property -dict [list CONFIG.CONST_WIDTH {1} CONFIG.CONST_VAL {1}] \
            [get_bd_cells $tie_hi]
        create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 $tie_lo
        set_property -dict [list CONFIG.CONST_WIDTH {1} CONFIG.CONST_VAL {0}] \
            [get_bd_cells $tie_lo]
        connect_bd_net [get_bd_pins $tie_hi/dout] [get_bd_pins $psr_name/dcm_locked]
        connect_bd_net [get_bd_pins $tie_hi/dout] [get_bd_pins $psr_name/aux_reset_in]
        connect_bd_net [get_bd_pins $tie_lo/dout] [get_bd_pins $psr_name/mb_debug_sys_rst]
    }

    set count 0
    foreach entry $work {
        lassign $entry port_name net_name src_name idx
        set slice "rs_hbm_$idx"
        if {[llength [get_bd_cells -quiet $slice]]} {
            error "slash_add_hbm_boundary_slices: cell '$slice' already exists"
        }

        # Drop the direct SmartConnect -> port connection, then re-fetch the
        # source pin by name: the old net object does not survive the delete.
        delete_bd_objs [get_bd_intf_nets $net_name]

        create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 $slice
        # 1 = Full (registered in both directions), 0 = Bypass.
        #
        # Every channel is registered, not just the address ones. Registering
        # AW/AR alone does close the path into the NoC master unit, but it then
        # leaves the read-data return as the longest thing crossing the
        # partition boundary, and that becomes the new critical path: measured
        # at 400 MHz it lands at WNS -0.150 with all twenty worst paths running
        # NMU -> decoupler -> boundary -> partition, most of them only two logic
        # levels deep and 83% route. Half a pipelined boundary just moves the
        # bottleneck from one direction to the other.
        #
        # Cost is one cycle per channel. On an HBM access measured in tens of
        # nanoseconds, 2.5 ns each way does not matter.
        # Mode 1 (Full) on every channel, uniformly.
        #
        # Mode 10 (SLR_Crossing) was tried on channels 60..63 - the four that
        # fail at 400 MHz, whose NoC master units sit at the far right of the die
        # while the partition ended at SLICE_X351. The mode is built for exactly
        # that hop, so it looked like the obvious fix. Measured, it was worse:
        # -0.880 post-place against -0.700 for plain Full. The extra stages cost
        # more in the surrounding congestion than the crossing won back.
        #
        # What those four channels actually needed was not a different slice mode
        # but room to reach: the partition's pblock stopped 34 SLICE columns short
        # of empty fabric on the side they route toward. See the pblock widening
        # in service/constraints/impl.xdc.
        set_property -dict [list \
            CONFIG.REG_AW {1} \
            CONFIG.REG_AR {1} \
            CONFIG.REG_W  {1} \
            CONFIG.REG_R  {1} \
            CONFIG.REG_B  {1} \
        ] [get_bd_cells $slice]

        connect_bd_intf_net [get_bd_intf_pins $src_name] \
                            [get_bd_intf_pins $slice/S_AXI]
        connect_bd_intf_net [get_bd_intf_pins $slice/M_AXI] \
                            [get_bd_intf_ports $port_name]
        connect_bd_net [get_bd_pins $slice/aclk]    $clk_port
        connect_bd_net [get_bd_pins $slice/aresetn] [get_bd_pins $psr_name/peripheral_aresetn]
        incr count
    }

    puts "INFO: \[slash\] pipelined $count HBM boundary channels\
          (AW/AR/W/R/B registered, reset synchronised by $psr_name on $clk_port_name)"
    return $count
}

# Check that the generated reset synchroniser really is active low.
#
# Call this only after the IP has been generated (after generate_target, or
# after the synthesis run that generates it). Before that point IP integrator
# reports C_EXT_RESET_HIGH as 1 no matter what is connected, and it is only
# resolved against the connected signal's polarity at generation time.
#
# Getting this wrong is silent and total: an active-high proc_sys_reset fed the
# active-low partition reset holds every HBM register slice in reset for the
# whole of normal operation, so the design closes timing, passes DRC, and moves
# no data. Worth failing the build over.
proc slash_check_hbm_boundary_reset {{psr_name psr_hbm}} {
    # Match on the IP definition, not just the name: the tie-off constants are
    # called ${psr_name}_tie_hi/_lo and would otherwise be caught by the glob,
    # and an xlconstant has no C_EXT_RESET_HIGH to check.
    set ips [get_ips -quiet -filter {IPDEF =~ *:proc_sys_reset:*} *${psr_name}_*]
    if {[llength $ips] == 0} {
        puts "WARNING: \[slash\] no generated '$psr_name' IP found; reset polarity unchecked"
        return
    }
    foreach ip $ips {
        set high [get_property -quiet CONFIG.C_EXT_RESET_HIGH $ip]
        if {$high ne "0"} {
            error "slash_check_hbm_boundary_reset: [get_property NAME $ip] generated\
                   with C_EXT_RESET_HIGH=$high. The HBM boundary slices are reset by\
                   the active-low partition reset, so this must be 0; at $high the\
                   slices would be held in reset during normal operation."
        }
        puts "INFO: \[slash\] [get_property NAME $ip]: C_EXT_RESET_HIGH=0 (active low), correct"
    }
}
