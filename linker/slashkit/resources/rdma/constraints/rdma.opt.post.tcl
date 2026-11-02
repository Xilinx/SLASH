# SPDX-License-Identifier: MIT
# opt_design post hook of a service layer with the RDMA service: placement and
# timing constraints of its DCMAC port, learnt on the V80 with the same
# datapaths (slash-roce). Sourced after SLASH's own service-layer hook, with
# ::slash_rdma_eth set to the port (0 or 2).

set rdma_eth $::slash_rdma_eth
set rdma_dc [expr {$rdma_eth == 0 ? 0 : 1}]
set rdma_q [expr {$rdma_eth == 0 ? "qsfp_0_n_1" : "qsfp_2_n_3"}]
set rdma_sl top_i/service_layer

# GT quad of the port's cage. Left unplaced, the placer put QSFP2's quad on
# GTM_QUAD_X0Y9, which is QSFP3's, and the port had no link.
set c [get_cells -quiet $rdma_sl/$rdma_q/gt_wrapper/gtwiz_versal_0/inst/intf_quad_map_inst/quad_top_inst/gt_quad_base_0_inst/inst/quad_inst]
if {[llength $c] != 1} {
    error "rdma.opt.post: GT quad of $rdma_q not found"
}
set_property LOC [expr {$rdma_eth == 0 ? "GTM_QUAD_X1Y7" : "GTM_QUAD_X0Y10"}] $c

# The static shell may define no clock on the QSFP reference clock input; the
# DCMAC clock wizard's outputs (DCMAC core / AXIS clocks, the stack's
# user_clk) and everything they clock would be unconstrained. 322.265625 MHz.
set p [get_ports -quiet qsfp${rdma_eth}_322mhz_clk_p]
if {[llength $p] && [llength [get_clocks -quiet -of_objects $p]] == 0} {
    create_clock -period 3.103 -name qsfp${rdma_eth}_322mhz_refclk $p
    puts "rdma.opt.post: create_clock 322.265625 MHz on qsfp${rdma_eth}_322mhz_clk_p"
}

# DCMAC core resets are asynchronous inputs; at the 782 MHz core clock the
# recovery check from the reset synchronizers of the DCMAC clock/reset
# controller cannot be met by any placement.
set rst_ff [get_cells -quiet -hier -filter \
    "NAME =~ $rdma_sl/$rdma_q/*/dcmac_reset_ctrl/* && NAME =~ *arststages_ff_reg*"]
set dcmacs [get_cells -quiet -hier -filter "NAME =~ $rdma_sl/$rdma_q/* && REF_NAME == DCMAC"]
if {[llength $rst_ff] && [llength $dcmacs]} {
    set_false_path -from $rst_ff -to $dcmacs
}

# dcmac_tx_cdc: its LUT RAM is written in user_clk and read into registers in
# the DCMAC clock; a half is used only three DCMAC cycles after its write
# pointer crossed, so bound the path by one DCMAC clock period.
set tx_mem [get_cells -quiet -hier -filter \
    "NAME =~ $rdma_sl/$rdma_q/*tx_path/inst/inst_cdc/g_slice*mem_reg* && IS_PRIMITIVE"]
set tx_q [get_cells -quiet -hier -filter \
    "NAME =~ $rdma_sl/$rdma_q/*tx_path/inst/inst_cdc/g_slice*q_reg* && IS_PRIMITIVE"]
if {[llength $tx_mem] && [llength $tx_q]} {
    set_max_delay -datapath_only -from $tx_mem -to $tx_q 2.5
} else {
    puts "rdma.opt.post: WARNING: TX CDC cells not found"
}

# DCMAC1 sits in SLR1, its clock wizard in SLR0. Left alone the placer puts
# the 1024-bit RX FIFO and the rest of the port's fabric in SLR0 and the 391
# MHz crossing misses by 2.3 ns: keep the DCMAC-side fabric in the SLR1 part
# of the service pblock around DCMAC1, and the TX output stage (two LUTs from
# the hard block at 391 MHz) in DCMAC1's own clock region.
proc rdma_sites_in_pblock {crs parent} {
    set ranges {}
    foreach r [get_property GRID_RANGES $parent] {
        if {[regexp {^(\w+)_X(\d+)Y(\d+):\w+_X(\d+)Y(\d+)$} $r -> t x0 y0 x1 y1]} {
            lappend ranges [list $t $x0 $y0 $x1 $y1]
        }
    }
    set out {}
    foreach s [get_sites -of_objects [get_clock_regions $crs] \
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

if {$rdma_dc == 1} {
    set parent [get_pblocks -quiet -of_objects [get_cells $rdma_sl]]
    set dc1_cells [get_cells -quiet [list \
        $rdma_sl/$rdma_q/dcmac_wrapper/seg_to_axis_0 \
        $rdma_sl/$rdma_q/dcmac_wrapper/axis_to_seg_0 \
        $rdma_sl/$rdma_q/datapath_rx_1 \
        $rdma_sl/$rdma_q/datapath_tx_1]]
    if {[llength $parent] != 1 || [llength $dc1_cells] != 4} {
        puts "rdma.opt.post: WARNING: no DCMAC1 pblock (service pblock '$parent', cells $dc1_cells)"
    } else {
        set pb [create_pblock pblock_rdma_dcmac1]
        set_property PARENT $parent $pb
        resize_pblock $pb -add [rdma_sites_in_pblock \
            {X0Y6 X0Y7 X1Y5 X1Y6 X2Y5 X2Y6 X3Y5 X3Y6} $parent]
        add_cells_to_pblock $pb $dc1_cells
        set tx_cells [get_cells -quiet [list \
            $rdma_sl/$rdma_q/datapath_tx_1/tx_path/inst/inst_skid \
            $rdma_sl/$rdma_q/dcmac_wrapper/axis_to_seg_0]]
        if {[llength $tx_cells] == 2} {
            set pb_tx [create_pblock pblock_rdma_dcmac1_tx]
            set_property PARENT $pb $pb_tx
            resize_pblock $pb_tx -add [rdma_sites_in_pblock {X0Y7} $pb]
            add_cells_to_pblock $pb_tx $tx_cells
        }
        puts "rdma.opt.post: pblock_rdma_dcmac1 [get_property GRID_RANGES $pb]"
    }
}
