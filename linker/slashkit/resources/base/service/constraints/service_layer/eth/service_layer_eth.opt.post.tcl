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

set_property -quiet -dict { PACKAGE_PIN AR51 } [get_ports -quiet {qsfp0_322mhz_clk_p}]
set_property -quiet -dict { PACKAGE_PIN AL17 } [get_ports -quiet {qsfp2_322mhz_clk_p}]

# GT quads of the QSFP0 and QSFP2 cages:
#
#   DCMAC0 (DCMAC_X1Y1)  GTM_QUAD_X1Y7   QSFP0      (GTM_QUAD_X1Y8   QSFP1)
#   DCMAC1 (DCMAC_X0Y2)  GTM_QUAD_X0Y10  QSFP2      (GTM_QUAD_X0Y9   QSFP3)
#
# CONFIG.DCMAC_LOCATION_C0 only places the DCMAC hard block. Left to the
# placer, QSFP2's quad lands on GTM_QUAD_X0Y9, which is wired to the QSFP3
# cage, and QSFP2 has no link.
foreach {q loc} {qsfp_0_n_1 GTM_QUAD_X1Y7 qsfp_2_n_3 GTM_QUAD_X0Y10} {
    set c [get_cells -quiet top_i/service_layer/$q/gt_wrapper/gtwiz_versal_0/inst/intf_quad_map_inst/quad_top_inst/gt_quad_base_0_inst/inst/quad_inst]
    if {[llength $c] != 1} {
        error "service_layer_eth.opt.post: GT quad of $q not found"
    }
    set_property LOC $loc $c
    puts "service_layer_eth.opt.post: LOC $loc on $c"
}

# The static shell defines no clock on the QSFP reference clock inputs. Without
# one, the DCMAC clock wizard outputs (DCMAC core clock and the 391 MHz AXIS
# clock), the GT clocks and the ~22,500 registers of each port they clock
# (seg_to_axis, axis_to_seg, the RX/TX datapaths, the write side of the CDC
# FIFOs) have no clock in timing analysis: check_timing reports them as
# no_clock and "all user specified timing constraints are met" says nothing
# about them. 322.265625 MHz (Versal-DCMAC board_config.tcl).
foreach port {qsfp0_322mhz_clk_p qsfp2_322mhz_clk_p} {
    set p [get_ports -quiet $port]
    if {[llength $p] == 0} {
        puts "service_layer_eth.opt.post: WARNING: no port $port"
    } elseif {[llength [get_clocks -quiet -of_objects $p]] == 0} {
        create_clock -period 3.103 -name ${port}_refclk $p
        puts "service_layer_eth.opt.post: create_clock 322.265625 MHz on $port"
    }
}

# RX_CORE_RESET and TX_CORE_RESET of the DCMAC are asynchronous. The DCMAC IP's
# own XDC declares them false paths, but with the cell filter
# *<ip>_core_*_top*, which matches nothing here (the hard block sits under
# i_<ip>_top), so the exception is dropped. Vivado then times a recovery check
# of about 1.27 ns at the 782 MHz core clock from dcmac_reset_ctrl's reset
# synchroniser: with the clocks constrained (above) DCMAC0 misses it by up to
# 0.058 ns, and no placement of the synchroniser meets it. Apply the IP's
# exception to the pins it means.
set rst_pins [get_pins -quiet -of_objects [get_cells -quiet -hierarchical -filter {REF_NAME == DCMAC && NAME =~ top_i/service_layer/*}] \
    -filter {REF_PIN_NAME == RX_CORE_RESET || REF_PIN_NAME == TX_CORE_RESET}]
if {[llength $rst_pins] != 4} {
    error "service_layer_eth.opt.post: expected the RX/TX_CORE_RESET pins of 2 DCMACs, found [llength $rst_pins]"
}
set_false_path -to $rst_pins
puts "service_layer_eth.opt.post: set_false_path to [llength $rst_pins] DCMAC core reset pins"

# Both DCMACs are in SLR1, their clock wizards (the only MMCMs of the service
# pblock) in SLR0. Left alone, the placer can pull a DCMAC's AXIS fabric into
# SLR0 next to the MMCMs: the 1024-bit RX path then crosses SLR0/SLR1 through
# a few SLLs, SLL assignment fails (Route 35-3424) and the auto-pipeline
# registers it inserts have no pblock (HDPR-1). Keep each DCMAC's fabric in
# the SLR1 part of the service pblock next to its hard block. The pblocks are
# hard: create_pblock is soft in this flow, and the placer drops a soft pblock
# that overflows without a message.
proc service_layer_eth_sites_in_pblock {crs parent} {
    set ranges {}
    foreach r [get_property GRID_RANGES $parent] {
        if {[regexp {^(\w+)_X(\d+)Y(\d+):\w+_X(\d+)Y(\d+)$} $r -> t x0 y0 x1 y1]} {
            lappend ranges [list $t $x0 $y0 $x1 $y1]
        }
    }
    set out {}
    foreach s [get_sites -of_objects [get_clock_regions -quiet $crs] \
            -filter {SITE_TYPE =~ SLICE* || SITE_TYPE =~ RAMB* || SITE_TYPE =~ URAM*}] {
        if {![regexp {^(\w+)_X(\d+)Y(\d+)$} $s -> t x y]} continue
        foreach r $ranges {
            lassign $r rt x0 y0 x1 y1
            if {$rt eq $t && $x >= $x0 && $x <= $x1 && $y >= $y0 && $y <= $y1} {
                lappend out $s
                break
            }
        }
    }
    return $out
}

# service_clk reset of the AXI register slices: util_ds_buf_0 puts it on a
# BUFG_FABRIC, whose 68 loads span slice columns ~100 to ~260 (register slices
# 4/5 sit at the service layer's 512-bit NoC ports, which the static shell's
# NoC solution places at X3Y10, far east). Left to itself the placer put the
# buffer in clock region X1Y4, west of every load, and the eastern reset
# replicas missed 250 MHz by 72 ps. Keep it in the middle of its loads.
set b [get_cells -quiet {top_i/service_layer/util_ds_buf_0/U0/USE_BUFG_FABRIC.GEN_BUFG_FABRIC[0].BUFG_FABRIC_U}]
set s [lindex [get_sites -quiet -of_objects [get_pblocks pblock_service_layer] \
    -filter {SITE_TYPE =~ BUFG_FABRIC* && CLOCK_REGION == X5Y4}] 0]
if {[llength $b] != 1 || $s eq ""} {
    error "service_layer_eth.opt.post: reset BUFG_FABRIC ($b) or a site for it in X5Y4 ($s) not found"
}
set_property LOC $s $b
puts "service_layer_eth.opt.post: LOC $s on $b"

foreach {q dc pb crs} {
    qsfp_0_n_1 0 pblock_eth_dcmac0
    {X6Y5 X6Y6 X7Y5 X7Y6 X8Y5 X8Y6 X9Y5 X9Y6 X10Y5 X10Y6 X11Y5 X11Y6}
    qsfp_2_n_3 1 pblock_eth_dcmac1
    {X0Y6 X0Y7 X1Y5 X1Y6 X2Y5 X2Y6 X3Y5 X3Y6 X4Y5 X5Y5}
} {
    set h top_i/service_layer/$q
    set cells [get_cells -quiet [list $h/dcmac_wrapper/seg_to_axis_0 $h/dcmac_wrapper/axis_to_seg_0 \
        $h/datapath_rx_$dc $h/datapath_tx_$dc]]
    if {[llength $cells] != 4} {
        error "service_layer_eth.opt.post: $q: expected 4 DCMAC fabric cells, found: $cells"
    }
    set parent [get_pblocks pblock_service_layer]
    set p [create_pblock $pb]
    set_property PARENT $parent $p
    set_property IS_SOFT FALSE $p
    resize_pblock $p -add [service_layer_eth_sites_in_pblock $crs $parent]
    add_cells_to_pblock $p $cells
    puts "service_layer_eth.opt.post: $pb [get_property GRID_RANGES $p]"
}
