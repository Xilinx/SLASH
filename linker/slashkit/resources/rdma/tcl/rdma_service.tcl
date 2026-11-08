# SPDX-License-Identifier: MIT
# The RDMA service in the service-layer block design ([network] rdma=eth_<n>),
# sourced by the generated service_layer.tcl after the other DCMAC (if any) and
# before the control SmartConnect, which takes rdma_0/s_axil and the port's
# DCMAC registers as targets.
#
#   slash_add_rdma <eth> <build> <rdma dir> [<lanes>]
#                                               (before the SmartConnect)
#   slash_rdma_assign_addresses <eth>           (after it: the control segments
#                                                are reachable only through it)
#     eth        0 (DCMAC0, qsfp_0_n_1, QSFP0) or 2 (DCMAC1, qsfp_2_n_3, QSFP2)
#     build      BUILD register value of rdma_service
#     rdma dir   the staged resources/rdma (hdl/, tcl/, iprepo/rdma_200)
#     lanes      the stream's two NoC lanes; empty: the port's own
#
# Cells: the port's DCMAC hierarchy with the rdma_dcmac.tcl datapaths (1024-bit
# at user_clk = 230 MHz), rdma_0 (rdma_service_bd), rdma_dm_0 / rdma_dm_1 (AXI
# DataMovers, service_clk), rdma_mem_0 / rdma_mem_1 (rdma_axi_join: each
# DataMover's MM2S and S2MM onto one port, wiring only), rst_rdma_0 (user_clk
# reset).
# The port's two SL2NOC ports, SL2NOC_<2n> and SL2NOC_<2n+1> (two per QSFP),
# take one DataMover each; rdma_service alternates packets between them.
# Two NoC lanes (dummy_noc_<l>, dummy_noc_m_<l>: the port's own <2n>, <2n+1>,
# or [network] rdma_lanes) carry the stream rdma.tx / rdma.rx, its 64-byte
# units alternating between them (rdma_stripe here, rdma_stream_tx /
# rdma_stream_rx in the user region).
#
# Addresses (S_AXILITE_INI, PF2 BAR2):
#   0x203_0200_0000 / 0x203_0300_0000  DCMAC0 / DCMAC1 registers (as in SLASH)
#   0x203_0400_0000                    rdma_0 (16 MiB; the stack at +0x40_0000)
# Memory (rdma_dm_*): the DDR window 0x600_0000_0000, 32 GiB, behind SL2NOC
# (VRT MemoryRangeType::DDR buffers).

# lanes: the two NoC lanes (0..7) of the stream; empty: the port's own
proc slash_add_rdma {eth build rdma_dir {lanes {}}} {
    switch -- $eth {
        0 { set dc 0; set hier qsfp_0_n_1 }
        2 { set dc 1; set hier qsfp_2_n_3 }
        default { error "slash_add_rdma: eth_$eth has no DCMAC (eth_0 or eth_2)" }
    }
    if {[llength $lanes] == 0} {
        set lanes [list [expr {2 * $eth}] [expr {2 * $eth + 1}]]
    }
    lassign $lanes l0 l1

    # ------------------------------------------------------------ sources
    set repos [get_property ip_repo_paths [current_project]]
    lappend repos [file normalize [file join $rdma_dir iprepo]]
    set_property ip_repo_paths $repos [current_project]
    update_ip_catalog
    if {[llength [get_ipdefs -quiet -all *:rdma:rdma_200:*]] == 0} {
        error "slash_add_rdma: the rdma_200 IP is not in [file join $rdma_dir iprepo]"
    }
    if {[llength [get_ips -quiet rdma_200_ip]] == 0} {
        create_ip -vlnv [lindex [get_ipdefs -all *:rdma:rdma_200:*] end] -module_name rdma_200_ip
        # QPs 0..7 retransmit WRITEs from the IP's buffer; dcmac_tx_path pads
        # TX frames, dcmac_rx_path drops bad-FCS frames
        set_property -dict {
            CONFIG.RT_QP_BITS 3 CONFIG.WR_WINDOW 32 CONFIG.RD_WINDOW 32 CONFIG.ACK_GAP_CYCLES 20
            CONFIG.TX_PAD 0 CONFIG.RX_FCS_FILTER 0
        } [get_ips rdma_200_ip]
    }
    set files {}
    foreach f {
        axis_skid.sv axil_split.sv rx_fcs_filter.sv rdma_lane_fifo.sv rdma_stripe.sv rdma_pay_rd.sv
        rdma_pay_wr.sv rdma_service.sv rdma_service_bd.v dcmac_rx_path.v dcmac_tx_path.v
        dcmac_tx_cdc.v dcmac_phy_ctl.v rdma_axi_join.v
    } {
        lappend files [file normalize [file join $rdma_dir hdl $f]]
    }
    add_files -fileset sources_1 -norecurse $files
    foreach f [get_files -of_objects [get_filesets sources_1] -filter {NAME =~ *.sv}] {
        set_property file_type SystemVerilog $f
    }
    update_compile_order -fileset sources_1

    # ------------------------------------------------------------ the DCMAC port
    if {[llength [info procs bd_dcmac_qsfp]] == 0} {
        slash_setup_dcmac
    }
    source [file join $rdma_dir tcl rdma_dcmac.tcl]
    # bd_dcmac_qsfp: parentCell, dcmac_index, dual_dcmac, dcmac_axil, board, nclk_f (user clock, MHz)
    bd_dcmac_qsfp "" $dc 0 1 v80 230
    rdma_connect_dcmac_extra $hier $dc

    foreach {name mode vlnv} [list \
        qsfp${eth}_322mhz Slave xilinx.com:interface:diff_clock_rtl:1.0 \
        qsfp${eth}_4x Master xilinx.com:interface:gt_rtl:1.0] {
        if {[llength [get_bd_intf_ports -quiet $name]] == 0} {
            create_bd_intf_port -mode $mode -vlnv $vlnv $name
        }
    }
    set_property -dict [list CONFIG.FREQ_HZ {322265625}] [get_bd_intf_ports qsfp${eth}_322mhz]
    connect_bd_intf_net [get_bd_intf_ports qsfp${eth}_322mhz] [get_bd_intf_pins $hier/qsfp_gt_clk]
    connect_bd_intf_net [get_bd_intf_ports qsfp${eth}_4x] [get_bd_intf_pins $hier/qsfp_gt0]

    # the stack's clock and reset: user_clk of the hierarchy, reset on MMCM lock
    set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_rdma_0]
    connect_bd_net [get_bd_pins ilreduced_logic_0/Res] [get_bd_pins $rst/ext_reset_in]
    connect_bd_net [get_bd_pins $hier/user_clk] [get_bd_pins $rst/slowest_sync_clk] \
        [get_bd_pins $hier/aclk]
    connect_bd_net [get_bd_pins $hier/user_locked] [get_bd_pins $rst/dcm_locked]
    connect_bd_net [get_bd_pins $rst/peripheral_aresetn] [get_bd_pins $hier/aresetn]

    # ------------------------------------------------------------ the service
    set svc [create_bd_cell -type module -reference rdma_service_bd rdma_0]
    set_property -dict [list CONFIG.BUILD $build CONFIG.ETH_INDEX $eth] $svc
    connect_bd_net [get_bd_pins $hier/user_clk] [get_bd_pins rdma_0/aclk]
    connect_bd_net [get_bd_pins $rst/peripheral_aresetn] [get_bd_pins rdma_0/aresetn]
    connect_bd_net [get_bd_ports service_clk] [get_bd_pins rdma_0/sclk]
    connect_bd_net [get_bd_pins ilreduced_logic_0/Res] [get_bd_pins rdma_0/sresetn]
    connect_bd_intf_net [get_bd_intf_pins $hier/M_AXIS_0] [get_bd_intf_pins rdma_0/s_axis_rx]
    connect_bd_intf_net [get_bd_intf_pins rdma_0/m_axis_tx] [get_bd_intf_pins $hier/S_AXIS_0]
    connect_bd_net [get_bd_pins $hier/rx_drop_count] [get_bd_pins rdma_0/rx_drop_count]
    connect_bd_net [get_bd_pins rdma_0/phy_ctrl] [get_bd_pins $hier/phy_ctrl]
    connect_bd_net [get_bd_pins $hier/phy_status] [get_bd_pins rdma_0/phy_status]

    # ------------------------------------------------------------ memory
    foreach i {0 1} {
        set p [expr {2 * $eth + $i}]
        set dm [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_datamover:5.1 rdma_dm_$i]
        set_property -dict [list \
            CONFIG.c_include_mm2s {Full} CONFIG.c_include_s2mm {Full} \
            CONFIG.c_m_axi_mm2s_data_width {512} CONFIG.c_m_axis_mm2s_tdata_width {512} \
            CONFIG.c_m_axi_s2mm_data_width {512} CONFIG.c_s_axis_s2mm_tdata_width {512} \
            CONFIG.c_include_mm2s_dre {true} CONFIG.c_include_s2mm_dre {true} \
            CONFIG.c_mm2s_btt_used {23} CONFIG.c_s2mm_btt_used {23} \
            CONFIG.c_addr_width {64} \
            CONFIG.c_mm2s_burst_size {64} CONFIG.c_s2mm_burst_size {64} \
            CONFIG.c_mm2s_include_sf {true} CONFIG.c_s2mm_include_sf {true} \
            CONFIG.c_s2mm_support_indet_btt {false} \
            CONFIG.c_enable_mm2s_adv_sig {1} CONFIG.c_enable_s2mm_adv_sig {1} \
        ] $dm
        # progress pulses for rdma_0's diagnosis counters
        foreach {from to} [list mm2s_addr_req_posted mm2s${i}_addr_posted mm2s_rd_xfer_cmplt mm2s${i}_rd_done \
                               s2mm_addr_req_posted s2mm${i}_addr_posted s2mm_wr_xfer_cmplt s2mm${i}_wr_done] {
            connect_bd_net [get_bd_pins rdma_dm_$i/$from] [get_bd_pins rdma_0/$to]
        }
        foreach pin {m_axi_mm2s_aclk m_axis_mm2s_cmdsts_aclk m_axi_s2mm_aclk m_axis_s2mm_cmdsts_awclk} {
            connect_bd_net [get_bd_ports service_clk] [get_bd_pins rdma_dm_$i/$pin]
        }
        foreach pin {m_axi_mm2s_aresetn m_axis_mm2s_cmdsts_aresetn m_axi_s2mm_aresetn m_axis_s2mm_cmdsts_aresetn} {
            connect_bd_net [get_bd_pins ilreduced_logic_0/Res] [get_bd_pins rdma_dm_$i/$pin]
        }
        # its reads and writes onto the one port (disjoint AXI channels: a
        # SmartConnect here stalled the writes on hardware)
        create_bd_cell -type module -reference rdma_axi_join rdma_mem_$i
        connect_bd_net [get_bd_ports service_clk] [get_bd_pins rdma_mem_$i/aclk]
        foreach {from to} [list \
            rdma_0/m_axis_mm2s${i}_cmd rdma_dm_$i/S_AXIS_MM2S_CMD \
            rdma_dm_$i/M_AXIS_MM2S     rdma_0/s_axis_mm2s$i \
            rdma_dm_$i/M_AXIS_MM2S_STS rdma_0/s_axis_mm2s${i}_sts \
            rdma_0/m_axis_s2mm${i}_cmd rdma_dm_$i/S_AXIS_S2MM_CMD \
            rdma_0/m_axis_s2mm$i       rdma_dm_$i/S_AXIS_S2MM \
            rdma_dm_$i/M_AXIS_S2MM_STS rdma_0/s_axis_s2mm${i}_sts \
            rdma_dm_$i/M_AXI_MM2S      rdma_mem_$i/s_axi_rd \
            rdma_dm_$i/M_AXI_S2MM      rdma_mem_$i/s_axi_wr \
            rdma_mem_$i/m_axi          sl2noc_$p/S00_AXI] {
            connect_bd_intf_net [get_bd_intf_pins $from] [get_bd_intf_pins $to]
        }
    }

    # ------------------------------------------------------------ lanes
    # The template ties some of the lanes' tready pins high; these lanes are ours.
    foreach i $lanes {
        set rdy [get_bd_pins dummy_noc_$i/M00_AXIS_tready]
        set net [get_bd_nets -quiet -of_objects $rdy]
        if {$net ne ""} {
            disconnect_bd_net $net $rdy
        }
    }
    connect_bd_intf_net [get_bd_intf_pins dummy_noc_$l0/M00_AXIS] [get_bd_intf_pins rdma_0/s_axis_tx0]
    connect_bd_intf_net [get_bd_intf_pins dummy_noc_$l1/M00_AXIS] [get_bd_intf_pins rdma_0/s_axis_tx1]
    connect_bd_intf_net [get_bd_intf_pins rdma_0/m_axis_rx0] [get_bd_intf_pins dummy_noc_m_$l0/S00_AXIS]
    connect_bd_intf_net [get_bd_intf_pins rdma_0/m_axis_rx1] [get_bd_intf_pins dummy_noc_m_$l1/S00_AXIS]

    # ------------------------------------------------------------ memory addresses
    # the SL2NOC ports' aperture: the DDR window of the static shell, seen
    # by each join; the DataMovers see the join's ports at the same place
    # (it passes addresses through)
    foreach i {0 1} {
        set port SL2NOC_[expr {2 * $eth + $i}]
        assign_bd_address -offset 0x060000000000 -range 0x000800000000 \
            -target_address_space [get_bd_addr_spaces rdma_mem_$i/m_axi] \
            [get_bd_addr_segs $port/Reg] -force
        foreach {sp intf} {Data_MM2S s_axi_rd Data_S2MM s_axi_wr} {
            set space [get_bd_addr_spaces rdma_dm_$i/$sp]
            assign_bd_address -offset 0x060000000000 -range 0x000800000000 \
                -target_address_space $space [get_bd_addr_segs rdma_mem_$i/$intf/reg0] -force
            # a module port has register usage, the DataMover's space memory
            # usage: BD excludes the segment unless told otherwise
            set ex [get_bd_addr_segs -quiet -excluded -of_objects $space]
            if {[llength $ex]} {
                include_bd_addr_seg $ex
            }
        }
    }
}

# The control segments of the RDMA port, in S_AXILITE_INI. Called after the
# SmartConnect connects rdma_0 and the DCMAC to S_AXILITE_INI: before, the
# segments are not reachable from it and the final assign_bd_address places
# them anywhere.
proc slash_rdma_assign_addresses {eth} {
    set dc [expr {$eth == 0 ? 0 : 1}]
    set hier [expr {$eth == 0 ? "qsfp_0_n_1" : "qsfp_2_n_3"}]
    set as [get_bd_addr_spaces S_AXILITE_INI]
    foreach {seg offset range} [list \
        $hier/dcmac_wrapper/dcmac_$dc/s_axi/Reg [expr {$dc ? 0x020303000000 : 0x020302000000}] 0x00040000 \
        rdma_0/s_axil/reg0 0x020304000000 0x01000000] {
        set s [get_bd_addr_segs $seg]
        if {[llength $s] != 1} {
            error "slash_rdma_assign_addresses: segment $seg not found"
        }
        assign_bd_address -offset $offset -range $range -target_address_space $as $s -force
    }
}
