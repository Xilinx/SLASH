# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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

# Network Layer Box
# Hierarchical cell: qsfp_0_n_1
proc create_hier_nlb { parentCell nameHier} {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_nlb() - Empty argument(s)!"}
     return
  }

  # Get object for parentCell
  set parentObj [get_bd_cells $parentCell]
  if { $parentObj == "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2090 -severity "ERROR" "Unable to find parent cell <$parentCell>!"}
     return
  }

  # Make sure parentObj is hier blk
  set parentType [get_property TYPE $parentObj]
  if { $parentType ne "hier" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2091 -severity "ERROR" "Parent <$parentObj> has TYPE = <$parentType>. Expected to be <hier>."}
     return
  }

  # Save current instance; Restore later
  set oldCurInst [current_bd_instance .]

  # Set parent object as current
  current_bd_instance $parentObj

  # Create cell and set as current instance
  set hier_obj [create_bd_cell -type hier $nameHier]
  current_bd_instance $hier_obj

  # Create interface pins

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 s_axi

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 TX_AXIS

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 RX_AXIS

  # Create pins
  create_bd_pin -dir I -type clk ap_clk
  create_bd_pin -dir I -type rst ap_rst_n

  # Create instance: smartconnect, and set properties
  set smartconnect [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect smartconnect ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MI {1} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect

  create_bd_cell -type ip -vlnv xilinx.com:ip:axis_register_slice ars_tx
  create_bd_cell -type ip -vlnv xilinx.com:ip:axis_register_slice ars_rx

  foreach ipname {ars_tx ars_rx} {
    set_property -dict [list \
      CONFIG.HAS_TSTRB.VALUE_SRC USER \
      CONFIG.TDATA_NUM_BYTES.VALUE_SRC USER \
      CONFIG.HAS_TREADY.VALUE_SRC USER \
      CONFIG.HAS_TKEEP.VALUE_SRC USER \
      CONFIG.HAS_TLAST.VALUE_SRC USER \
      CONFIG.HAS_TKEEP {1} \
      CONFIG.HAS_TLAST {1} \
      CONFIG.REG_CONFIG {8} \
      CONFIG.TDATA_NUM_BYTES {64} \
      CONFIG.TUSER_WIDTH {0} \
    ] [get_bd_cells ${ipname}]
  }

  # Create instance: networklayer, and set properties
  set networklayer [ create_bd_cell -type ip -vlnv xilinx.com:RTLKernel:networklayer networklayer ]

  #set rx_tready [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant rx_tready]
  #set_property CONFIG.CONST_VAL {1} ${rx_tready}

  #set traffic_producer [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer traffic_producer ]
  save_bd_design

  connect_bd_intf_net [get_bd_intf_pins s_axi] [get_bd_intf_pins smartconnect/S00_AXI]
  connect_bd_intf_net [get_bd_intf_pins smartconnect/M00_AXI] [get_bd_intf_pins networklayer/S_AXIL_nl]
  #connect_bd_intf_net [get_bd_intf_pins smartconnect/M01_AXI] [get_bd_intf_pins traffic_producer/s_axi_control]
  connect_bd_intf_net [get_bd_intf_pins RX_AXIS] [get_bd_intf_pins ars_rx/S_AXIS]
  connect_bd_intf_net [get_bd_intf_pins ars_tx/M_AXIS] [get_bd_intf_pins TX_AXIS]
  connect_bd_intf_net [get_bd_intf_pins networklayer/M_AXIS_nl2eth] [get_bd_intf_pins ars_tx/S_AXIS]
  connect_bd_intf_net [get_bd_intf_pins ars_rx/M_AXIS] [get_bd_intf_pins networklayer/S_AXIS_eth2nl]

  save_bd_design

  #connect_bd_intf_net [get_bd_intf_pins traffic_producer/axis_out] [get_bd_intf_pins networklayer/S_AXIS_sk2nl]
  save_bd_design

  connect_bd_net [get_bd_pins ap_clk] [get_bd_pins smartconnect/aclk] [get_bd_pins ars_tx/aclk] [get_bd_pins networklayer/ap_clk] [get_bd_pins ars_rx/aclk]
  connect_bd_net [get_bd_pins ap_rst_n] [get_bd_pins smartconnect/aresetn] [get_bd_pins ars_rx/aresetn] [get_bd_pins ars_tx/aresetn] [get_bd_pins networklayer/ap_rst_n]

  #connect_bd_net [get_bd_pins traffic_producer/ap_clk]
  #connect_bd_net [get_bd_pins traffic_producer/ap_rst_n]


  #connect_bd_net [get_bd_pins rx_tready/dout] [get_bd_pins networklayer/M_AXIS_nl2sk_tready]

  save_bd_design

  #create_bd_cell -type ip -vlnv xilinx.com:ip:axis_ila axis_ila
  #set_property -dict [list \
  #  CONFIG.C_MON_TYPE {Interface_Monitor} \
  #  CONFIG.C_NUM_MONITOR_SLOTS {2} \
  #  CONFIG.C_DATA_DEPTH {2048} \
  #  CONFIG.C_EN_STRG_QUAL {1} \
  #  CONFIG.C_SLOT_0_INTF_TYPE {xilinx.com:interface:axis_rtl:1.0} \
  #  CONFIG.C_SLOT_1_INTF_TYPE {xilinx.com:interface:axis_rtl:1.0} \
  #] [get_bd_cells axis_ila]

  #connect_bd_intf_net [get_bd_intf_pins axis_ila/SLOT_0_AXIS] [get_bd_intf_pins traffic_producer/axis_out]
  #connect_bd_intf_net [get_bd_intf_pins axis_ila/SLOT_1_AXIS] [get_bd_intf_pins ars_tx/S_AXIS]

  #connect_bd_net [get_bd_pins ap_clk] [get_bd_pins axis_ila/clk]
  #connect_bd_net [get_bd_pins ap_rst_n] [get_bd_pins axis_ila/resetn]
  save_bd_design
  # Restore current instance
  current_bd_instance $oldCurInst
}


proc create_network_layer_box { nlb_index } {

  if {![string is integer -strict $nlb_index] || !($nlb_index == 0 || $nlb_index == 1 || $nlb_index == 2 || $nlb_index == 3)} {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "nlb_index (with value $nlb_index) is out range. Valid values are 0, 1, 2 or 3"}
     return
  }

  set nlb_hier_name "nlb${nlb_index}"
  create_hier_nlb [current_bd_instance .] ${nlb_hier_name}
  save_bd_design

  # Add extra manager ports to the memory mapped interconnect

  set num_managers [get_property CONFIG.NUM_MI [get_bd_cells bar_sc]]
  set new_num_managers "[expr {$num_managers + 1}]"
  set manager_port "M0${num_managers}_AXI"

  set_property CONFIG.NUM_MI ${new_num_managers} [get_bd_cells bar_sc]
  connect_bd_intf_net [get_bd_intf_pins bar_sc/${manager_port}] [get_bd_intf_pins ${nlb_hier_name}/s_axi]
  save_bd_design

  if { (${nlb_index} == "0" || ${nlb_index} == "1") } {
    set new_index 0
  } else {
    set new_index 2
  }

  set qsfp_hier_name "qsfp_${new_index}_n_[expr {$new_index + 1}]"
  set qsfp_interface [expr ${nlb_index} % 2]

  connect_bd_intf_net [get_bd_intf_pins ${qsfp_hier_name}/M_AXIS_${qsfp_interface}] [get_bd_intf_pins ${nlb_hier_name}/RX_AXIS]
  connect_bd_intf_net [get_bd_intf_pins ${qsfp_hier_name}/S_AXIS_${qsfp_interface}] [get_bd_intf_pins ${nlb_hier_name}/TX_AXIS]

  connect_bd_net [get_bd_pins base_logic/clk_out2] [get_bd_pins ${nlb_hier_name}/ap_clk]
  connect_bd_net [get_bd_pins base_logic/peripheral_aresetn] [get_bd_pins ${nlb_hier_name}/ap_rst_n]
  save_bd_design

  set offset_increment [expr  0x1000000 * ${nlb_index}]

  # Create address segments
  foreach pcie_noc {CPM_PCIE_NOC_0 CPM_PCIE_NOC_1} {
    assign_bd_address -offset [expr {0x020104000000 + ${offset_increment}}] -range 0x2000 -target_address_space [get_bd_addr_spaces cips/${pcie_noc}] [get_bd_addr_segs /${nlb_hier_name}/networklayer/S_AXIL_nl/reg0] -force
    #assign_bd_address -offset [expr {0x020104002000 + ${offset_increment}}] -range 512 -target_address_space [get_bd_addr_spaces cips/${pcie_noc}] [get_bd_addr_segs /${nlb_hier_name}/traffic_producer/s_axi_control/Reg] -force
    save_bd_design
  }

  # Restore current instance
  save_bd_design
}