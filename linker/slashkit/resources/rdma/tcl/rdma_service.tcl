# SPDX-License-Identifier: MIT
# The RDMA cores in the service-layer block design ([network] rdma=eth_<n>, one
# per DCMAC port), sourced by the generated service_layer.tcl after the plain
# DCMAC (if any) and before the control SmartConnect, which takes
# rdma_<n>/s_axil and the port's DCMAC registers as targets.
#
#   slash_add_rdma <eth> <build> <rdma dir> [<lanes>] [<read window>]
#                                               (before the SmartConnect, once
#                                                per core)
#   slash_rdma_assign_addresses <eth>           (after it: the control segments
#                                                are reachable only through it)
#     eth        0 (DCMAC0, qsfp_0_n_1, QSFP0) or 2 (DCMAC1, qsfp_2_n_3, QSFP2)
#     build      BUILD register value of rdma_service
#     rdma dir   the staged resources/rdma (hdl/, tcl/, iprepo/rdma_200)
#     lanes      the stream's two NoC lanes; empty: 0,1 on eth_0, 2,6 on eth_2
#     read window  READ requests in flight per QP, 1..32 (RD_WINDOW of the IP;
#                the cores share the IP, so the first call sets it for all)
#
# Cells of the core on eth_<n>: the port's DCMAC hierarchy with the
# rdma_dcmac.tcl datapaths (1024-bit at user_clk = 230 MHz), rdma_<n>
# (rdma_service_bd), rdma_dm_<2n> / rdma_dm_<2n+1> (AXI DataMovers,
# service_clk), rdma_mem_<2n> / rdma_mem_<2n+1> (rdma_axi_join: each
# DataMover's MM2S and S2MM onto one port, wiring only), rst_rdma_<n>
# (user_clk reset), rst_rdma_<n>_s (service_clk reset of the core and its
# DataMovers).
# Two SL2NOC ports, SL2NOC_1 / SL2NOC_3 for eth_0 and SL2NOC_0 / SL2NOC_2 for
# eth_2, take one DataMover each; rdma_service alternates packets between them.
# Two NoC lanes (dummy_noc_<l>, dummy_noc_m_<l>, picked by the linker:
# connectivity.RDMA_LANES) carry the stream rdma.tx / rdma.rx, its 64-byte
# units alternating between them (rdma_stripe here, rdma_stream_tx /
# rdma_stream_rx in the user region).
#
# Addresses (S_AXILITE_INI, PF2 BAR2):
#   0x203_0200_0000 / 0x203_0300_0000  DCMAC0 / DCMAC1 registers (as in SLASH)
#   0x203_0400_0000 / 0x203_0500_0000  rdma_0 / rdma_2 (16 MiB each; the stack
#                                      at +0x40_0000)
# Memory (rdma_dm_*): the DDR window 0x600_0000_0000, 32 GiB, behind SL2NOC
# (VRT MemoryRangeType::DDR buffers), shared by the cores.
#
# The static shell fixes the NoC site of every SL2NOC port and lane, so the
# ports above are chosen by where those sites are: next to the port's DCMAC
# (DCMAC0 on the right of the service layer, DCMAC1 on the left). With the
# port's own ports (SL2NOC_<2n>, <2n+1>, lanes <2n>, <2n+1>), the cores sit
# across the region from them and two cores miss timing.

# lanes: the two NoC lanes (0..7) of the stream; empty: the default ones
proc slash_add_rdma {eth build rdma_dir {lanes {}} {read_window 32}} {
    switch -- $eth {
        0 { set dc 0; set hier qsfp_0_n_1 }
        2 { set dc 1; set hier qsfp_2_n_3 }
        default { error "slash_add_rdma: eth_$eth has no DCMAC (eth_0 or eth_2)" }
    }
    if {[llength $lanes] == 0} {
        set lanes [expr {$dc ? {2 6} : {0 1}}]
    }
    # the SL2NOC ports of the DataMovers: the static shell fixes the NoC site
    # of each, and these are the ones next to the port's DCMAC (SL2NOC_1/3 in
    # clock region X5 for DCMAC0, SL2NOC_0/2 in X1/X3 for DCMAC1)
    set sl2noc [expr {$dc ? {0 2} : {1 3}}]
    lassign $lanes l0 l1
    set core rdma_$eth

    # ------------------------------------------------------------ sources
    # (the second core finds them in the project already)
    set repos [get_property ip_repo_paths [current_project]]
    set repo [file normalize [file join $rdma_dir iprepo]]
    if {[lsearch -exact $repos $repo] < 0} {
        lappend repos $repo
        set_property ip_repo_paths $repos [current_project]
        update_ip_catalog
    }
    if {[llength [get_ipdefs -quiet -all *:rdma:rdma_200:*]] == 0} {
        error "slash_add_rdma: the rdma_200 IP is not in [file join $rdma_dir iprepo]"
    }
    if {[llength [get_ips -quiet rdma_200_ip]] == 0} {
        create_ip -vlnv [lindex [get_ipdefs -all *:rdma:rdma_200:*] end] -module_name rdma_200_ip
        # QPs 0..7 retransmit WRITEs from the IP's buffer; dcmac_tx_path pads
        # TX frames, dcmac_rx_path drops bad-FCS frames
        set_property -dict [list \
            CONFIG.RT_QP_BITS 3 CONFIG.WR_WINDOW 32 CONFIG.RD_WINDOW $read_window \
            CONFIG.ACK_GAP_CYCLES 20 CONFIG.TX_PAD 0 CONFIG.RX_FCS_FILTER 0 \
        ] [get_ips rdma_200_ip]
    }
    set files {}
    set have [get_files -quiet -of_objects [get_filesets sources_1]]
    foreach f {
        axis_skid.sv axil_split.sv rx_fcs_filter.sv rdma_lane_fifo.sv rdma_stripe.sv rdma_pay_rd.sv
        rdma_pay_wr.sv rdma_service.sv rdma_service_bd.v dcmac_rx_path.v dcmac_tx_path.v
        dcmac_tx_cdc.v dcmac_phy_ctl.v rdma_axi_join.v
    } {
        set path [file normalize [file join $rdma_dir hdl $f]]
        if {[lsearch -exact $have $path] < 0} {
            lappend files $path
        }
    }
    if {[llength $files]} {
        add_files -fileset sources_1 -norecurse $files
        foreach f [get_files -of_objects [get_filesets sources_1] -filter {NAME =~ *.sv}] {
            set_property file_type SystemVerilog $f
        }
        update_compile_order -fileset sources_1
    }

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
    set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_$core]
    connect_bd_net [get_bd_pins ilreduced_logic_0/Res] [get_bd_pins $rst/ext_reset_in]
    connect_bd_net [get_bd_pins $hier/user_clk] [get_bd_pins $rst/slowest_sync_clk] \
        [get_bd_pins $hier/aclk]
    connect_bd_net [get_bd_pins $hier/user_locked] [get_bd_pins $rst/dcm_locked]
    connect_bd_net [get_bd_pins $rst/peripheral_aresetn] [get_bd_pins $hier/aresetn]
    # the core's own service_clk reset: the service layer's one reaches every
    # service_clk cell of the region, and a core far from it misses timing
    set srst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_${core}_s]
    connect_bd_net [get_bd_pins ilreduced_logic_0/Res] [get_bd_pins $srst/ext_reset_in]
    connect_bd_net [get_bd_ports service_clk] [get_bd_pins $srst/slowest_sync_clk]
    set sresetn [get_bd_pins $srst/peripheral_aresetn]

    # ------------------------------------------------------------ the service
    set svc [create_bd_cell -type module -reference rdma_service_bd $core]
    set_property -dict [list CONFIG.BUILD $build CONFIG.ETH_INDEX $eth] $svc
    connect_bd_net [get_bd_pins $hier/user_clk] [get_bd_pins $core/aclk]
    connect_bd_net [get_bd_pins $rst/peripheral_aresetn] [get_bd_pins $core/aresetn]
    connect_bd_net [get_bd_ports service_clk] [get_bd_pins $core/sclk]
    connect_bd_net $sresetn [get_bd_pins $core/sresetn]
    connect_bd_intf_net [get_bd_intf_pins $hier/M_AXIS_0] [get_bd_intf_pins $core/s_axis_rx]
    connect_bd_intf_net [get_bd_intf_pins $core/m_axis_tx] [get_bd_intf_pins $hier/S_AXIS_0]
    connect_bd_net [get_bd_pins $hier/rx_drop_count] [get_bd_pins $core/rx_drop_count]
    connect_bd_net [get_bd_pins $core/phy_ctrl] [get_bd_pins $hier/phy_ctrl]
    connect_bd_net [get_bd_pins $hier/phy_status] [get_bd_pins $core/phy_status]

    # ------------------------------------------------------------ memory
    foreach i {0 1} {
        set p [expr {2 * $eth + $i}]
        set dm_name rdma_dm_$p
        set mem rdma_mem_$p
        set dm [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_datamover:5.1 $dm_name]
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
        # progress pulses for the core's diagnosis counters
        foreach {from to} [list mm2s_addr_req_posted mm2s${i}_addr_posted mm2s_rd_xfer_cmplt mm2s${i}_rd_done \
                               s2mm_addr_req_posted s2mm${i}_addr_posted s2mm_wr_xfer_cmplt s2mm${i}_wr_done] {
            connect_bd_net [get_bd_pins $dm_name/$from] [get_bd_pins $core/$to]
        }
        foreach pin {m_axi_mm2s_aclk m_axis_mm2s_cmdsts_aclk m_axi_s2mm_aclk m_axis_s2mm_cmdsts_awclk} {
            connect_bd_net [get_bd_ports service_clk] [get_bd_pins $dm_name/$pin]
        }
        foreach pin {m_axi_mm2s_aresetn m_axis_mm2s_cmdsts_aresetn m_axi_s2mm_aresetn m_axis_s2mm_cmdsts_aresetn} {
            connect_bd_net $sresetn [get_bd_pins $dm_name/$pin]
        }
        # its reads and writes onto the one port (disjoint AXI channels: a
        # SmartConnect here stalled the writes on hardware)
        create_bd_cell -type module -reference rdma_axi_join $mem
        connect_bd_net [get_bd_ports service_clk] [get_bd_pins $mem/aclk]
        foreach {from to} [list \
            $core/m_axis_mm2s${i}_cmd  $dm_name/S_AXIS_MM2S_CMD \
            $dm_name/M_AXIS_MM2S       $core/s_axis_mm2s$i \
            $dm_name/M_AXIS_MM2S_STS   $core/s_axis_mm2s${i}_sts \
            $core/m_axis_s2mm${i}_cmd  $dm_name/S_AXIS_S2MM_CMD \
            $core/m_axis_s2mm$i        $dm_name/S_AXIS_S2MM \
            $dm_name/M_AXIS_S2MM_STS   $core/s_axis_s2mm${i}_sts \
            $dm_name/M_AXI_MM2S        $mem/s_axi_rd \
            $dm_name/M_AXI_S2MM        $mem/s_axi_wr \
            $mem/m_axi                 sl2noc_[lindex $sl2noc $i]/S00_AXI] {
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
    connect_bd_intf_net [get_bd_intf_pins dummy_noc_$l0/M00_AXIS] [get_bd_intf_pins $core/s_axis_tx0]
    connect_bd_intf_net [get_bd_intf_pins dummy_noc_$l1/M00_AXIS] [get_bd_intf_pins $core/s_axis_tx1]
    connect_bd_intf_net [get_bd_intf_pins $core/m_axis_rx0] [get_bd_intf_pins dummy_noc_m_$l0/S00_AXIS]
    connect_bd_intf_net [get_bd_intf_pins $core/m_axis_rx1] [get_bd_intf_pins dummy_noc_m_$l1/S00_AXIS]

    # ------------------------------------------------------------ memory addresses
    # the SL2NOC ports' aperture: the DDR window of the static shell, seen
    # by each join; the DataMovers see the join's ports at the same place
    # (it passes addresses through)
    foreach i {0 1} {
        set p [expr {2 * $eth + $i}]
        set port SL2NOC_[lindex $sl2noc $i]
        assign_bd_address -offset 0x060000000000 -range 0x000800000000 \
            -target_address_space [get_bd_addr_spaces rdma_mem_$p/m_axi] \
            [get_bd_addr_segs $port/Reg] -force
        foreach {sp intf} {Data_MM2S s_axi_rd Data_S2MM s_axi_wr} {
            set space [get_bd_addr_spaces rdma_dm_$p/$sp]
            assign_bd_address -offset 0x060000000000 -range 0x000800000000 \
                -target_address_space $space [get_bd_addr_segs rdma_mem_$p/$intf/reg0] -force
            # a module port has register usage, the DataMover's space memory
            # usage: BD excludes the segment unless told otherwise
            set ex [get_bd_addr_segs -quiet -excluded -of_objects $space]
            if {[llength $ex]} {
                include_bd_addr_seg $ex
            }
        }
    }
}

# The control segments of the RDMA port eth_<eth>, in S_AXILITE_INI. Called
# after the SmartConnect connects rdma_<eth> and the DCMAC to S_AXILITE_INI:
# before, the segments are not reachable from it and the final
# assign_bd_address places them anywhere.
proc slash_rdma_assign_addresses {eth} {
    set dc [expr {$eth == 0 ? 0 : 1}]
    set hier [expr {$eth == 0 ? "qsfp_0_n_1" : "qsfp_2_n_3"}]
    set as [get_bd_addr_spaces S_AXILITE_INI]
    foreach {seg offset range} [list \
        $hier/dcmac_wrapper/dcmac_$dc/s_axi/Reg [expr {$dc ? 0x020303000000 : 0x020302000000}] 0x00040000 \
        rdma_$eth/s_axil/reg0 [expr {$dc ? 0x020305000000 : 0x020304000000}] 0x01000000] {
        set s [get_bd_addr_segs $seg]
        if {[llength $s] != 1} {
            error "slash_rdma_assign_addresses: segment $seg not found"
        }
        assign_bd_address -offset $offset -range $range -target_address_space $as $s -force
    }
}
