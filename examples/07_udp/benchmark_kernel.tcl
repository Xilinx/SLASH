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


# Normalize pwd
set exec_dir "[file normalize "."]"
set root_dir "[file normalize "${exec_dir}/../../../../../../"]"

puts "\n\n\nroot_dir for benchmark kernel is ${root_dir} and exec_dir is ${exec_dir} \n\n\n"

source ${root_dir}/build/v80-vitis-flow/build/dcmac_config.tcl

# Get existing IP repository
set oldrepos [get_property ip_repo_paths [current_project]]

# Update IP repository
set newrepos ${oldrepos}
lappend newrepos [file normalize "${root_dir}/vnx/Benchmark_kernel"]
set_property ip_repo_paths ${newrepos} [current_project]
update_ip_catalog

#set_property ip_repo_paths [list $oldrepos ${root_dir}/vnx/Benchmark_kernel] [current_project]
#update_ip_catalog

# Open BD
set bd_design [get_bd_designs top]
puts "benchmarking kernel bd_design ${bd_design}"
if {[llength ${bd_design}] == 0} {
  # Open BD only if there is no open BD
  puts "Trying to open: ${exec_dir}/build/prj.srcs/sources_1/bd/top/top.bd"
  open_bd_design {${exec_dir}/build/prj.srcs/sources_1/bd/top/top.bd}
}

save_bd_design

# Network Layer Box
proc modify_hier_nlb { nlb_index } {

  variable script_folder

  # Save current instance; Restore later
  set oldCurInst [current_bd_instance .]

  # Set parent object as current
  set parentObj [current_bd_instance .]
  current_bd_instance $parentObj

  # Get bd cell and set as current instance
  set nameHier "nlb${nlb_index}"
  set hier_obj [get_bd_cell $nameHier]
  current_bd_instance $hier_obj

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 summary

  for {set idx 0} {$idx < 4} {incr idx} {
    create_bd_cell -type ip -vlnv xilinx.com:RTLKernel:traffic_generator:1.0 traffic_generator_${idx}
  }

  create_bd_cell -type ip -vlnv xilinx.com:RTLKernel:switch_wrapper:1.0 switch_wrapper_0

  for {set idx 0} {$idx < 4} {incr idx} {
    connect_bd_intf_net [get_bd_intf_pins traffic_generator_${idx}/M_AXIS_k2n] [get_bd_intf_pins switch_wrapper_0/s_tx_in${idx}]
    connect_bd_intf_net [get_bd_intf_pins switch_wrapper_0/m_rx_out${idx}] [get_bd_intf_pins traffic_generator_${idx}/S_AXIS_n2k]
  }

  connect_bd_intf_net [get_bd_intf_pins switch_wrapper_0/m_tx_out] [get_bd_intf_pins networklayer/S_AXIS_sk2nl]
  connect_bd_intf_net [get_bd_intf_pins networklayer/M_AXIS_nl2sk] [get_bd_intf_pins switch_wrapper_0/s_rx_in]

  connect_bd_intf_net [get_bd_intf_pins traffic_generator_0/M_AXIS_summary] [get_bd_intf_pins summary]

  save_bd_design

  set_property -dict [list \
    CONFIG.NUM_MI {5} \
  ] [get_bd_cells smartconnect]

  for {set idx 0} {$idx < 4} {incr idx} {
    connect_bd_net [get_bd_pins ap_clk] [get_bd_pins traffic_generator_${idx}/ap_clk]
    connect_bd_net [get_bd_pins ap_rst_n] [get_bd_pins traffic_generator_${idx}/ap_rst_n]
    set port "[expr {$idx + 1}]"
    connect_bd_intf_net [get_bd_intf_pins smartconnect/M0${port}_AXI] [get_bd_intf_pins traffic_generator_${idx}/S_AXIL]
  }

  save_bd_design

  connect_bd_net [get_bd_pins ap_clk] [get_bd_pins switch_wrapper_0/ap_clk]
  connect_bd_net [get_bd_pins ap_rst_n] [get_bd_pins switch_wrapper_0/ap_rst_n]

  save_bd_design

  for {set idx 0} {$idx < 4} {incr idx} {
    foreach pcie_noc {CPM_PCIE_NOC_0 CPM_PCIE_NOC_1} {
      set offset_increment [expr  0x1000000 * ${nlb_index} + 512 * ${idx}]
      assign_bd_address -offset [expr {0x020104002000 + ${offset_increment}}] -range 512 -target_address_space [get_bd_addr_spaces cips/${pcie_noc}] [get_bd_addr_segs traffic_generator_${idx}/S_AXIL/reg0] -force
      save_bd_design
    }
  }

  save_bd_design
  # Restore current instance
  current_bd_instance $oldCurInst
}

# Modify Base Logic
proc modify_base_logic { nlb_index } {

  variable script_folder

  # Save current instance; Restore later
  set oldCurInst [current_bd_instance .]

  # Set parent object as current
  set parentObj [current_bd_instance .]
  current_bd_instance $parentObj

  # Get bd cell and set as current instance
  set nameHier "base_logic"
  set hier_obj [get_bd_cell $nameHier]
  current_bd_instance $hier_obj

  save_bd_design
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 "summary_${nlb_index}"
  save_bd_design
  connect_bd_intf_net [get_bd_intf_pins summary_${nlb_index}] [get_bd_intf_pins collector_${nlb_index}/summary]
  save_bd_design

  foreach pcie_noc {CPM_PCIE_NOC_0 CPM_PCIE_NOC_1} {
    set offset_increment [expr  0x1000000 * ${nlb_index} + 2048]
    #delete_bd_objs [get_bd_addr_segs cips/${pcie_noc}/SEG_collector_${nlb_index}_Reg]
    assign_bd_address -offset [expr {0x020104002000 + ${offset_increment}}] -range 512 -target_address_space [get_bd_addr_spaces cips/${pcie_noc}] [get_bd_addr_segs collector_${nlb_index}/s_axi_control/Reg] -force
    save_bd_design
  }

  # Restore current instance
  current_bd_instance $oldCurInst
  connect_bd_intf_net [get_bd_intf_pins nlb${nlb_index}/summary] [get_bd_intf_pins base_logic/summary_${nlb_index}]
  save_bd_design
}

# Create network hierarchy
if { ${DCMAC0_ENABLED} == "1" } {
    modify_hier_nlb 0
    modify_base_logic 0
    if { ${DUAL_QSFP_DCMAC0} == "1"} {
        modify_hier_nlb 1
        modify_base_logic 0
    }
    save_bd_design
}
if { ${DCMAC1_ENABLED} == "1" } {
    modify_hier_nlb 2
    modify_base_logic 2
    if { ${DUAL_QSFP_DCMAC1} == "1"} {
        modify_hier_nlb 3
        modify_base_logic 3
    }
    save_bd_design
}

save_bd_design

#close_bd_design [get_bd_designs top]