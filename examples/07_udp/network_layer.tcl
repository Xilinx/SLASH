# Normalize pwd
set exec_dir "[file normalize "."]"
set root_dir "${exec_dir}/../../../../../../"

puts "root_dir is ${root_dir}"

# Source information about the network configuration
source ${root_dir}/build/v80-vitis-flow/build/dcmac_config.tcl
source ${root_dir}/build/v80-vitis-flow/resources/dcmac/tcl/nlb.tcl

# Get existing IP repository
set oldrepos [get_property ip_repo_paths [current_project]]
# Update IP repository
set_property ip_repo_paths [list $oldrepos ${root_dir}/vnx/NetLayers] [current_project]
update_ip_catalog

# Open BD
open_bd_design {${exec_dir}/build/prj.srcs/sources_1/bd/top/top.bd}

save_bd_design

# Create network hierarchy
if { ${DCMAC0_ENABLED} == "1" } {
    create_network_layer_box 0
    if { ${DUAL_QSFP_DCMAC0} == "1"} {
        create_network_layer_box 1
    }
    save_bd_design
}
if { ${DCMAC1_ENABLED} == "1" } {
    create_network_layer_box 2
    if { ${DUAL_QSFP_DCMAC1} == "1"} {
        create_network_layer_box 3
    }
    save_bd_design
}

save_bd_design
