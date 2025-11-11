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

puts "\n\n\nroot_dir for network layer is ${root_dir} and exec_dir is ${exec_dir} \n\n\n"

# Source information about the network configuration
source ${root_dir}/build/v80-vitis-flow/build/dcmac_config.tcl
source ${root_dir}/build/v80-vitis-flow/resources/dcmac/tcl/nlb.tcl

# Get existing IP repository
set oldrepos [get_property ip_repo_paths [current_project]]

# Update IP repository
set newrepos ${oldrepos}
lappend newrepos [file normalize "${root_dir}/vnx/NetLayers"]
set_property ip_repo_paths ${newrepos} [current_project]
update_ip_catalog

# Open BD
open_bd_design {${exec_dir}/build/prj.srcs/sources_1/bd/top/top.bd}
save_bd_design

if { ${DCMAC0_ENABLED} == "1" || ${DCMAC1_ENABLED} == "1"} {
    set_property CONFIG.NUM_CLKS {2} [get_bd_cells bar_sc]
    connect_bd_net [get_bd_pins bar_sc/aclk1] [get_bd_pins base_logic/clk_out2]
}

# Create network hierarchy
if { ${DCMAC0_ENABLED} == "1" } {
    disconnect_bd_net /clock_reset_resetn_pl_ic [get_bd_pins qsfp_0_n_1/ap_rst_n]
    disconnect_bd_net /cips_pl0_ref_clk [get_bd_pins qsfp_0_n_1/ap_clk]
    connect_bd_net [get_bd_pins base_logic/clk_out2] [get_bd_pins qsfp_0_n_1/ap_clk]
    connect_bd_net [get_bd_pins base_logic/peripheral_aresetn] [get_bd_pins qsfp_0_n_1/ap_rst_n]
    create_network_layer_box 0
    if { ${DUAL_QSFP_DCMAC0} == "1"} {
        create_network_layer_box 1
    }
    save_bd_design
}
if { ${DCMAC1_ENABLED} == "1" } {
    disconnect_bd_net /clock_reset_resetn_pl_ic [get_bd_pins qsfp_2_n_3/ap_rst_n]
    disconnect_bd_net /cips_pl0_ref_clk [get_bd_pins qsfp_2_n_3/ap_clk]
    connect_bd_net [get_bd_pins base_logic/clk_out2] [get_bd_pins qsfp_2_n_3/ap_clk]
    connect_bd_net [get_bd_pins base_logic/peripheral_aresetn] [get_bd_pins qsfp_2_n_3/ap_rst_n]
    create_network_layer_box 2
    if { ${DUAL_QSFP_DCMAC1} == "1"} {
        create_network_layer_box 3
    }
    save_bd_design
}

save_bd_design
#close_bd_design [get_bd_designs top]