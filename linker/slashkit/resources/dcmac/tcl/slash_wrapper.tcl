######################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
######################################################################################

# Directory holding this file, captured at source time: either
# <build_dir>/dcmac/tcl (generated designs) or
# <slashkit>/resources/dcmac/tcl (static shell, sourced in place).
set ::slash_dcmac_tcl_dir [file dirname [file normalize [info script]]]

# Locate the Versal-DCMAC sources without depending on the caller's cwd:
#   1. ../versal next to this file - the copy staged by the linker into the
#      build directory, and the copy shipped inside a slashkit package;
#   2. <repo>/submodules/Versal-DCMAC - a source checkout with the submodule
#      initialised (only reachable when running from resources/ in place).
# Mirrors stage_versal_dcmac() in emit/hw/service_region/service_layer_ctx.py.
proc slash_resolve_versal_dcmac_root {} {
    set candidates [list \
        [file normalize [file join $::slash_dcmac_tcl_dir .. versal]] \
        [file normalize [file join $::slash_dcmac_tcl_dir .. .. .. .. .. submodules Versal-DCMAC]]]
    foreach root $candidates {
        if { [file isfile [file join $root tcl dcmac.tcl]] } {
            return $root
        }
    }
    error "Versal-DCMAC sources not found (looked in: [join $candidates {, }]).\
           From a source checkout, run:\n\
           \    git submodule update --init submodules/Versal-DCMAC"
}

proc slash_setup_dcmac { {versal_dcmac_root ""} } {
    if { $versal_dcmac_root eq "" } {
        set versal_dcmac_root [slash_resolve_versal_dcmac_root]
    }
    set hdl_dir [file join $versal_dcmac_root hdl]
    foreach f {
        axis_seg_to_unseg_converter.v
        axis_to_dcmac_seg.sv
        axis_to_dcmac_seg_wrapper.v
        clock_utils.v
        dcmac200g_ctl_port.v
        dcmac_reset_ctrl.sv
        dcmac_reset_ctrl_wrapper.v
    } {
        import_files -fileset sources_1 -norecurse [file join $hdl_dir $f]
    }
    uplevel #0 [list source [file join $versal_dcmac_root tcl dcmac.tcl]]
}

proc add_dcmac_inst {} {
    upvar #0 DCMAC0_ENABLED dc0  DCMAC1_ENABLED dc1
    upvar #0 DUAL_QSFP_DCMAC0 dq0  DUAL_QSFP_DCMAC1 dq1

    # DCMAC AXIS clock in MHz; consumed by cr_bd_dcmac for aclk FREQ_HZ.
    set NCLK_F 391

    if { $dc0 == 1 } {
        # bd_dcmac_qsfp args: parentCell, dcmac_index, dual_dcmac, dcmac_axil, board, nclk_f
        bd_dcmac_qsfp "" 0 $dq0 1 v80 $NCLK_F
        # DCMAC 0 register space (256K).
        assign_bd_address \
            -offset 0x020302000000 -range 0x00040000 \
            -target_address_space [get_bd_addr_spaces S_AXILITE_INI] \
            [get_bd_addr_segs qsfp_0_n_1/dcmac_wrapper/dcmac_0/s_axi/Reg] -force
    }
    if { $dc1 == 1 } {
        bd_dcmac_qsfp "" 1 $dq1 1 v80 $NCLK_F
        assign_bd_address \
            -offset 0x020303000000 -range 0x00040000 \
            -target_address_space [get_bd_addr_spaces S_AXILITE_INI] \
            [get_bd_addr_segs qsfp_2_n_3/dcmac_wrapper/dcmac_1/s_axi/Reg] -force
    }
}
