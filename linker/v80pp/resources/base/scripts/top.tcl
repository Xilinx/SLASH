
################################################################
# This is a generated script based on design: top
#
# Though there are limitations about the generated script,
# the main purpose of this utility is to make learning
# IP Integrator Tcl commands easier.
################################################################

namespace eval _tcl {
proc get_script_folder {} {
   set script_path [file normalize [info script]]
   set script_folder [file dirname $script_path]
   return $script_folder
}
}
variable script_folder
set script_folder [_tcl::get_script_folder]

################################################################
# Check if script is running in correct Vivado version.
################################################################
set scripts_vivado_version 2025.1
set current_vivado_version [version -short]

if { [string first $scripts_vivado_version $current_vivado_version] == -1 } {
   puts ""
   if { [string compare $scripts_vivado_version $current_vivado_version] > 0 } {
      catch {common::send_gid_msg -ssname BD::TCL -id 2042 -severity "ERROR" " This script was generated using Vivado <$scripts_vivado_version> and is being run in <$current_vivado_version> of Vivado. Sourcing the script failed since it was created with a future version of Vivado."}

   } else {
     catch {common::send_gid_msg -ssname BD::TCL -id 2041 -severity "ERROR" "This script was generated using Vivado <$scripts_vivado_version> and is being run in <$current_vivado_version> of Vivado. Please run the script in Vivado <$scripts_vivado_version> then open the design in Vivado <$current_vivado_version>. Upgrade the design by running \"Tools => Report => Report IP Status...\", then run write_bd_tcl to create an updated script."}

   }

   return 1
}

################################################################
# START
################################################################

# To test this script, run the following commands from Vivado Tcl console:
# source top_script.tcl


# The design that will be created by this Tcl script contains the following 
# block design container source references:
# slash_base, slash_vadd, service_layer, service_layer_vadd

# Please add the sources before sourcing this Tcl script.

# If there is no project opened, this script will create a
# project, but make sure you do not have an existing project
# <./myproj/project_1.xpr> in the current working folder.

set list_projs [get_projects -quiet]
if { $list_projs eq "" } {
   create_project project_1 myproj -part xcv80-lsva4737-2MHP-e-S
}


# CHANGE DESIGN NAME HERE
variable design_name
set design_name top

# If you do not already have an existing IP Integrator design open,
# you can create a design using the following command:
#    create_bd_design $design_name

# Creating design if needed
set errMsg ""
set nRet 0

set cur_design [current_bd_design -quiet]
set list_cells [get_bd_cells -quiet]

if { ${design_name} eq "" } {
   # USE CASES:
   #    1) Design_name not set

   set errMsg "Please set the variable <design_name> to a non-empty value."
   set nRet 1

} elseif { ${cur_design} ne "" && ${list_cells} eq "" } {
   # USE CASES:
   #    2): Current design opened AND is empty AND names same.
   #    3): Current design opened AND is empty AND names diff; design_name NOT in project.
   #    4): Current design opened AND is empty AND names diff; design_name exists in project.

   if { $cur_design ne $design_name } {
      common::send_gid_msg -ssname BD::TCL -id 2001 -severity "INFO" "Changing value of <design_name> from <$design_name> to <$cur_design> since current design is empty."
      set design_name [get_property NAME $cur_design]
   }
   common::send_gid_msg -ssname BD::TCL -id 2002 -severity "INFO" "Constructing design in IPI design <$cur_design>..."

} elseif { ${cur_design} ne "" && $list_cells ne "" && $cur_design eq $design_name } {
   # USE CASES:
   #    5) Current design opened AND has components AND same names.

   set errMsg "Design <$design_name> already exists in your project, please set the variable <design_name> to another value."
   set nRet 1
} elseif { [get_files -quiet ${design_name}.bd] ne "" } {
   # USE CASES: 
   #    6) Current opened design, has components, but diff names, design_name exists in project.
   #    7) No opened design, design_name exists in project.

   set errMsg "Design <$design_name> already exists in your project, please set the variable <design_name> to another value."
   set nRet 2

} else {
   # USE CASES:
   #    8) No opened design, design_name not in project.
   #    9) Current opened design, has components, but diff names, design_name not in project.

   common::send_gid_msg -ssname BD::TCL -id 2003 -severity "INFO" "Currently there is no design <$design_name> in project, so creating one..."

   create_bd_design $design_name

   common::send_gid_msg -ssname BD::TCL -id 2004 -severity "INFO" "Making design <$design_name> as current_bd_design."
   current_bd_design $design_name

}

common::send_gid_msg -ssname BD::TCL -id 2005 -severity "INFO" "Currently the variable <design_name> is equal to \"$design_name\"."

if { $nRet != 0 } {
   catch {common::send_gid_msg -ssname BD::TCL -id 2006 -severity "ERROR" $errMsg}
   return $nRet
}

set bCheckIPsPassed 1
##################################################################
# CHECK IPs
##################################################################
set bCheckIPs 1
if { $bCheckIPs == 1 } {
   set list_check_ips "\ 
xilinx.com:ip:clk_wizard:1.0\
xilinx.com:ip:proc_sys_reset:5.0\
xilinx.com:ip:axi_noc:1.1\
xilinx.com:ip:util_vector_logic:2.0\
xilinx.com:ip:dfx_decoupler:1.0\
xilinx.com:ip:versal_cips:3.4\
xilinx.com:ip:axis_noc:1.0\
xilinx.com:ip:smartconnect:1.0\
xilinx.com:inline_hdl:ilreduced_logic:1.0\
xilinx.com:ip:c_shift_ram:12.0\
xilinx.com:ip:hw_discovery:1.0\
xilinx.com:ip:shell_utils_uuid_rom:2.0\
xilinx.com:ip:smbus:1.1\
xilinx.com:ip:cmd_queue:2.0\
xilinx.com:ip:axi_gpio:2.0\
xilinx.com:ip:xlconcat:2.1\
xilinx.com:ip:util_reduced_logic:2.0\
"

   set list_ips_missing ""
   common::send_gid_msg -ssname BD::TCL -id 2011 -severity "INFO" "Checking if the following IPs exist in the project's IP catalog: $list_check_ips ."

   foreach ip_vlnv $list_check_ips {
      set ip_obj [get_ipdefs -all $ip_vlnv]
      if { $ip_obj eq "" } {
         lappend list_ips_missing $ip_vlnv
      }
   }

   if { $list_ips_missing ne "" } {
      catch {common::send_gid_msg -ssname BD::TCL -id 2012 -severity "ERROR" "The following IPs are not found in the IP Catalog:\n  $list_ips_missing\n\nResolution: Please add the repository containing the IP(s) to the project." }
      set bCheckIPsPassed 0
   }

}

##################################################################
# CHECK Block Design Container Sources
##################################################################
set bCheckSources 1
set list_bdc_active "slash_base, service_layer"

array set map_bdc_missing {}
set map_bdc_missing(ACTIVE) ""
set map_bdc_missing(DFX) ""
set map_bdc_missing(BDC) ""

if { $bCheckSources == 1 } {
   set list_check_srcs "\ 
slash_base \
service_layer \
"

   common::send_gid_msg -ssname BD::TCL -id 2056 -severity "INFO" "Checking if the following sources for block design container exist in the project: $list_check_srcs .\n\n"

   foreach src $list_check_srcs {
      if { [can_resolve_reference $src] == 0 } {
         if { [lsearch $list_bdc_active $src] != -1 } {
            set map_bdc_missing(ACTIVE) "$map_bdc_missing(ACTIVE) $src"
         } else {
            set map_bdc_missing(BDC) "$map_bdc_missing(BDC) $src"
         }
      }
   }

   if { [llength $map_bdc_missing(ACTIVE)] > 0 } {
      catch {common::send_gid_msg -ssname BD::TCL -id 2057 -severity "ERROR" "The following source(s) of Active variants are not found in the project: $map_bdc_missing(ACTIVE)" }
      common::send_gid_msg -ssname BD::TCL -id 2060 -severity "INFO" "Please add source files for the missing source(s) above."
      set bCheckIPsPassed 0
   }
   if { [llength $map_bdc_missing(DFX)] > 0 } {
      catch {common::send_gid_msg -ssname BD::TCL -id 2058 -severity "ERROR" "The following source(s) of DFX variants are not found in the project: $map_bdc_missing(DFX)" }
      common::send_gid_msg -ssname BD::TCL -id 2060 -severity "INFO" "Please add source files for the missing source(s) above."
      set bCheckIPsPassed 0
   }
   if { [llength $map_bdc_missing(BDC)] > 0 } {
      catch {common::send_gid_msg -ssname BD::TCL -id 2059 -severity "WARNING" "The following source(s) of variants are not found in the project: $map_bdc_missing(BDC)" }
      common::send_gid_msg -ssname BD::TCL -id 2060 -severity "INFO" "Please add source files for the missing source(s) above."
   }
}

if { $bCheckIPsPassed != 1 } {
  common::send_gid_msg -ssname BD::TCL -id 2023 -severity "WARNING" "Will not continue with creation of design due to the error(s) above."
  return 3
}

##################################################################
# DESIGN PROCs
##################################################################


# Hierarchical cell: pcie_mgmt_pdi_reset
proc create_hier_cell_pcie_mgmt_pdi_reset { parentCell nameHier } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_pcie_mgmt_pdi_reset() - Empty argument(s)!"}
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


  # Create pins
  create_bd_pin -dir I -type clk clk
  create_bd_pin -dir I -type rst resetn
  create_bd_pin -dir I -type rst resetn_in

  # Create instance: pcie_mgmt_pdi_reset_gpio, and set properties
  set pcie_mgmt_pdi_reset_gpio [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 pcie_mgmt_pdi_reset_gpio ]
  set_property -dict [list \
    CONFIG.C_ALL_INPUTS_2 {1} \
    CONFIG.C_ALL_OUTPUTS {1} \
    CONFIG.C_DOUT_DEFAULT {0x00000000} \
    CONFIG.C_GPIO2_WIDTH {1} \
    CONFIG.C_GPIO_WIDTH {1} \
    CONFIG.C_IS_DUAL {1} \
  ] $pcie_mgmt_pdi_reset_gpio


  # Create instance: inv, and set properties
  set inv [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic:2.0 inv ]
  set_property -dict [list \
    CONFIG.C_OPERATION {not} \
    CONFIG.C_SIZE {1} \
  ] $inv


  # Create instance: ccat, and set properties
  set ccat [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 ccat ]

  # Create instance: and_0, and set properties
  set and_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_reduced_logic:2.0 and_0 ]
  set_property CONFIG.C_SIZE {2} $and_0


  # Create interface connections
  connect_bd_intf_net -intf_net s_axi_1 [get_bd_intf_pins s_axi] [get_bd_intf_pins pcie_mgmt_pdi_reset_gpio/S_AXI]

  # Create port connections
  connect_bd_net -net and_0_Res  [get_bd_pins and_0/Res] \
  [get_bd_pins pcie_mgmt_pdi_reset_gpio/gpio2_io_i]
  connect_bd_net -net ccat_dout  [get_bd_pins ccat/dout] \
  [get_bd_pins and_0/Op1]
  connect_bd_net -net clk_1  [get_bd_pins clk] \
  [get_bd_pins pcie_mgmt_pdi_reset_gpio/s_axi_aclk]
  connect_bd_net -net inv_Res  [get_bd_pins inv/Res] \
  [get_bd_pins ccat/In1]
  connect_bd_net -net pcie_mgmt_pdi_reset_gpio_gpio_io_o  [get_bd_pins pcie_mgmt_pdi_reset_gpio/gpio_io_o] \
  [get_bd_pins ccat/In0]
  connect_bd_net -net resetn_1  [get_bd_pins resetn] \
  [get_bd_pins pcie_mgmt_pdi_reset_gpio/s_axi_aresetn]
  connect_bd_net -net resetn_in_1  [get_bd_pins resetn_in] \
  [get_bd_pins inv/Op1]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: base_logic
proc create_hier_cell_base_logic { parentCell nameHier } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_base_logic() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 s_axi_pcie_mgmt_slr0

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 s_axi_rpu

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:pcie3_cfg_ext_rtl:1.0 pcie_cfg_ext

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 smbus_rpu

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 m_axi_pcie_mgmt_pdi_reset


  # Create pins
  create_bd_pin -dir I -type clk clk_pcie
  create_bd_pin -dir I -type clk clk_pl
  create_bd_pin -dir I -type rst resetn_pcie_periph
  create_bd_pin -dir I -type rst resetn_pl_periph
  create_bd_pin -dir I -type rst resetn_pl_ic
  create_bd_pin -dir O -type intr irq_gcq_m2r
  create_bd_pin -dir O -type intr irq_axi_smbus_rpu

  # Create instance: pcie_slr0_mgmt_sc, and set properties
  set pcie_slr0_mgmt_sc [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 pcie_slr0_mgmt_sc ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MI {4} \
    CONFIG.NUM_SI {1} \
  ] $pcie_slr0_mgmt_sc


  # Create instance: rpu_sc, and set properties
  set rpu_sc [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 rpu_sc ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MI {2} \
    CONFIG.NUM_SI {1} \
  ] $rpu_sc


  # Create instance: hw_discovery, and set properties
  set hw_discovery [ create_bd_cell -type ip -vlnv xilinx.com:ip:hw_discovery:1.0 hw_discovery ]
  set_property -dict [list \
    CONFIG.C_CAP_BASE_ADDR {0x600} \
    CONFIG.C_INJECT_ENDPOINTS {0} \
    CONFIG.C_MANUAL {1} \
    CONFIG.C_NEXT_CAP_ADDR {0x000} \
    CONFIG.C_NUM_PFS {1} \
    CONFIG.C_PF0_BAR_INDEX {0} \
    CONFIG.C_PF0_ENDPOINT_NAMES {0} \
    CONFIG.C_PF0_ENTRY_ADDR_0 {0x000001001000} \
    CONFIG.C_PF0_ENTRY_ADDR_1 {0x000001010000} \
    CONFIG.C_PF0_ENTRY_ADDR_2 {0x000008000000} \
    CONFIG.C_PF0_ENTRY_BAR_0 {0} \
    CONFIG.C_PF0_ENTRY_BAR_1 {0} \
    CONFIG.C_PF0_ENTRY_BAR_2 {0} \
    CONFIG.C_PF0_ENTRY_MAJOR_VERSION_0 {1} \
    CONFIG.C_PF0_ENTRY_MAJOR_VERSION_1 {1} \
    CONFIG.C_PF0_ENTRY_MAJOR_VERSION_2 {1} \
    CONFIG.C_PF0_ENTRY_MINOR_VERSION_0 {0} \
    CONFIG.C_PF0_ENTRY_MINOR_VERSION_1 {2} \
    CONFIG.C_PF0_ENTRY_MINOR_VERSION_2 {0} \
    CONFIG.C_PF0_ENTRY_RSVD0_0 {0x0} \
    CONFIG.C_PF0_ENTRY_RSVD0_1 {0x0} \
    CONFIG.C_PF0_ENTRY_RSVD0_2 {0x0} \
    CONFIG.C_PF0_ENTRY_TYPE_0 {0x50} \
    CONFIG.C_PF0_ENTRY_TYPE_1 {0x54} \
    CONFIG.C_PF0_ENTRY_TYPE_2 {0x55} \
    CONFIG.C_PF0_ENTRY_VERSION_TYPE_0 {0x01} \
    CONFIG.C_PF0_ENTRY_VERSION_TYPE_1 {0x01} \
    CONFIG.C_PF0_ENTRY_VERSION_TYPE_2 {0x01} \
    CONFIG.C_PF0_HIGH_OFFSET {0x00000000} \
    CONFIG.C_PF0_LOW_OFFSET {0x0100000} \
    CONFIG.C_PF0_NUM_SLOTS_BAR_LAYOUT_TABLE {3} \
    CONFIG.C_PF0_S_AXI_ADDR_WIDTH {32} \
  ] $hw_discovery


  # Create instance: uuid_rom, and set properties
  set uuid_rom [ create_bd_cell -type ip -vlnv xilinx.com:ip:shell_utils_uuid_rom:2.0 uuid_rom ]
  set_property CONFIG.C_INITIAL_UUID {00000000000000000000000000000000} $uuid_rom


  # Create instance: axi_smbus_rpu, and set properties
  set axi_smbus_rpu [ create_bd_cell -type ip -vlnv xilinx.com:ip:smbus:1.1 axi_smbus_rpu ]
  set_property -dict [list \
    CONFIG.NUM_TARGET_DEVICES {8} \
    CONFIG.SMBUS_DEV_CLASS {0} \
  ] $axi_smbus_rpu


  # Create instance: gcq_m2r, and set properties
  set gcq_m2r [ create_bd_cell -type ip -vlnv xilinx.com:ip:cmd_queue:2.0 gcq_m2r ]

  # Create interface connections
  connect_bd_intf_net -intf_net axi_smbus_rpu_SMBUS [get_bd_intf_pins axi_smbus_rpu/SMBUS] [get_bd_intf_pins smbus_rpu]
  connect_bd_intf_net -intf_net pcie_cfg_ext_1 [get_bd_intf_pins pcie_cfg_ext] [get_bd_intf_pins hw_discovery/s_pcie4_cfg_ext]
  connect_bd_intf_net -intf_net pcie_slr0_mgmt_sc_M00_AXI [get_bd_intf_pins pcie_slr0_mgmt_sc/M00_AXI] [get_bd_intf_pins hw_discovery/s_axi_ctrl_pf0]
  connect_bd_intf_net -intf_net pcie_slr0_mgmt_sc_M01_AXI [get_bd_intf_pins pcie_slr0_mgmt_sc/M01_AXI] [get_bd_intf_pins uuid_rom/S_AXI]
  connect_bd_intf_net -intf_net pcie_slr0_mgmt_sc_M02_AXI [get_bd_intf_pins pcie_slr0_mgmt_sc/M02_AXI] [get_bd_intf_pins gcq_m2r/S00_AXI]
  connect_bd_intf_net -intf_net pcie_slr0_mgmt_sc_M03_AXI [get_bd_intf_pins pcie_slr0_mgmt_sc/M03_AXI] [get_bd_intf_pins m_axi_pcie_mgmt_pdi_reset]
  connect_bd_intf_net -intf_net rpu_sc_M00_AXI [get_bd_intf_pins rpu_sc/M00_AXI] [get_bd_intf_pins gcq_m2r/S01_AXI]
  connect_bd_intf_net -intf_net rpu_sc_M01_AXI [get_bd_intf_pins axi_smbus_rpu/S_AXI] [get_bd_intf_pins rpu_sc/M01_AXI]
  connect_bd_intf_net -intf_net s_axi_pcie_mgmt_slr0_1 [get_bd_intf_pins s_axi_pcie_mgmt_slr0] [get_bd_intf_pins pcie_slr0_mgmt_sc/S00_AXI]
  connect_bd_intf_net -intf_net s_axi_rpu_1 [get_bd_intf_pins s_axi_rpu] [get_bd_intf_pins rpu_sc/S00_AXI]

  # Create port connections
  connect_bd_net -net axi_smbus_rpu_ip2intc_irpt  [get_bd_pins axi_smbus_rpu/ip2intc_irpt] \
  [get_bd_pins irq_axi_smbus_rpu]
  connect_bd_net -net clk_pcie_1  [get_bd_pins clk_pcie] \
  [get_bd_pins hw_discovery/aclk_pcie]
  connect_bd_net -net clk_pl_1  [get_bd_pins clk_pl] \
  [get_bd_pins pcie_slr0_mgmt_sc/aclk] \
  [get_bd_pins rpu_sc/aclk] \
  [get_bd_pins hw_discovery/aclk_ctrl] \
  [get_bd_pins uuid_rom/S_AXI_ACLK] \
  [get_bd_pins gcq_m2r/aclk] \
  [get_bd_pins axi_smbus_rpu/s_axi_aclk]
  connect_bd_net -net gcq_m2r_irq_sq  [get_bd_pins gcq_m2r/irq_sq] \
  [get_bd_pins irq_gcq_m2r]
  connect_bd_net -net resetn_pcie_periph_1  [get_bd_pins resetn_pcie_periph] \
  [get_bd_pins hw_discovery/aresetn_pcie]
  connect_bd_net -net resetn_pl_ic_1  [get_bd_pins resetn_pl_ic] \
  [get_bd_pins pcie_slr0_mgmt_sc/aresetn] \
  [get_bd_pins rpu_sc/aresetn]
  connect_bd_net -net resetn_pl_periph_1  [get_bd_pins resetn_pl_periph] \
  [get_bd_pins hw_discovery/aresetn_ctrl] \
  [get_bd_pins uuid_rom/S_AXI_ARESETN] \
  [get_bd_pins gcq_m2r/aresetn] \
  [get_bd_pins axi_smbus_rpu/s_axi_aresetn]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: clock_reset
proc create_hier_cell_clock_reset { parentCell nameHier } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_clock_reset() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 s_axi_pcie_mgmt_pdi_reset


  # Create pins
  create_bd_pin -dir I -type clk clk_pl
  create_bd_pin -dir I -type clk clk_freerun
  create_bd_pin -dir I -type clk clk_pcie
  create_bd_pin -dir I -type rst dma_axi_aresetn
  create_bd_pin -dir I -type rst resetn_pl_axi
  create_bd_pin -dir O -from 0 -to 0 -type rst resetn_pcie_ic
  create_bd_pin -dir O -from 0 -to 0 -type rst resetn_pcie_periph
  create_bd_pin -dir O -from 0 -to 0 -type rst resetn_pl_ic
  create_bd_pin -dir O -from 0 -to 0 -type rst resetn_pl_periph
  create_bd_pin -dir O -type clk clk_usr_0
  create_bd_pin -dir O -from 0 -to 0 -type rst resetn_usr_0_ic
  create_bd_pin -dir O -from 0 -to 0 -type rst resetn_usr_0_periph
  create_bd_pin -dir O -type clk clk_usr_1
  create_bd_pin -dir O -from 0 -to 0 -type rst resetn_usr_1_ic
  create_bd_pin -dir O -from 0 -to 0 -type rst resetn_usr_1_periph

  # Create instance: pcie_psr, and set properties
  set pcie_psr [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 pcie_psr ]
  set_property CONFIG.C_EXT_RST_WIDTH {1} $pcie_psr


  # Create instance: pl_psr, and set properties
  set pl_psr [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 pl_psr ]
  set_property CONFIG.C_EXT_RST_WIDTH {1} $pl_psr


  # Create instance: usr_clk_wiz, and set properties
  set usr_clk_wiz [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wizard:1.0 usr_clk_wiz ]
  set_property -dict [list \
    CONFIG.CLKOUT_DRIVES {No_buffer,No_buffer} \
    CONFIG.CLKOUT_REQUESTED_OUT_FREQUENCY {300,500} \
    CONFIG.CLKOUT_USED {true,true} \
    CONFIG.PRIM_SOURCE {No_buffer} \
    CONFIG.USE_DYN_RECONFIG {false} \
    CONFIG.USE_LOCKED {true} \
    CONFIG.USE_POWER_DOWN {false} \
    CONFIG.USE_RESET {false} \
  ] $usr_clk_wiz


  # Create instance: usr_0_psr, and set properties
  set usr_0_psr [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 usr_0_psr ]
  set_property CONFIG.C_EXT_RST_WIDTH {1} $usr_0_psr


  # Create instance: usr_1_psr, and set properties
  set usr_1_psr [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 usr_1_psr ]
  set_property CONFIG.C_EXT_RST_WIDTH {1} $usr_1_psr


  # Create instance: pcie_mgmt_pdi_reset
  create_hier_cell_pcie_mgmt_pdi_reset $hier_obj pcie_mgmt_pdi_reset

  # Create interface connections
  connect_bd_intf_net -intf_net s_axi_pcie_mgmt_pdi_reset_1 [get_bd_intf_pins s_axi_pcie_mgmt_pdi_reset] [get_bd_intf_pins pcie_mgmt_pdi_reset/s_axi]

  # Create port connections
  connect_bd_net -net clk_freerun_1  [get_bd_pins clk_freerun] \
  [get_bd_pins usr_clk_wiz/clk_in1]
  connect_bd_net -net clk_pcie_1  [get_bd_pins clk_pcie] \
  [get_bd_pins pcie_psr/slowest_sync_clk]
  connect_bd_net -net clk_pl_1  [get_bd_pins clk_pl] \
  [get_bd_pins pl_psr/slowest_sync_clk] \
  [get_bd_pins pcie_mgmt_pdi_reset/clk]
  connect_bd_net -net dma_axi_aresetn_1  [get_bd_pins dma_axi_aresetn] \
  [get_bd_pins pcie_mgmt_pdi_reset/resetn_in]
  connect_bd_net -net pcie_psr_interconnect_aresetn  [get_bd_pins pcie_psr/interconnect_aresetn] \
  [get_bd_pins resetn_pcie_ic]
  connect_bd_net -net pcie_psr_peripheral_aresetn  [get_bd_pins pcie_psr/peripheral_aresetn] \
  [get_bd_pins resetn_pcie_periph]
  connect_bd_net -net pl_psr_interconnect_aresetn  [get_bd_pins pl_psr/interconnect_aresetn] \
  [get_bd_pins resetn_pl_ic] \
  [get_bd_pins pcie_psr/ext_reset_in] \
  [get_bd_pins usr_0_psr/ext_reset_in] \
  [get_bd_pins usr_1_psr/ext_reset_in]
  connect_bd_net -net pl_psr_peripheral_aresetn  [get_bd_pins pl_psr/peripheral_aresetn] \
  [get_bd_pins resetn_pl_periph] \
  [get_bd_pins pcie_mgmt_pdi_reset/resetn]
  connect_bd_net -net resetn_pl_axi_1  [get_bd_pins resetn_pl_axi] \
  [get_bd_pins pl_psr/ext_reset_in]
  connect_bd_net -net usr_0_psr_interconnect_aresetn  [get_bd_pins usr_0_psr/interconnect_aresetn] \
  [get_bd_pins resetn_usr_0_ic]
  connect_bd_net -net usr_0_psr_peripheral_aresetn  [get_bd_pins usr_0_psr/peripheral_aresetn] \
  [get_bd_pins resetn_usr_0_periph]
  connect_bd_net -net usr_1_psr_interconnect_aresetn  [get_bd_pins usr_1_psr/interconnect_aresetn] \
  [get_bd_pins resetn_usr_1_ic]
  connect_bd_net -net usr_1_psr_peripheral_aresetn  [get_bd_pins usr_1_psr/peripheral_aresetn] \
  [get_bd_pins resetn_usr_1_periph]
  connect_bd_net -net usr_clk_wiz_clk_out1  [get_bd_pins usr_clk_wiz/clk_out1] \
  [get_bd_pins clk_usr_0] \
  [get_bd_pins usr_0_psr/slowest_sync_clk]
  connect_bd_net -net usr_clk_wiz_clk_out2  [get_bd_pins usr_clk_wiz/clk_out2] \
  [get_bd_pins clk_usr_1] \
  [get_bd_pins usr_1_psr/slowest_sync_clk]
  connect_bd_net -net usr_clk_wiz_locked  [get_bd_pins usr_clk_wiz/locked] \
  [get_bd_pins usr_0_psr/dcm_locked] \
  [get_bd_pins usr_1_psr/dcm_locked]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: clk_rst_shell
proc create_hier_cell_clk_rst_shell { parentCell nameHier } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_clk_rst_shell() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI


  # Create pins
  create_bd_pin -dir I -type clk pl0_ref_clk
  create_bd_pin -dir I -type rst aresetn
  create_bd_pin -dir I -type clk refclk
  create_bd_pin -dir O -type clk service_clk
  create_bd_pin -dir O -from 0 -to 0 -type rst service_arstn
  create_bd_pin -dir O -type clk slash_clk
  create_bd_pin -dir O -from 0 -to 0 -type rst slash_arstn

  # Create instance: axi_noc_0, and set properties
  set axi_noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_0


  set_property -dict [ list \
   CONFIG.APERTURES {{0x204_0000_0000 512K}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /static_region/clk_rst_shell/axi_noc_0/M00_AXI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_AXI {read_bw {500} write_bw {500} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /static_region/clk_rst_shell/axi_noc_0/S00_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI} \
 ] [get_bd_pins /static_region/clk_rst_shell/axi_noc_0/aclk0]

  # Create instance: smartconnect_0, and set properties
  set smartconnect_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_0 ]
  set_property -dict [list \
    CONFIG.NUM_MI {2} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect_0


  # Create instance: clk_wizard_slash, and set properties
  set clk_wizard_slash [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wizard:1.0 clk_wizard_slash ]
  set_property -dict [list \
    CONFIG.CLKOUT_DRIVES {BUFG,BUFG,BUFG,BUFG,BUFG,BUFG,BUFG} \
    CONFIG.CLKOUT_DYN_PS {None,None,None,None,None,None,None} \
    CONFIG.CLKOUT_GROUPING {Auto,Auto,Auto,Auto,Auto,Auto,Auto} \
    CONFIG.CLKOUT_MATCHED_ROUTING {false,false,false,false,false,false,false} \
    CONFIG.CLKOUT_PORT {clk_out1,clk_out2,clk_out3,clk_out4,clk_out5,clk_out6,clk_out7} \
    CONFIG.CLKOUT_REQUESTED_DUTY_CYCLE {50.000,50.000,50.000,50.000,50.000,50.000,50.000} \
    CONFIG.CLKOUT_REQUESTED_OUT_FREQUENCY {200.000,100.000,100.000,100.000,100.000,100.000,100.000} \
    CONFIG.CLKOUT_REQUESTED_PHASE {0.000,0.000,0.000,0.000,0.000,0.000,0.000} \
    CONFIG.CLKOUT_USED {true,false,false,false,false,false,false} \
    CONFIG.USE_DYN_RECONFIG {true} \
  ] $clk_wizard_slash


  # Create instance: clk_wizard_service, and set properties
  set clk_wizard_service [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wizard:1.0 clk_wizard_service ]
  set_property -dict [list \
    CONFIG.CLKOUT_DRIVES {BUFG,BUFG,BUFG,BUFG,BUFG,BUFG,BUFG} \
    CONFIG.CLKOUT_DYN_PS {None,None,None,None,None,None,None} \
    CONFIG.CLKOUT_GROUPING {Auto,Auto,Auto,Auto,Auto,Auto,Auto} \
    CONFIG.CLKOUT_MATCHED_ROUTING {false,false,false,false,false,false,false} \
    CONFIG.CLKOUT_PORT {clk_out1,clk_out2,clk_out3,clk_out4,clk_out5,clk_out6,clk_out7} \
    CONFIG.CLKOUT_REQUESTED_DUTY_CYCLE {50.000,50.000,50.000,50.000,50.000,50.000,50.000} \
    CONFIG.CLKOUT_REQUESTED_OUT_FREQUENCY {300.000,100.000,100.000,100.000,100.000,100.000,100.000} \
    CONFIG.CLKOUT_REQUESTED_PHASE {0.000,0.000,0.000,0.000,0.000,0.000,0.000} \
    CONFIG.CLKOUT_USED {true,false,false,false,false,false,false} \
    CONFIG.USE_DYN_RECONFIG {true} \
  ] $clk_wizard_service


  # Create instance: proc_sys_reset_0, and set properties
  set proc_sys_reset_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_0 ]

  # Create instance: proc_sys_reset_1, and set properties
  set proc_sys_reset_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_1 ]

  # Create instance: slash_rst_conv_in, and set properties
  set slash_rst_conv_in [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilreduced_logic:1.0 slash_rst_conv_in ]
  set_property -dict [list \
    CONFIG.C_OPERATION {or} \
    CONFIG.C_SIZE {1} \
  ] $slash_rst_conv_in


  # Create instance: service_rst_conv_in, and set properties
  set service_rst_conv_in [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilreduced_logic:1.0 service_rst_conv_in ]
  set_property -dict [list \
    CONFIG.C_OPERATION {or} \
    CONFIG.C_SIZE {1} \
  ] $service_rst_conv_in


  # Create instance: slash_rst_pipe_slr0, and set properties
  set slash_rst_pipe_slr0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:c_shift_ram:12.0 slash_rst_pipe_slr0 ]
  set_property -dict [list \
    CONFIG.Depth {1} \
    CONFIG.Width {1} \
  ] $slash_rst_pipe_slr0


  # Create instance: service_rst_pipe_slr0, and set properties
  set service_rst_pipe_slr0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:c_shift_ram:12.0 service_rst_pipe_slr0 ]
  set_property -dict [list \
    CONFIG.Depth {1} \
    CONFIG.Width {1} \
  ] $service_rst_pipe_slr0


  # Create interface connections
  connect_bd_intf_net -intf_net S00_INI_1 [get_bd_intf_pins S00_INI] [get_bd_intf_pins axi_noc_0/S00_INI]
  connect_bd_intf_net -intf_net axi_noc_0_M00_AXI [get_bd_intf_pins smartconnect_0/S00_AXI] [get_bd_intf_pins axi_noc_0/M00_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M00_AXI [get_bd_intf_pins clk_wizard_slash/s_axi_lite] [get_bd_intf_pins smartconnect_0/M00_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M01_AXI [get_bd_intf_pins clk_wizard_service/s_axi_lite] [get_bd_intf_pins smartconnect_0/M01_AXI]

  # Create port connections
  connect_bd_net -net aresetn_1  [get_bd_pins aresetn] \
  [get_bd_pins smartconnect_0/aresetn] \
  [get_bd_pins clk_wizard_slash/s_axi_aresetn] \
  [get_bd_pins clk_wizard_service/s_axi_aresetn] \
  [get_bd_pins proc_sys_reset_0/ext_reset_in] \
  [get_bd_pins proc_sys_reset_1/ext_reset_in]
  connect_bd_net -net clk_wizard_service_clk_out1  [get_bd_pins clk_wizard_service/clk_out1] \
  [get_bd_pins proc_sys_reset_1/slowest_sync_clk] \
  [get_bd_pins service_clk] \
  [get_bd_pins service_rst_pipe_slr0/CLK]
  connect_bd_net -net clk_wizard_service_locked  [get_bd_pins clk_wizard_service/locked] \
  [get_bd_pins proc_sys_reset_1/dcm_locked]
  connect_bd_net -net clk_wizard_slash_clk_out1  [get_bd_pins clk_wizard_slash/clk_out1] \
  [get_bd_pins proc_sys_reset_0/slowest_sync_clk] \
  [get_bd_pins slash_clk] \
  [get_bd_pins slash_rst_pipe_slr0/CLK]
  connect_bd_net -net clk_wizard_slash_locked  [get_bd_pins clk_wizard_slash/locked] \
  [get_bd_pins proc_sys_reset_0/dcm_locked]
  connect_bd_net -net pl0_ref_clk_1  [get_bd_pins pl0_ref_clk] \
  [get_bd_pins axi_noc_0/aclk0] \
  [get_bd_pins smartconnect_0/aclk] \
  [get_bd_pins clk_wizard_slash/s_axi_aclk] \
  [get_bd_pins clk_wizard_service/s_axi_aclk]
  connect_bd_net -net pl3_ref_clk_1  [get_bd_pins refclk] \
  [get_bd_pins clk_wizard_slash/clk_in1] \
  [get_bd_pins clk_wizard_service/clk_in1]
  connect_bd_net -net proc_sys_reset_0_peripheral_aresetn  [get_bd_pins proc_sys_reset_0/peripheral_aresetn] \
  [get_bd_pins slash_rst_conv_in/Op1]
  connect_bd_net -net proc_sys_reset_1_peripheral_aresetn  [get_bd_pins proc_sys_reset_1/peripheral_aresetn] \
  [get_bd_pins service_rst_conv_in/Op1]
  connect_bd_net -net service_rst_conv_in_Res  [get_bd_pins service_rst_conv_in/Res] \
  [get_bd_pins service_rst_pipe_slr0/D]
  connect_bd_net -net service_rst_pipe_slr0_Q  [get_bd_pins service_rst_pipe_slr0/Q] \
  [get_bd_pins service_arstn]
  connect_bd_net -net slash_rst_conv_in_Res  [get_bd_pins slash_rst_conv_in/Res] \
  [get_bd_pins slash_rst_pipe_slr0/D]
  connect_bd_net -net slash_rst_pipe_slr0_Q  [get_bd_pins slash_rst_pipe_slr0/Q] \
  [get_bd_pins slash_arstn]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: virt_noc
proc create_hier_cell_virt_noc { parentCell nameHier } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_virt_noc() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI2

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI3

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI4

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI1

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI2

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI3

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI4


  # Create pins

  # Create instance: axi_noc_0, and set properties
  set axi_noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_0


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/virt_noc/axi_noc_0/M00_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
 ] [get_bd_intf_pins /static_region/virt_noc/axi_noc_0/S00_INI]

  # Create instance: axi_noc_1, and set properties
  set axi_noc_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_1 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_1


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/virt_noc/axi_noc_1/M00_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
 ] [get_bd_intf_pins /static_region/virt_noc/axi_noc_1/S00_INI]

  # Create instance: axi_noc_2, and set properties
  set axi_noc_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_2 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_2


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/virt_noc/axi_noc_2/M00_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
 ] [get_bd_intf_pins /static_region/virt_noc/axi_noc_2/S00_INI]

  # Create instance: axi_noc_3, and set properties
  set axi_noc_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_3 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_3


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/virt_noc/axi_noc_3/M00_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
 ] [get_bd_intf_pins /static_region/virt_noc/axi_noc_3/S00_INI]

  # Create instance: axi_noc_4, and set properties
  set axi_noc_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_4 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_4


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/virt_noc/axi_noc_4/M00_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
 ] [get_bd_intf_pins /static_region/virt_noc/axi_noc_4/S00_INI]

  # Create interface connections
  connect_bd_intf_net -intf_net Conn1 [get_bd_intf_pins axi_noc_0/M00_INI] [get_bd_intf_pins M00_INI]
  connect_bd_intf_net -intf_net Conn2 [get_bd_intf_pins axi_noc_0/S00_INI] [get_bd_intf_pins S00_INI]
  connect_bd_intf_net -intf_net Conn3 [get_bd_intf_pins axi_noc_1/S00_INI] [get_bd_intf_pins S00_INI1]
  connect_bd_intf_net -intf_net Conn4 [get_bd_intf_pins axi_noc_2/S00_INI] [get_bd_intf_pins S00_INI2]
  connect_bd_intf_net -intf_net Conn5 [get_bd_intf_pins axi_noc_3/S00_INI] [get_bd_intf_pins S00_INI3]
  connect_bd_intf_net -intf_net Conn6 [get_bd_intf_pins axi_noc_4/S00_INI] [get_bd_intf_pins S00_INI4]
  connect_bd_intf_net -intf_net Conn7 [get_bd_intf_pins axi_noc_1/M00_INI] [get_bd_intf_pins M00_INI1]
  connect_bd_intf_net -intf_net Conn8 [get_bd_intf_pins axi_noc_2/M00_INI] [get_bd_intf_pins M00_INI2]
  connect_bd_intf_net -intf_net Conn9 [get_bd_intf_pins axi_noc_3/M00_INI] [get_bd_intf_pins M00_INI3]
  connect_bd_intf_net -intf_net Conn10 [get_bd_intf_pins axi_noc_4/M00_INI] [get_bd_intf_pins M00_INI4]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: dcmac_noc
proc create_hier_cell_dcmac_noc { parentCell nameHier } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_dcmac_noc() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS1

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS2

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS2

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS3

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS3

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS4

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS4

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS5

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS5

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS6

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS6

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS7

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS7

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS8

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS8

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS9

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS9

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS10

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS10

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS11

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS11

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS12

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS12

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS13

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS13

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS14

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS14

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS15

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS15


  # Create pins

  # Create instance: dcmac_service2slash_0, and set properties
  set dcmac_service2slash_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_service2slash_0 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_service2slash_0


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_0/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_0/S00_INIS]

  # Create instance: dcmac_service2slash_1, and set properties
  set dcmac_service2slash_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_service2slash_1 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_service2slash_1


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_1/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_1/S00_INIS]

  # Create instance: dcmac_service2slash_2, and set properties
  set dcmac_service2slash_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_service2slash_2 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_service2slash_2


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_2/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_2/S00_INIS]

  # Create instance: dcmac_service2slash_3, and set properties
  set dcmac_service2slash_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_service2slash_3 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_service2slash_3


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_3/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_3/S00_INIS]

  # Create instance: dcmac_service2slash_4, and set properties
  set dcmac_service2slash_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_service2slash_4 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_service2slash_4


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_4/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_4/S00_INIS]

  # Create instance: dcmac_service2slash_5, and set properties
  set dcmac_service2slash_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_service2slash_5 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_service2slash_5


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_5/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_5/S00_INIS]

  # Create instance: dcmac_service2slash_6, and set properties
  set dcmac_service2slash_6 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_service2slash_6 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_service2slash_6


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_6/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_6/S00_INIS]

  # Create instance: dcmac_service2slash_7, and set properties
  set dcmac_service2slash_7 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_service2slash_7 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_service2slash_7


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_7/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_service2slash_7/S00_INIS]

  # Create instance: dcmac_slash2service_0, and set properties
  set dcmac_slash2service_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_slash2service_0 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_slash2service_0


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_0/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_0/S00_INIS]

  # Create instance: dcmac_slash2service_1, and set properties
  set dcmac_slash2service_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_slash2service_1 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_slash2service_1


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_1/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_1/S00_INIS]

  # Create instance: dcmac_slash2service_2, and set properties
  set dcmac_slash2service_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_slash2service_2 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_slash2service_2


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_2/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_2/S00_INIS]

  # Create instance: dcmac_slash2service_3, and set properties
  set dcmac_slash2service_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_slash2service_3 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_slash2service_3


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_3/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_3/S00_INIS]

  # Create instance: dcmac_slash2service_4, and set properties
  set dcmac_slash2service_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_slash2service_4 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_slash2service_4


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_4/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_4/S00_INIS]

  # Create instance: dcmac_slash2service_5, and set properties
  set dcmac_slash2service_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_slash2service_5 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_slash2service_5


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_5/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_5/S00_INIS]

  # Create instance: dcmac_slash2service_6, and set properties
  set dcmac_slash2service_6 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_slash2service_6 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_slash2service_6


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_6/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_6/S00_INIS]

  # Create instance: dcmac_slash2service_7, and set properties
  set dcmac_slash2service_7 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_slash2service_7 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_slash2service_7


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_7/M00_INIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {100}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /static_region/dcmac_noc/dcmac_slash2service_7/S00_INIS]

  # Create interface connections
  connect_bd_intf_net -intf_net Conn1 [get_bd_intf_pins dcmac_slash2service_0/S00_INIS] [get_bd_intf_pins S00_INIS]
  connect_bd_intf_net -intf_net Conn2 [get_bd_intf_pins dcmac_slash2service_0/M00_INIS] [get_bd_intf_pins M00_INIS]
  connect_bd_intf_net -intf_net Conn3 [get_bd_intf_pins dcmac_slash2service_1/S00_INIS] [get_bd_intf_pins S00_INIS1]
  connect_bd_intf_net -intf_net Conn4 [get_bd_intf_pins dcmac_slash2service_1/M00_INIS] [get_bd_intf_pins M00_INIS1]
  connect_bd_intf_net -intf_net Conn5 [get_bd_intf_pins dcmac_slash2service_2/S00_INIS] [get_bd_intf_pins S00_INIS2]
  connect_bd_intf_net -intf_net Conn6 [get_bd_intf_pins dcmac_slash2service_2/M00_INIS] [get_bd_intf_pins M00_INIS2]
  connect_bd_intf_net -intf_net Conn7 [get_bd_intf_pins dcmac_slash2service_3/S00_INIS] [get_bd_intf_pins S00_INIS3]
  connect_bd_intf_net -intf_net Conn8 [get_bd_intf_pins dcmac_slash2service_3/M00_INIS] [get_bd_intf_pins M00_INIS3]
  connect_bd_intf_net -intf_net Conn9 [get_bd_intf_pins dcmac_slash2service_4/S00_INIS] [get_bd_intf_pins S00_INIS4]
  connect_bd_intf_net -intf_net Conn10 [get_bd_intf_pins dcmac_slash2service_4/M00_INIS] [get_bd_intf_pins M00_INIS4]
  connect_bd_intf_net -intf_net Conn11 [get_bd_intf_pins dcmac_slash2service_5/S00_INIS] [get_bd_intf_pins S00_INIS5]
  connect_bd_intf_net -intf_net Conn12 [get_bd_intf_pins dcmac_slash2service_5/M00_INIS] [get_bd_intf_pins M00_INIS5]
  connect_bd_intf_net -intf_net Conn13 [get_bd_intf_pins dcmac_slash2service_6/S00_INIS] [get_bd_intf_pins S00_INIS6]
  connect_bd_intf_net -intf_net Conn14 [get_bd_intf_pins dcmac_slash2service_6/M00_INIS] [get_bd_intf_pins M00_INIS6]
  connect_bd_intf_net -intf_net Conn15 [get_bd_intf_pins dcmac_slash2service_7/S00_INIS] [get_bd_intf_pins S00_INIS7]
  connect_bd_intf_net -intf_net Conn16 [get_bd_intf_pins dcmac_slash2service_7/M00_INIS] [get_bd_intf_pins M00_INIS7]
  connect_bd_intf_net -intf_net Conn17 [get_bd_intf_pins dcmac_service2slash_0/S00_INIS] [get_bd_intf_pins S00_INIS8]
  connect_bd_intf_net -intf_net Conn18 [get_bd_intf_pins dcmac_service2slash_0/M00_INIS] [get_bd_intf_pins M00_INIS8]
  connect_bd_intf_net -intf_net Conn19 [get_bd_intf_pins dcmac_service2slash_1/S00_INIS] [get_bd_intf_pins S00_INIS9]
  connect_bd_intf_net -intf_net Conn20 [get_bd_intf_pins dcmac_service2slash_1/M00_INIS] [get_bd_intf_pins M00_INIS9]
  connect_bd_intf_net -intf_net Conn21 [get_bd_intf_pins dcmac_service2slash_2/S00_INIS] [get_bd_intf_pins S00_INIS10]
  connect_bd_intf_net -intf_net Conn22 [get_bd_intf_pins dcmac_service2slash_2/M00_INIS] [get_bd_intf_pins M00_INIS10]
  connect_bd_intf_net -intf_net Conn23 [get_bd_intf_pins dcmac_service2slash_3/S00_INIS] [get_bd_intf_pins S00_INIS11]
  connect_bd_intf_net -intf_net Conn24 [get_bd_intf_pins dcmac_service2slash_3/M00_INIS] [get_bd_intf_pins M00_INIS11]
  connect_bd_intf_net -intf_net Conn25 [get_bd_intf_pins dcmac_service2slash_4/S00_INIS] [get_bd_intf_pins S00_INIS12]
  connect_bd_intf_net -intf_net Conn26 [get_bd_intf_pins dcmac_service2slash_4/M00_INIS] [get_bd_intf_pins M00_INIS12]
  connect_bd_intf_net -intf_net Conn27 [get_bd_intf_pins dcmac_service2slash_5/S00_INIS] [get_bd_intf_pins S00_INIS13]
  connect_bd_intf_net -intf_net Conn28 [get_bd_intf_pins dcmac_service2slash_5/M00_INIS] [get_bd_intf_pins M00_INIS13]
  connect_bd_intf_net -intf_net Conn29 [get_bd_intf_pins dcmac_service2slash_6/S00_INIS] [get_bd_intf_pins S00_INIS14]
  connect_bd_intf_net -intf_net Conn30 [get_bd_intf_pins dcmac_service2slash_6/M00_INIS] [get_bd_intf_pins M00_INIS14]
  connect_bd_intf_net -intf_net Conn31 [get_bd_intf_pins dcmac_service2slash_7/S00_INIS] [get_bd_intf_pins S00_INIS15]
  connect_bd_intf_net -intf_net Conn32 [get_bd_intf_pins dcmac_service2slash_7/M00_INIS] [get_bd_intf_pins M00_INIS15]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: aved
proc create_hier_cell_aved { parentCell nameHier } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_aved() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 gt_pcie_refclk

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 gt_pciea1

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 smbus_0

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 CPM_PCIE_NOC_0

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 CPM_PCIE_NOC_1

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 PMC_NOC_AXI_0

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 LPD_AXI_NOC_0

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 s_axi_pcie_mgmt_slr0

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 NOC_PMC_AXI_0

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 NOC_CPM_PCIE_0


  # Create pins
  create_bd_pin -dir O -type clk pl0_ref_clk
  create_bd_pin -dir O -type clk lpd_axi_noc_clk
  create_bd_pin -dir O -type clk pmc_axi_noc_axi0_clk
  create_bd_pin -dir O -type clk cpm_pcie_noc_axi1_clk
  create_bd_pin -dir O -type clk cpm_pcie_noc_axi0_clk
  create_bd_pin -dir O -from 0 -to 0 -type rst resetn_pl_periph
  create_bd_pin -dir O -type clk noc_pmc_axi_axi0_clk
  create_bd_pin -dir O -type clk pl3_ref_clk
  create_bd_pin -dir O -type rst pl3_resetn
  create_bd_pin -dir O -type clk noc_cpm_pcie_axi0_clk
  create_bd_pin -dir O eos
  create_bd_pin -dir O -type rst pl0_resetn

  # Create instance: clock_reset
  create_hier_cell_clock_reset $hier_obj clock_reset

  # Create instance: base_logic
  create_hier_cell_base_logic $hier_obj base_logic

  # Create instance: cips, and set properties
  set cips [ create_bd_cell -type ip -vlnv xilinx.com:ip:versal_cips:3.4 cips ]
  set_property -dict [list \
    CONFIG.CPM_CONFIG { \
      CPM_PCIE0_MODES {None} \
      CPM_PCIE0_TANDEM {None} \
      CPM_PCIE1_ACS_CAP_ON {0} \
      CPM_PCIE1_ARI_CAP_ENABLED {1} \
      CPM_PCIE1_BRIDGE_AXI_SLAVE_IF {1} \
      CPM_PCIE1_CFG_EXT_IF {1} \
      CPM_PCIE1_CFG_VEND_ID {10ee} \
      CPM_PCIE1_COPY_PF0_QDMA_ENABLED {0} \
      CPM_PCIE1_EXT_PCIE_CFG_SPACE_ENABLED {Extended_Large} \
      CPM_PCIE1_FUNCTIONAL_MODE {QDMA} \
      CPM_PCIE1_MAX_LINK_SPEED {32.0_GT/s} \
      CPM_PCIE1_MODES {DMA} \
      CPM_PCIE1_MODE_SELECTION {Advanced} \
      CPM_PCIE1_MSI_X_OPTIONS {MSI-X_Internal} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_BASEADDR_0 {0x0000008000000000} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_BASEADDR_1 {0x0000008040000000} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_BASEADDR_2 {0x0000008080000000} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_BASEADDR_3 {0x00000080C0000000} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_BASEADDR_4 {0x0000008100000000} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_BASEADDR_5 {0x0000008140000000} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_HIGHADDR_0 {0x000000803FFFFFFFF} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_HIGHADDR_1 {0x000000807FFFFFFFF} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_HIGHADDR_2 {0x00000080BFFFFFFFF} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_HIGHADDR_3 {0x00000080FFFFFFFFF} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_HIGHADDR_4 {0x000000813FFFFFFFF} \
      CPM_PCIE1_PF0_AXIBAR2PCIE_HIGHADDR_5 {0x000000817FFFFFFFF} \
      CPM_PCIE1_PF0_BAR0_QDMA_64BIT {1} \
      CPM_PCIE1_PF0_BAR0_QDMA_ENABLED {1} \
      CPM_PCIE1_PF0_BAR0_QDMA_PREFETCHABLE {1} \
      CPM_PCIE1_PF0_BAR0_QDMA_SCALE {Megabytes} \
      CPM_PCIE1_PF0_BAR0_QDMA_SIZE {256} \
      CPM_PCIE1_PF0_BAR0_QDMA_TYPE {AXI_Bridge_Master} \
      CPM_PCIE1_PF0_BAR2_QDMA_64BIT {0} \
      CPM_PCIE1_PF0_BAR2_QDMA_ENABLED {0} \
      CPM_PCIE1_PF0_BAR2_QDMA_PREFETCHABLE {0} \
      CPM_PCIE1_PF0_BAR2_QDMA_SCALE {Kilobytes} \
      CPM_PCIE1_PF0_BAR2_QDMA_SIZE {4} \
      CPM_PCIE1_PF0_BAR2_QDMA_TYPE {AXI_Bridge_Master} \
      CPM_PCIE1_PF0_BASE_CLASS_VALUE {12} \
      CPM_PCIE1_PF0_CFG_DEV_ID {50b4} \
      CPM_PCIE1_PF0_CFG_SUBSYS_ID {000e} \
      CPM_PCIE1_PF0_DEV_CAP_FUNCTION_LEVEL_RESET_CAPABLE {0} \
      CPM_PCIE1_PF0_MSIX_CAP_TABLE_OFFSET {40} \
      CPM_PCIE1_PF0_MSIX_CAP_TABLE_SIZE {1} \
      CPM_PCIE1_PF0_MSIX_ENABLED {0} \
      CPM_PCIE1_PF0_PCIEBAR2AXIBAR_QDMA_0 {0x0000020100000000} \
      CPM_PCIE1_PF0_SUB_CLASS_VALUE {00} \
      CPM_PCIE1_PF1_BAR0_QDMA_64BIT {1} \
      CPM_PCIE1_PF1_BAR0_QDMA_ENABLED {1} \
      CPM_PCIE1_PF1_BAR0_QDMA_PREFETCHABLE {1} \
      CPM_PCIE1_PF1_BAR0_QDMA_SCALE {Kilobytes} \
      CPM_PCIE1_PF1_BAR0_QDMA_SIZE {512} \
      CPM_PCIE1_PF1_BAR0_QDMA_TYPE {DMA} \
      CPM_PCIE1_PF1_BAR2_QDMA_64BIT {0} \
      CPM_PCIE1_PF1_BAR2_QDMA_ENABLED {0} \
      CPM_PCIE1_PF1_BAR2_QDMA_PREFETCHABLE {0} \
      CPM_PCIE1_PF1_BAR2_QDMA_SCALE {Kilobytes} \
      CPM_PCIE1_PF1_BAR2_QDMA_SIZE {4} \
      CPM_PCIE1_PF1_BAR2_QDMA_TYPE {AXI_Bridge_Master} \
      CPM_PCIE1_PF1_BASE_CLASS_VALUE {12} \
      CPM_PCIE1_PF1_CFG_DEV_ID {50b5} \
      CPM_PCIE1_PF1_CFG_SUBSYS_ID {000e} \
      CPM_PCIE1_PF1_CFG_SUBSYS_VEND_ID {10EE} \
      CPM_PCIE1_PF1_MSIX_CAP_TABLE_OFFSET {50000} \
      CPM_PCIE1_PF1_MSIX_CAP_TABLE_SIZE {8} \
      CPM_PCIE1_PF1_MSIX_ENABLED {1} \
      CPM_PCIE1_PF1_PCIEBAR2AXIBAR_QDMA_2 {0x0000020200000000} \
      CPM_PCIE1_PF1_SUB_CLASS_VALUE {00} \
      CPM_PCIE1_PF2_BAR0_QDMA_64BIT {1} \
      CPM_PCIE1_PF2_BAR0_QDMA_SCALE {Megabytes} \
      CPM_PCIE1_PF2_BAR0_QDMA_SIZE {128} \
      CPM_PCIE1_PF2_BAR0_QDMA_TYPE {AXI_Bridge_Master} \
      CPM_PCIE1_PF2_BAR2_QDMA_64BIT {1} \
      CPM_PCIE1_PF2_BAR2_QDMA_ENABLED {1} \
      CPM_PCIE1_PF2_BAR2_QDMA_SCALE {Megabytes} \
      CPM_PCIE1_PF2_BAR2_QDMA_SIZE {128} \
      CPM_PCIE1_PF2_BAR2_QDMA_TYPE {AXI_Bridge_Master} \
      CPM_PCIE1_PF2_BAR3_QDMA_ENABLED {0} \
      CPM_PCIE1_PF2_BAR3_QDMA_SIZE {4} \
      CPM_PCIE1_PF2_BAR4_QDMA_64BIT {1} \
      CPM_PCIE1_PF2_BAR4_QDMA_ENABLED {1} \
      CPM_PCIE1_PF2_BAR4_QDMA_SIZE {512} \
      CPM_PCIE1_PF2_BASE_CLASS_VALUE {12} \
      CPM_PCIE1_PF2_CFG_DEV_ID {50b6} \
      CPM_PCIE1_PF2_CFG_SUBSYS_ID {000e} \
      CPM_PCIE1_PF2_CFG_SUBSYS_VEND_ID {10EE} \
      CPM_PCIE1_PF2_PCIEBAR2AXIBAR_QDMA_0 {0x0000020200000000} \
      CPM_PCIE1_PF2_PCIEBAR2AXIBAR_QDMA_2 {0x0000020300000000} \
      CPM_PCIE1_PF2_PCIEBAR2AXIBAR_QDMA_4 {0x0000020400000000} \
      CPM_PCIE1_PF2_USE_CLASS_CODE_LOOKUP_ASSISTANT {0} \
      CPM_PCIE1_PL_LINK_CAP_MAX_LINK_WIDTH {X8} \
      CPM_PCIE1_TL_PF_ENABLE_REG {3} \
    } \
    CONFIG.PS_PMC_CONFIG { \
      BOOT_MODE {Custom} \
      CLOCK_MODE {Custom} \
      DDR_MEMORY_MODE {Custom} \
      DESIGN_MODE {1} \
      DEVICE_INTEGRITY_MODE {Custom} \
      IO_CONFIG_MODE {Custom} \
      PCIE_APERTURES_DUAL_ENABLE {0} \
      PCIE_APERTURES_SINGLE_ENABLE {1} \
      PMC_BANK_1_IO_STANDARD {LVCMOS3.3} \
      PMC_CRP_OSPI_REF_CTRL_FREQMHZ {200} \
      PMC_CRP_PL0_REF_CTRL_FREQMHZ {100} \
      PMC_CRP_PL1_REF_CTRL_FREQMHZ {33.3333333} \
      PMC_CRP_PL2_REF_CTRL_FREQMHZ {250} \
      PMC_CRP_PL3_REF_CTRL_FREQMHZ {100} \
      PMC_GLITCH_CONFIG {{DEPTH_SENSITIVITY 1} {MIN_PULSE_WIDTH 0.5} {TYPE CUSTOM} {VCC_PMC_VALUE 0.88}} \
      PMC_GLITCH_CONFIG_1 {{DEPTH_SENSITIVITY 1} {MIN_PULSE_WIDTH 0.5} {TYPE CUSTOM} {VCC_PMC_VALUE 0.88}} \
      PMC_GLITCH_CONFIG_2 {{DEPTH_SENSITIVITY 1} {MIN_PULSE_WIDTH 0.5} {TYPE CUSTOM} {VCC_PMC_VALUE 0.88}} \
      PMC_GPIO_EMIO_PERIPHERAL_ENABLE {0} \
      PMC_MIO11 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO12 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO13 {{AUX_IO 0} {DIRECTION inout} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE Reserved}} \
      PMC_MIO17 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO26 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO27 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO28 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO29 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO30 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO31 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO32 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO33 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO34 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO35 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO36 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO37 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO38 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO39 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO40 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO41 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO42 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO43 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO44 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO48 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO49 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO50 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO51 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PMC_MIO_EN_FOR_PL_PCIE {0} \
      PMC_OSPI_PERIPHERAL {{ENABLE 1} {IO {PMC_MIO 0 .. 11}} {MODE Single}} \
      PMC_REF_CLK_FREQMHZ {33.333333} \
      PMC_SD0_DATA_TRANSFER_MODE {8Bit} \
      PMC_SD0_PERIPHERAL {{CLK_100_SDR_OTAP_DLY 0x00} {CLK_200_SDR_OTAP_DLY 0x2} {CLK_50_DDR_ITAP_DLY 0x1E} {CLK_50_DDR_OTAP_DLY 0x5} {CLK_50_SDR_ITAP_DLY 0x2C} {CLK_50_SDR_OTAP_DLY 0x5} {ENABLE 1} {IO\
{PMC_MIO 13 .. 25}}} \
      PMC_SD0_SLOT_TYPE {eMMC} \
      PMC_USE_NOC_PMC_AXI0 {1} \
      PMC_USE_PMC_NOC_AXI0 {1} \
      PS_BANK_2_IO_STANDARD {LVCMOS3.3} \
      PS_BOARD_INTERFACE {Custom} \
      PS_CRL_CPM_TOPSW_REF_CTRL_FREQMHZ {1000} \
      PS_GEN_IPI0_ENABLE {0} \
      PS_GEN_IPI1_ENABLE {0} \
      PS_GEN_IPI2_ENABLE {0} \
      PS_GEN_IPI3_ENABLE {1} \
      PS_GEN_IPI3_MASTER {R5_0} \
      PS_GEN_IPI4_ENABLE {1} \
      PS_GEN_IPI4_MASTER {R5_0} \
      PS_GEN_IPI5_ENABLE {1} \
      PS_GEN_IPI5_MASTER {R5_1} \
      PS_GEN_IPI6_ENABLE {1} \
      PS_GEN_IPI6_MASTER {R5_1} \
      PS_GPIO_EMIO_PERIPHERAL_ENABLE {0} \
      PS_I2C0_PERIPHERAL {{ENABLE 1} {IO {PS_MIO 2 .. 3}}} \
      PS_I2C1_PERIPHERAL {{ENABLE 1} {IO {PS_MIO 0 .. 1}}} \
      PS_IRQ_USAGE {{CH0 1} {CH1 1} {CH10 0} {CH11 0} {CH12 0} {CH13 0} {CH14 0} {CH15 0} {CH2 0} {CH3 0} {CH4 0} {CH5 0} {CH6 0} {CH7 0} {CH8 0} {CH9 0}} \
      PS_KAT_ENABLE {0} \
      PS_KAT_ENABLE_1 {0} \
      PS_KAT_ENABLE_2 {0} \
      PS_MIO10 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO11 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO12 {{AUX_IO 0} {DIRECTION inout} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE Reserved}} \
      PS_MIO13 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO14 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO18 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO19 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO22 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO23 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO24 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO25 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO4 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO5 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO6 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO7 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE GPIO}} \
      PS_MIO8 {{AUX_IO 0} {DIRECTION in} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 0} {SLEW slow} {USAGE Reserved}} \
      PS_MIO9 {{AUX_IO 0} {DIRECTION out} {DRIVE_STRENGTH 8mA} {OUTPUT_DATA default} {PULL pullup} {SCHMITT 1} {SLEW slow} {USAGE Reserved}} \
      PS_M_AXI_LPD_DATA_WIDTH {32} \
      PS_NUM_FABRIC_RESETS {4} \
      PS_PCIE1_PERIPHERAL_ENABLE {0} \
      PS_PCIE2_PERIPHERAL_ENABLE {1} \
      PS_PCIE_EP_RESET1_IO {PMC_MIO 24} \
      PS_PCIE_EP_RESET2_IO {PMC_MIO 25} \
      PS_PCIE_RESET {ENABLE 1} \
      PS_PL_CONNECTIVITY_MODE {Custom} \
      PS_SPI0 {{GRP_SS0_ENABLE 1} {GRP_SS0_IO {PS_MIO 15}} {GRP_SS1_ENABLE 0} {GRP_SS1_IO {PMC_MIO 14}} {GRP_SS2_ENABLE 0} {GRP_SS2_IO {PMC_MIO 13}} {PERIPHERAL_ENABLE 1} {PERIPHERAL_IO {PS_MIO 12 .. 17}}}\
\
      PS_SPI1 {{GRP_SS0_ENABLE 0} {GRP_SS0_IO {PS_MIO 9}} {GRP_SS1_ENABLE 0} {GRP_SS1_IO {PS_MIO 8}} {GRP_SS2_ENABLE 0} {GRP_SS2_IO {PS_MIO 7}} {PERIPHERAL_ENABLE 0} {PERIPHERAL_IO {PS_MIO 6 .. 11}}} \
      PS_TTC0_PERIPHERAL_ENABLE {1} \
      PS_TTC1_PERIPHERAL_ENABLE {1} \
      PS_TTC2_PERIPHERAL_ENABLE {1} \
      PS_TTC3_PERIPHERAL_ENABLE {1} \
      PS_UART0_PERIPHERAL {{ENABLE 1} {IO {PS_MIO 8 .. 9}}} \
      PS_UART1_PERIPHERAL {{ENABLE 1} {IO {PS_MIO 20 .. 21}}} \
      PS_USE_FPD_CCI_NOC {0} \
      PS_USE_M_AXI_FPD {0} \
      PS_USE_M_AXI_LPD {1} \
      PS_USE_NOC_LPD_AXI0 {1} \
      PS_USE_PMCPL_CLK0 {1} \
      PS_USE_PMCPL_CLK1 {1} \
      PS_USE_PMCPL_CLK2 {1} \
      PS_USE_PMCPL_CLK3 {1} \
      PS_USE_STARTUP {1} \
      PS_USE_S_AXI_LPD {0} \
      SMON_ALARMS {Set_Alarms_On} \
      SMON_ENABLE_TEMP_AVERAGING {0} \
      SMON_MEAS100 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 4.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {4 V unipolar}} {NAME VCCO_500} {SUPPLY_NUM 9}} \
      SMON_MEAS101 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 4.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {4 V unipolar}} {NAME VCCO_501} {SUPPLY_NUM 10}} \
      SMON_MEAS102 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 4.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {4 V unipolar}} {NAME VCCO_502} {SUPPLY_NUM 11}} \
      SMON_MEAS103 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 4.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {4 V unipolar}} {NAME VCCO_503} {SUPPLY_NUM 12}} \
      SMON_MEAS104 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCCO_700} {SUPPLY_NUM 13}} \
      SMON_MEAS105 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCCO_701} {SUPPLY_NUM 14}} \
      SMON_MEAS106 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCCO_702} {SUPPLY_NUM 15}} \
      SMON_MEAS118 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCC_PMC} {SUPPLY_NUM 0}} \
      SMON_MEAS119 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCC_PSFP} {SUPPLY_NUM 1}} \
      SMON_MEAS120 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCC_PSLP} {SUPPLY_NUM 2}} \
      SMON_MEAS121 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCC_RAM} {SUPPLY_NUM 3}} \
      SMON_MEAS122 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCC_SOC} {SUPPLY_NUM 4}} \
      SMON_MEAS47 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME GTYP_AVCCAUX_104} {SUPPLY_NUM 20}} \
      SMON_MEAS48 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME GTYP_AVCCAUX_105} {SUPPLY_NUM 21}} \
      SMON_MEAS64 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME GTYP_AVCC_104} {SUPPLY_NUM 18}} \
      SMON_MEAS65 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME GTYP_AVCC_105} {SUPPLY_NUM 19}} \
      SMON_MEAS81 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME GTYP_AVTT_104} {SUPPLY_NUM 22}} \
      SMON_MEAS82 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME GTYP_AVTT_105} {SUPPLY_NUM 23}} \
      SMON_MEAS96 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCCAUX} {SUPPLY_NUM 6}} \
      SMON_MEAS97 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCCAUX_PMC} {SUPPLY_NUM 7}} \
      SMON_MEAS98 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCCAUX_SMON} {SUPPLY_NUM 8}} \
      SMON_MEAS99 {{ALARM_ENABLE 1} {ALARM_LOWER 0.00} {ALARM_UPPER 2.00} {AVERAGE_EN 0} {ENABLE 1} {MODE {2 V unipolar}} {NAME VCCINT} {SUPPLY_NUM 5}} \
      SMON_TEMP_AVERAGING_SAMPLES {0} \
      SMON_VOLTAGE_AVERAGING_SAMPLES {8} \
    } \
    CONFIG.PS_PMC_CONFIG_APPLIED {1} \
  ] $cips


  # Create interface connections
  connect_bd_intf_net -intf_net Conn1 [get_bd_intf_pins cips/gt_refclk1] [get_bd_intf_pins gt_pcie_refclk]
  connect_bd_intf_net -intf_net Conn2 [get_bd_intf_pins cips/PCIE1_GT] [get_bd_intf_pins gt_pciea1]
  connect_bd_intf_net -intf_net Conn3 [get_bd_intf_pins base_logic/smbus_rpu] [get_bd_intf_pins smbus_0]
  connect_bd_intf_net -intf_net Conn4 [get_bd_intf_pins cips/CPM_PCIE_NOC_0] [get_bd_intf_pins CPM_PCIE_NOC_0]
  connect_bd_intf_net -intf_net Conn5 [get_bd_intf_pins cips/CPM_PCIE_NOC_1] [get_bd_intf_pins CPM_PCIE_NOC_1]
  connect_bd_intf_net -intf_net Conn6 [get_bd_intf_pins cips/PMC_NOC_AXI_0] [get_bd_intf_pins PMC_NOC_AXI_0]
  connect_bd_intf_net -intf_net Conn7 [get_bd_intf_pins cips/LPD_AXI_NOC_0] [get_bd_intf_pins LPD_AXI_NOC_0]
  connect_bd_intf_net -intf_net Conn8 [get_bd_intf_pins base_logic/s_axi_pcie_mgmt_slr0] [get_bd_intf_pins s_axi_pcie_mgmt_slr0]
  connect_bd_intf_net -intf_net NOC_CPM_PCIE_0_1 [get_bd_intf_pins NOC_CPM_PCIE_0] [get_bd_intf_pins cips/NOC_CPM_PCIE_0]
  connect_bd_intf_net -intf_net NOC_PMC_AXI_0_1 [get_bd_intf_pins NOC_PMC_AXI_0] [get_bd_intf_pins cips/NOC_PMC_AXI_0]
  connect_bd_intf_net -intf_net base_logic_m_axi_pcie_mgmt_pdi_reset [get_bd_intf_pins base_logic/m_axi_pcie_mgmt_pdi_reset] [get_bd_intf_pins clock_reset/s_axi_pcie_mgmt_pdi_reset]
  connect_bd_intf_net -intf_net cips_M_AXI_LPD [get_bd_intf_pins cips/M_AXI_LPD] [get_bd_intf_pins base_logic/s_axi_rpu]
  connect_bd_intf_net -intf_net cips_pcie1_cfg_ext [get_bd_intf_pins cips/pcie1_cfg_ext] [get_bd_intf_pins base_logic/pcie_cfg_ext]

  # Create port connections
  connect_bd_net -net base_logic_irq_axi_smbus_rpu  [get_bd_pins base_logic/irq_axi_smbus_rpu] \
  [get_bd_pins cips/pl_ps_irq1]
  connect_bd_net -net base_logic_irq_gcq_m2r  [get_bd_pins base_logic/irq_gcq_m2r] \
  [get_bd_pins cips/pl_ps_irq0]
  connect_bd_net -net cips_cpm_pcie_noc_axi0_clk  [get_bd_pins cips/cpm_pcie_noc_axi0_clk] \
  [get_bd_pins cpm_pcie_noc_axi0_clk]
  connect_bd_net -net cips_cpm_pcie_noc_axi1_clk  [get_bd_pins cips/cpm_pcie_noc_axi1_clk] \
  [get_bd_pins cpm_pcie_noc_axi1_clk]
  connect_bd_net -net cips_dma1_axi_aresetn  [get_bd_pins cips/dma1_axi_aresetn] \
  [get_bd_pins clock_reset/dma_axi_aresetn]
  connect_bd_net -net cips_eos  [get_bd_pins cips/eos] \
  [get_bd_pins eos]
  connect_bd_net -net cips_lpd_axi_noc_clk  [get_bd_pins cips/lpd_axi_noc_clk] \
  [get_bd_pins lpd_axi_noc_clk]
  connect_bd_net -net cips_noc_cpm_pcie_axi0_clk  [get_bd_pins cips/noc_cpm_pcie_axi0_clk] \
  [get_bd_pins noc_cpm_pcie_axi0_clk]
  connect_bd_net -net cips_noc_pmc_axi_axi0_clk  [get_bd_pins cips/noc_pmc_axi_axi0_clk] \
  [get_bd_pins noc_pmc_axi_axi0_clk]
  connect_bd_net -net cips_pl0_ref_clk  [get_bd_pins cips/pl0_ref_clk] \
  [get_bd_pins pl0_ref_clk] \
  [get_bd_pins cips/m_axi_lpd_aclk] \
  [get_bd_pins base_logic/clk_pl] \
  [get_bd_pins clock_reset/clk_pl]
  connect_bd_net -net cips_pl0_resetn  [get_bd_pins cips/pl0_resetn] \
  [get_bd_pins clock_reset/resetn_pl_axi] \
  [get_bd_pins pl0_resetn]
  connect_bd_net -net cips_pl1_ref_clk  [get_bd_pins cips/pl1_ref_clk] \
  [get_bd_pins clock_reset/clk_freerun]
  connect_bd_net -net cips_pl2_ref_clk  [get_bd_pins cips/pl2_ref_clk] \
  [get_bd_pins cips/dma1_intrfc_clk] \
  [get_bd_pins base_logic/clk_pcie] \
  [get_bd_pins clock_reset/clk_pcie]
  connect_bd_net -net cips_pl3_ref_clk  [get_bd_pins cips/pl3_ref_clk] \
  [get_bd_pins pl3_ref_clk]
  connect_bd_net -net cips_pl3_resetn  [get_bd_pins cips/pl3_resetn] \
  [get_bd_pins pl3_resetn]
  connect_bd_net -net cips_pmc_axi_noc_axi0_clk  [get_bd_pins cips/pmc_axi_noc_axi0_clk] \
  [get_bd_pins pmc_axi_noc_axi0_clk]
  connect_bd_net -net clock_reset_resetn_pcie_ic  [get_bd_pins clock_reset/resetn_pcie_ic] \
  [get_bd_pins cips/dma1_intrfc_resetn]
  connect_bd_net -net clock_reset_resetn_pcie_periph  [get_bd_pins clock_reset/resetn_pcie_periph] \
  [get_bd_pins base_logic/resetn_pcie_periph]
  connect_bd_net -net clock_reset_resetn_pl_ic  [get_bd_pins clock_reset/resetn_pl_ic] \
  [get_bd_pins base_logic/resetn_pl_ic]
  connect_bd_net -net clock_reset_resetn_pl_periph  [get_bd_pins clock_reset/resetn_pl_periph] \
  [get_bd_pins base_logic/resetn_pl_periph] \
  [get_bd_pins resetn_pl_periph]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: noc
proc create_hier_cell_noc { parentCell nameHier } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_noc() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:ddr4_rtl:1.0 CH0_DDR4_0_1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 sys_clk0_1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 sys_clk0_0

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:ddr4_rtl:1.0 CH0_DDR4_0_0

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S03_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 hbm_ref_clk_0

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 hbm_ref_clk_1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S01_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S00_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S02_AXI

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M00_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM63_AXI

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M02_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S01_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S02_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S03_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S04_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S05_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S06_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S07_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S08_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S09_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S10_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S11_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S12_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S13_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S14_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S15_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S16_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S17_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S18_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S19_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S20_INI1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S21_INI1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S22_INI1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S23_INI1

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M05_INI
  set_property APERTURES {{0x203_0000_0000 128M}} [get_bd_intf_pins /static_region/noc/M05_INI]

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M04_INI

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M06_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM00_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM01_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM02_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM03_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM04_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM05_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM06_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM07_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM08_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM09_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM10_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM11_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM12_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM13_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM14_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM15_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM16_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM17_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM18_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM19_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM20_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM21_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM22_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM23_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM24_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM25_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM26_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM27_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM28_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM29_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM30_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM31_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM32_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM33_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM34_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM35_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM36_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM37_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM38_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM39_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM40_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM41_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM42_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM43_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM44_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM45_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM46_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM47_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM48_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM49_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM50_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM51_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM52_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM53_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM54_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM55_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM56_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM57_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM58_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM59_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM60_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM61_AXI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM62_AXI


  # Create pins
  create_bd_pin -dir I -type clk aclk0
  create_bd_pin -dir I -type clk aclk3
  create_bd_pin -dir I -type clk aclk1
  create_bd_pin -dir I -type clk aclk2
  create_bd_pin -dir I -type clk aclk4
  create_bd_pin -dir I -type clk aclk5
  create_bd_pin -dir I -type clk aclk6

  # Create instance: axi_noc_mc_ddr4_0, and set properties
  set axi_noc_mc_ddr4_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_mc_ddr4_0 ]
  set_property -dict [list \
    CONFIG.CONTROLLERTYPE {DDR4_SDRAM} \
    CONFIG.MC_CHAN_REGION1 {DDR_CH1} \
    CONFIG.MC_COMPONENT_WIDTH {x16} \
    CONFIG.MC_DATAWIDTH {72} \
    CONFIG.MC_DM_WIDTH {9} \
    CONFIG.MC_DQS_WIDTH {9} \
    CONFIG.MC_DQ_WIDTH {72} \
    CONFIG.MC_INIT_MEM_USING_ECC_SCRUB {true} \
    CONFIG.MC_INPUTCLK0_PERIOD {5000} \
    CONFIG.MC_MEMORY_DEVICETYPE {Components} \
    CONFIG.MC_MEMORY_SPEEDGRADE {DDR4-3200AA(22-22-22)} \
    CONFIG.MC_NO_CHANNELS {Single} \
    CONFIG.MC_RANK {1} \
    CONFIG.MC_ROWADDRESSWIDTH {16} \
    CONFIG.MC_STACKHEIGHT {1} \
    CONFIG.MC_SYSTEM_CLOCK {Differential} \
    CONFIG.NUM_CLKS {0} \
    CONFIG.NUM_MC {1} \
    CONFIG.NUM_MCP {4} \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {0} \
    CONFIG.NUM_NSI {2} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_mc_ddr4_0


  set_property -dict [ list \
   CONFIG.CONNECTIONS { MC_0 {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64} } } \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_mc_ddr4_0/S00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS { MC_1 {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64} } } \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_mc_ddr4_0/S01_INI]

  # Create instance: axi_noc_cips, and set properties
  set axi_noc_cips [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_cips ]
  set_property -dict [list \
    CONFIG.HBM_CHNL0_CONFIG {HBM_REORDER_EN FALSE HBM_MAINTAIN_COHERENCY TRUE HBM_Q_AGE_LIMIT 0x7F HBM_CLOSE_PAGE_REORDER FALSE HBM_LOOKAHEAD_PCH TRUE HBM_COMMAND_PARITY FALSE HBM_DQ_WR_PARITY FALSE HBM_DQ_RD_PARITY\
FALSE HBM_RD_DBI TRUE HBM_WR_DBI TRUE HBM_REFRESH_MODE SINGLE_BANK_REFRESH HBM_PC0_PRE_DEFINED_ADDRESS_MAP USER_DEFINED_ADDRESS_MAP HBM_PC1_PRE_DEFINED_ADDRESS_MAP USER_DEFINED_ADDRESS_MAP HBM_PC0_USER_DEFINED_ADDRESS_MAP\
1BG-15RA-1SID-2BA-5CA-1BG HBM_PC1_USER_DEFINED_ADDRESS_MAP 1BG-15RA-1SID-2BA-5CA-1BG HBM_PC0_ADDRESS_MAP BA3,RA14,RA13,RA12,RA11,RA10,RA9,RA8,RA7,RA6,RA5,RA4,RA3,RA2,RA1,RA0,SID,BA1,BA0,CA5,CA4,CA3,CA2,CA1,BA2,NC,NA,NA,NA,NA\
HBM_PC1_ADDRESS_MAP BA3,RA14,RA13,RA12,RA11,RA10,RA9,RA8,RA7,RA6,RA5,RA4,RA3,RA2,RA1,RA0,SID,BA1,BA0,CA5,CA4,CA3,CA2,CA1,BA2,NC,NA,NA,NA,NA HBM_PWR_DWN_IDLE_TIMEOUT_ENTRY FALSE HBM_SELF_REF_IDLE_TIMEOUT_ENTRY\
FALSE HBM_IDLE_TIME_TO_ENTER_PWR_DWN_MODE 0x0001000 HBM_IDLE_TIME_TO_ENTER_SELF_REF_MODE 1X HBM_ECC_CORRECTION_EN FALSE HBM_WRITE_BACK_CORRECTED_DATA TRUE HBM_ECC_SCRUBBING FALSE HBM_ECC_INITIALIZE_EN\
FALSE HBM_ECC_SCRUB_SIZE 1092 HBM_WRITE_DATA_MASK TRUE HBM_REF_PERIOD_TEMP_COMP FALSE HBM_PARITY_LATENCY 3 HBM_PC0_PAGE_HIT 100.000 HBM_PC1_PAGE_HIT 100.000 HBM_PC0_READ_RATE 25.000 HBM_PC1_READ_RATE 25.000\
HBM_PC0_WRITE_RATE 25.000 HBM_PC1_WRITE_RATE 25.000 HBM_PC0_PHY_ACTIVE ENABLED HBM_PC1_PHY_ACTIVE ENABLED HBM_PC0_SCRUB_START_ADDRESS 0x0000000 HBM_PC0_SCRUB_END_ADDRESS 0x3FFFBFF HBM_PC0_SCRUB_INTERVAL\
24.000 HBM_PC1_SCRUB_START_ADDRESS 0x0000000 HBM_PC1_SCRUB_END_ADDRESS 0x3FFFBFF HBM_PC1_SCRUB_INTERVAL 24.000} \
    CONFIG.HBM_NUM_CHNL {16} \
    CONFIG.HBM_REF_CLK_FREQ0 {200.000} \
    CONFIG.HBM_REF_CLK_FREQ1 {200.000} \
    CONFIG.HBM_REF_CLK_SELECTION {External} \
    CONFIG.NUM_CLKS {7} \
    CONFIG.NUM_HBM_BLI {64} \
    CONFIG.NUM_MI {2} \
    CONFIG.NUM_NMI {7} \
    CONFIG.NUM_NSI {24} \
    CONFIG.NUM_SI {4} \
    CONFIG.SI_SIDEBAND_PINS { ,0,0,0} \
  ] $axi_noc_cips


  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X2Y0} \
   CONFIG.CONNECTIONS {HBM0_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM00_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X3Y0} \
   CONFIG.CONNECTIONS {HBM0_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM01_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X1Y0} \
   CONFIG.CONNECTIONS {HBM0_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM02_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X0Y0} \
   CONFIG.CONNECTIONS {HBM0_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM03_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X4Y0} \
   CONFIG.CONNECTIONS {HBM1_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM04_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X5Y0} \
   CONFIG.CONNECTIONS {HBM1_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM05_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X6Y0} \
   CONFIG.CONNECTIONS {HBM1_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM06_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X7Y0} \
   CONFIG.CONNECTIONS {HBM1_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM07_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X10Y0} \
   CONFIG.CONNECTIONS {HBM2_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM08_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X11Y0} \
   CONFIG.CONNECTIONS {HBM2_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM09_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X8Y0} \
   CONFIG.CONNECTIONS {HBM2_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM10_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X9Y0} \
   CONFIG.CONNECTIONS {HBM2_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM11_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X12Y0} \
   CONFIG.CONNECTIONS {HBM3_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM12_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X13Y0} \
   CONFIG.CONNECTIONS {HBM3_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM13_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X15Y0} \
   CONFIG.CONNECTIONS {HBM3_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM14_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X14Y0} \
   CONFIG.CONNECTIONS {HBM3_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM15_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X19Y0} \
   CONFIG.CONNECTIONS {HBM4_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM16_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X18Y0} \
   CONFIG.CONNECTIONS {HBM4_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM17_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X17Y0} \
   CONFIG.CONNECTIONS {HBM4_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM18_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X16Y0} \
   CONFIG.CONNECTIONS {HBM4_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM19_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X20Y0} \
   CONFIG.CONNECTIONS {HBM5_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM20_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X21Y0} \
   CONFIG.CONNECTIONS {HBM5_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM21_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X22Y0} \
   CONFIG.CONNECTIONS {HBM5_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM22_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X23Y0} \
   CONFIG.CONNECTIONS {HBM5_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM23_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X27Y0} \
   CONFIG.CONNECTIONS {HBM6_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM24_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X26Y0} \
   CONFIG.CONNECTIONS {HBM6_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM25_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X24Y0} \
   CONFIG.CONNECTIONS {HBM6_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM26_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X25Y0} \
   CONFIG.CONNECTIONS {HBM6_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM27_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X29Y0} \
   CONFIG.CONNECTIONS {HBM7_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM28_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X28Y0} \
   CONFIG.CONNECTIONS {HBM7_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM29_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X31Y0} \
   CONFIG.CONNECTIONS {HBM7_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM30_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X30Y0} \
   CONFIG.CONNECTIONS {HBM7_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM31_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X35Y0} \
   CONFIG.CONNECTIONS {HBM8_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM32_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X34Y0} \
   CONFIG.CONNECTIONS {HBM8_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM33_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X33Y0} \
   CONFIG.CONNECTIONS {HBM8_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM34_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X32Y0} \
   CONFIG.CONNECTIONS {HBM8_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM35_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X37Y0} \
   CONFIG.CONNECTIONS {HBM9_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM36_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X36Y0} \
   CONFIG.CONNECTIONS {HBM9_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM37_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X39Y0} \
   CONFIG.CONNECTIONS {HBM9_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM38_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X38Y0} \
   CONFIG.CONNECTIONS {HBM9_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM39_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X43Y0} \
   CONFIG.CONNECTIONS {HBM10_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM40_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X42Y0} \
   CONFIG.CONNECTIONS {HBM10_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM41_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X41Y0} \
   CONFIG.CONNECTIONS {HBM10_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM42_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X40Y0} \
   CONFIG.CONNECTIONS {HBM10_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM43_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X44Y0} \
   CONFIG.CONNECTIONS {HBM11_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM44_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X45Y0} \
   CONFIG.CONNECTIONS {HBM11_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM45_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X47Y0} \
   CONFIG.CONNECTIONS {HBM11_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM46_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X46Y0} \
   CONFIG.CONNECTIONS {HBM11_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM47_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X51Y0} \
   CONFIG.CONNECTIONS {HBM12_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM48_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X50Y0} \
   CONFIG.CONNECTIONS {HBM12_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM49_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X48Y0} \
   CONFIG.CONNECTIONS {HBM12_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM50_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X49Y0} \
   CONFIG.CONNECTIONS {HBM12_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM51_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X52Y0} \
   CONFIG.CONNECTIONS {HBM13_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM52_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X53Y0} \
   CONFIG.CONNECTIONS {HBM13_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM53_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X55Y0} \
   CONFIG.CONNECTIONS {HBM13_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM54_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X54Y0} \
   CONFIG.CONNECTIONS {HBM13_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM55_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X58Y0} \
   CONFIG.CONNECTIONS {HBM14_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM56_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X59Y0} \
   CONFIG.CONNECTIONS {HBM14_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM57_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X57Y0} \
   CONFIG.CONNECTIONS {HBM14_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM58_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X56Y0} \
   CONFIG.CONNECTIONS {HBM14_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM59_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X61Y0} \
   CONFIG.CONNECTIONS {HBM15_PORT0 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM60_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X60Y0} \
   CONFIG.CONNECTIONS {HBM15_PORT1 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM61_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X63Y0} \
   CONFIG.CONNECTIONS {HBM15_PORT2 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM62_AXI]

  set_property -dict [ list \
   CONFIG.PHYSICAL_LOC {NOC_NMU_HBM2E_X62Y0} \
   CONFIG.CONNECTIONS {HBM15_PORT3 {read_bw {2000} write_bw {2000} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl_hbm} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/HBM63_AXI]

  set_property -dict [ list \
   CONFIG.DATA_WIDTH {32} \
   CONFIG.APERTURES {{0x201_0000_0000 0x200_0000}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/M00_AXI]

  set_property -dict [ list \
   CONFIG.DATA_WIDTH {128} \
   CONFIG.CATEGORY {ps_pmc} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/M01_AXI]

  set_property -dict [ list \
   CONFIG.APERTURES {{0x202_0000_0000 0x100_0000}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/M04_INI]

  set_property -dict [ list \
   CONFIG.APERTURES {{0x203_0000_0000 0x40_0000}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/M05_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {HBM10_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M02_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}} HBM15_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM10_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M01_AXI {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M06_INI {read_bw {500} write_bw {500}} HBM12_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M04_INI {read_bw {500} write_bw {500}} HBM2_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M05_INI {read_bw {500} write_bw {500}} HBM11_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M00_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}} HBM9_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M00_AXI {read_bw {5} write_bw {5} read_avg_burst {64} write_avg_burst {64}}} \
   CONFIG.DEST_IDS {M01_AXI:0x1:M00_AXI:0xd00} \
   CONFIG.REMAPS {M00_INI {{0x20108000000 0x00038000000 0x08000000}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {ps_pcie} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S00_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {M02_INI {read_bw {800} write_bw {800}} M00_INI {read_bw {800} write_bw {800}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {HBM10_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M01_AXI {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM10_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M01_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}} HBM0_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M06_INI {read_bw {500} write_bw {500}} HBM6_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M04_INI {read_bw {500} write_bw {500}} M05_INI {read_bw {500} write_bw {500}} HBM9_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M03_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}} HBM2_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} M00_AXI {read_bw {5} write_bw {5} read_avg_burst {64} write_avg_burst {64}} HBM13_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {M01_AXI:0x1:M00_AXI:0xd00} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {ps_pcie} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S01_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {M01_INI {read_bw {800} write_bw {800}} M03_INI {read_bw {800} write_bw {800}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S01_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M02_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}} M00_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {ps_pmc} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S02_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {M02_INI {read_bw {800} write_bw {800}} M00_INI {read_bw {800} write_bw {800}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S02_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {ps_rpu} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S03_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {M01_INI {read_bw {800} write_bw {800}} M03_INI {read_bw {800} write_bw {800}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S03_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {HBM10_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM10_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S04_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {HBM10_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM10_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S05_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {HBM10_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM10_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S06_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {HBM10_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM10_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S07_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {HBM10_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM10_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S08_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {HBM10_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM10_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S09_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {HBM10_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM10_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT0 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT2 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S10_INI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {auto} \
   CONFIG.CONNECTIONS {HBM10_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM10_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM15_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM5_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM1_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM0_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM6_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM12_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM8_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM14_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM3_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM4_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM9_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM11_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM7_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM2_PORT1 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}} HBM13_PORT3 {read_bw {50} write_bw {50} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S11_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M02_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S12_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M03_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S13_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M02_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S14_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M03_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S15_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M02_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S16_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M03_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S17_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M02_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S18_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M03_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S19_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M02_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S20_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M03_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S21_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M02_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S22_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M03_INI {read_bw {50} write_bw {50}}} \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_cips/S23_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /static_region/noc/axi_noc_cips/aclk0]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S01_AXI} \
 ] [get_bd_pins /static_region/noc/axi_noc_cips/aclk1]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S02_AXI} \
 ] [get_bd_pins /static_region/noc/axi_noc_cips/aclk2]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S03_AXI} \
 ] [get_bd_pins /static_region/noc/axi_noc_cips/aclk3]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI} \
 ] [get_bd_pins /static_region/noc/axi_noc_cips/aclk4]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {HBM00_AXI:HBM01_AXI:HBM02_AXI:HBM03_AXI:HBM04_AXI:HBM05_AXI:HBM06_AXI:HBM07_AXI:HBM08_AXI:HBM09_AXI:HBM10_AXI:HBM11_AXI:HBM12_AXI:HBM13_AXI:HBM14_AXI:HBM15_AXI:HBM16_AXI:HBM17_AXI:HBM18_AXI:HBM19_AXI:HBM20_AXI:HBM21_AXI:HBM22_AXI:HBM23_AXI:HBM24_AXI:HBM25_AXI:HBM26_AXI:HBM27_AXI:HBM28_AXI:HBM29_AXI:HBM30_AXI:HBM31_AXI:HBM32_AXI:HBM33_AXI:HBM34_AXI:HBM35_AXI:HBM36_AXI:HBM37_AXI:HBM38_AXI:HBM39_AXI:HBM40_AXI:HBM41_AXI:HBM42_AXI:HBM43_AXI:HBM44_AXI:HBM45_AXI:HBM46_AXI:HBM47_AXI:HBM48_AXI:HBM49_AXI:HBM50_AXI:HBM51_AXI:HBM52_AXI:HBM53_AXI:HBM54_AXI:HBM55_AXI:HBM56_AXI:HBM57_AXI:HBM58_AXI:HBM59_AXI:HBM60_AXI:HBM61_AXI:HBM62_AXI:HBM63_AXI} \
 ] [get_bd_pins /static_region/noc/axi_noc_cips/aclk5]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M01_AXI} \
 ] [get_bd_pins /static_region/noc/axi_noc_cips/aclk6]

  # Create instance: axi_noc_mc_ddr4_1, and set properties
  set axi_noc_mc_ddr4_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_mc_ddr4_1 ]
  set_property -dict [list \
    CONFIG.CONTROLLERTYPE {DDR4_SDRAM} \
    CONFIG.MC0_CONFIG_NUM {config21} \
    CONFIG.MC0_FLIPPED_PINOUT {false} \
    CONFIG.MC_CHAN_REGION0 {DDR_CH2} \
    CONFIG.MC_COMPONENT_WIDTH {x4} \
    CONFIG.MC_DATAWIDTH {72} \
    CONFIG.MC_INIT_MEM_USING_ECC_SCRUB {true} \
    CONFIG.MC_INPUTCLK0_PERIOD {5000} \
    CONFIG.MC_MEMORY_DEVICETYPE {RDIMMs} \
    CONFIG.MC_MEMORY_SPEEDGRADE {DDR4-3200AA(22-22-22)} \
    CONFIG.MC_NO_CHANNELS {Single} \
    CONFIG.MC_PARITY {true} \
    CONFIG.MC_RANK {1} \
    CONFIG.MC_ROWADDRESSWIDTH {18} \
    CONFIG.MC_STACKHEIGHT {1} \
    CONFIG.MC_SYSTEM_CLOCK {Differential} \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MC {1} \
    CONFIG.NUM_MCP {4} \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {0} \
    CONFIG.NUM_NSI {2} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_mc_ddr4_1


  set_property -dict [ list \
   CONFIG.CONNECTIONS { MC_0 {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64} } } \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_mc_ddr4_1/S00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS { MC_1 {read_bw {800} write_bw {800} read_avg_burst {64} write_avg_burst {64} } } \
 ] [get_bd_intf_pins /static_region/noc/axi_noc_mc_ddr4_1/S01_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {} \
 ] [get_bd_pins /static_region/noc/axi_noc_mc_ddr4_1/aclk0]

  # Create interface connections
  connect_bd_intf_net -intf_net Conn1 [get_bd_intf_pins axi_noc_mc_ddr4_1/CH0_DDR4_0] [get_bd_intf_pins CH0_DDR4_0_1]
  connect_bd_intf_net -intf_net Conn2 [get_bd_intf_pins axi_noc_mc_ddr4_1/sys_clk0] [get_bd_intf_pins sys_clk0_1]
  connect_bd_intf_net -intf_net Conn3 [get_bd_intf_pins axi_noc_mc_ddr4_0/sys_clk0] [get_bd_intf_pins sys_clk0_0]
  connect_bd_intf_net -intf_net Conn4 [get_bd_intf_pins axi_noc_mc_ddr4_0/CH0_DDR4_0] [get_bd_intf_pins CH0_DDR4_0_0]
  connect_bd_intf_net -intf_net Conn5 [get_bd_intf_pins axi_noc_cips/S03_AXI] [get_bd_intf_pins S03_AXI]
  connect_bd_intf_net -intf_net Conn6 [get_bd_intf_pins axi_noc_cips/hbm_ref_clk0] [get_bd_intf_pins hbm_ref_clk_0]
  connect_bd_intf_net -intf_net Conn7 [get_bd_intf_pins axi_noc_cips/hbm_ref_clk1] [get_bd_intf_pins hbm_ref_clk_1]
  connect_bd_intf_net -intf_net Conn8 [get_bd_intf_pins axi_noc_cips/S01_AXI] [get_bd_intf_pins S01_AXI]
  connect_bd_intf_net -intf_net Conn9 [get_bd_intf_pins axi_noc_cips/S00_AXI] [get_bd_intf_pins S00_AXI]
  connect_bd_intf_net -intf_net Conn10 [get_bd_intf_pins axi_noc_cips/S02_AXI] [get_bd_intf_pins S02_AXI]
  connect_bd_intf_net -intf_net Conn11 [get_bd_intf_pins axi_noc_cips/M00_AXI] [get_bd_intf_pins M00_AXI]
  connect_bd_intf_net -intf_net Conn12 [get_bd_intf_pins axi_noc_cips/HBM00_AXI] [get_bd_intf_pins HBM00_AXI]
  connect_bd_intf_net -intf_net Conn13 [get_bd_intf_pins axi_noc_cips/HBM01_AXI] [get_bd_intf_pins HBM01_AXI]
  connect_bd_intf_net -intf_net Conn14 [get_bd_intf_pins axi_noc_cips/HBM02_AXI] [get_bd_intf_pins HBM02_AXI]
  connect_bd_intf_net -intf_net Conn15 [get_bd_intf_pins axi_noc_cips/HBM03_AXI] [get_bd_intf_pins HBM03_AXI]
  connect_bd_intf_net -intf_net Conn16 [get_bd_intf_pins axi_noc_cips/HBM04_AXI] [get_bd_intf_pins HBM04_AXI]
  connect_bd_intf_net -intf_net Conn17 [get_bd_intf_pins axi_noc_cips/HBM05_AXI] [get_bd_intf_pins HBM05_AXI]
  connect_bd_intf_net -intf_net Conn18 [get_bd_intf_pins axi_noc_cips/HBM06_AXI] [get_bd_intf_pins HBM06_AXI]
  connect_bd_intf_net -intf_net Conn19 [get_bd_intf_pins axi_noc_cips/HBM07_AXI] [get_bd_intf_pins HBM07_AXI]
  connect_bd_intf_net -intf_net Conn20 [get_bd_intf_pins axi_noc_cips/HBM08_AXI] [get_bd_intf_pins HBM08_AXI]
  connect_bd_intf_net -intf_net Conn21 [get_bd_intf_pins axi_noc_cips/HBM09_AXI] [get_bd_intf_pins HBM09_AXI]
  connect_bd_intf_net -intf_net Conn22 [get_bd_intf_pins axi_noc_cips/HBM10_AXI] [get_bd_intf_pins HBM10_AXI]
  connect_bd_intf_net -intf_net Conn23 [get_bd_intf_pins axi_noc_cips/HBM11_AXI] [get_bd_intf_pins HBM11_AXI]
  connect_bd_intf_net -intf_net Conn24 [get_bd_intf_pins axi_noc_cips/HBM12_AXI] [get_bd_intf_pins HBM12_AXI]
  connect_bd_intf_net -intf_net Conn25 [get_bd_intf_pins axi_noc_cips/HBM13_AXI] [get_bd_intf_pins HBM13_AXI]
  connect_bd_intf_net -intf_net Conn26 [get_bd_intf_pins axi_noc_cips/HBM14_AXI] [get_bd_intf_pins HBM14_AXI]
  connect_bd_intf_net -intf_net Conn27 [get_bd_intf_pins axi_noc_cips/HBM15_AXI] [get_bd_intf_pins HBM15_AXI]
  connect_bd_intf_net -intf_net Conn28 [get_bd_intf_pins axi_noc_cips/HBM16_AXI] [get_bd_intf_pins HBM16_AXI]
  connect_bd_intf_net -intf_net Conn29 [get_bd_intf_pins axi_noc_cips/HBM17_AXI] [get_bd_intf_pins HBM17_AXI]
  connect_bd_intf_net -intf_net Conn30 [get_bd_intf_pins axi_noc_cips/HBM18_AXI] [get_bd_intf_pins HBM18_AXI]
  connect_bd_intf_net -intf_net Conn31 [get_bd_intf_pins axi_noc_cips/HBM19_AXI] [get_bd_intf_pins HBM19_AXI]
  connect_bd_intf_net -intf_net Conn32 [get_bd_intf_pins axi_noc_cips/HBM20_AXI] [get_bd_intf_pins HBM20_AXI]
  connect_bd_intf_net -intf_net Conn33 [get_bd_intf_pins axi_noc_cips/HBM21_AXI] [get_bd_intf_pins HBM21_AXI]
  connect_bd_intf_net -intf_net Conn34 [get_bd_intf_pins axi_noc_cips/HBM22_AXI] [get_bd_intf_pins HBM22_AXI]
  connect_bd_intf_net -intf_net Conn35 [get_bd_intf_pins axi_noc_cips/HBM23_AXI] [get_bd_intf_pins HBM23_AXI]
  connect_bd_intf_net -intf_net Conn36 [get_bd_intf_pins axi_noc_cips/HBM24_AXI] [get_bd_intf_pins HBM24_AXI]
  connect_bd_intf_net -intf_net Conn37 [get_bd_intf_pins axi_noc_cips/HBM25_AXI] [get_bd_intf_pins HBM25_AXI]
  connect_bd_intf_net -intf_net Conn38 [get_bd_intf_pins axi_noc_cips/HBM26_AXI] [get_bd_intf_pins HBM26_AXI]
  connect_bd_intf_net -intf_net Conn39 [get_bd_intf_pins axi_noc_cips/HBM27_AXI] [get_bd_intf_pins HBM27_AXI]
  connect_bd_intf_net -intf_net Conn40 [get_bd_intf_pins axi_noc_cips/HBM28_AXI] [get_bd_intf_pins HBM28_AXI]
  connect_bd_intf_net -intf_net Conn41 [get_bd_intf_pins axi_noc_cips/HBM29_AXI] [get_bd_intf_pins HBM29_AXI]
  connect_bd_intf_net -intf_net Conn42 [get_bd_intf_pins axi_noc_cips/HBM30_AXI] [get_bd_intf_pins HBM30_AXI]
  connect_bd_intf_net -intf_net Conn43 [get_bd_intf_pins axi_noc_cips/HBM31_AXI] [get_bd_intf_pins HBM31_AXI]
  connect_bd_intf_net -intf_net Conn44 [get_bd_intf_pins axi_noc_cips/HBM32_AXI] [get_bd_intf_pins HBM32_AXI]
  connect_bd_intf_net -intf_net Conn45 [get_bd_intf_pins axi_noc_cips/HBM33_AXI] [get_bd_intf_pins HBM33_AXI]
  connect_bd_intf_net -intf_net Conn46 [get_bd_intf_pins axi_noc_cips/HBM34_AXI] [get_bd_intf_pins HBM34_AXI]
  connect_bd_intf_net -intf_net Conn47 [get_bd_intf_pins axi_noc_cips/HBM35_AXI] [get_bd_intf_pins HBM35_AXI]
  connect_bd_intf_net -intf_net Conn48 [get_bd_intf_pins axi_noc_cips/HBM36_AXI] [get_bd_intf_pins HBM36_AXI]
  connect_bd_intf_net -intf_net Conn49 [get_bd_intf_pins axi_noc_cips/HBM37_AXI] [get_bd_intf_pins HBM37_AXI]
  connect_bd_intf_net -intf_net Conn50 [get_bd_intf_pins axi_noc_cips/HBM38_AXI] [get_bd_intf_pins HBM38_AXI]
  connect_bd_intf_net -intf_net Conn51 [get_bd_intf_pins axi_noc_cips/HBM39_AXI] [get_bd_intf_pins HBM39_AXI]
  connect_bd_intf_net -intf_net Conn52 [get_bd_intf_pins axi_noc_cips/HBM40_AXI] [get_bd_intf_pins HBM40_AXI]
  connect_bd_intf_net -intf_net Conn53 [get_bd_intf_pins axi_noc_cips/HBM41_AXI] [get_bd_intf_pins HBM41_AXI]
  connect_bd_intf_net -intf_net Conn54 [get_bd_intf_pins axi_noc_cips/HBM42_AXI] [get_bd_intf_pins HBM42_AXI]
  connect_bd_intf_net -intf_net Conn55 [get_bd_intf_pins axi_noc_cips/HBM43_AXI] [get_bd_intf_pins HBM43_AXI]
  connect_bd_intf_net -intf_net Conn56 [get_bd_intf_pins axi_noc_cips/HBM44_AXI] [get_bd_intf_pins HBM44_AXI]
  connect_bd_intf_net -intf_net Conn57 [get_bd_intf_pins axi_noc_cips/HBM45_AXI] [get_bd_intf_pins HBM45_AXI]
  connect_bd_intf_net -intf_net Conn58 [get_bd_intf_pins axi_noc_cips/HBM46_AXI] [get_bd_intf_pins HBM46_AXI]
  connect_bd_intf_net -intf_net Conn59 [get_bd_intf_pins axi_noc_cips/HBM47_AXI] [get_bd_intf_pins HBM47_AXI]
  connect_bd_intf_net -intf_net Conn60 [get_bd_intf_pins axi_noc_cips/HBM48_AXI] [get_bd_intf_pins HBM48_AXI]
  connect_bd_intf_net -intf_net Conn61 [get_bd_intf_pins axi_noc_cips/HBM49_AXI] [get_bd_intf_pins HBM49_AXI]
  connect_bd_intf_net -intf_net Conn62 [get_bd_intf_pins axi_noc_cips/HBM50_AXI] [get_bd_intf_pins HBM50_AXI]
  connect_bd_intf_net -intf_net Conn63 [get_bd_intf_pins axi_noc_cips/HBM51_AXI] [get_bd_intf_pins HBM51_AXI]
  connect_bd_intf_net -intf_net Conn64 [get_bd_intf_pins axi_noc_cips/HBM52_AXI] [get_bd_intf_pins HBM52_AXI]
  connect_bd_intf_net -intf_net Conn65 [get_bd_intf_pins axi_noc_cips/HBM53_AXI] [get_bd_intf_pins HBM53_AXI]
  connect_bd_intf_net -intf_net Conn66 [get_bd_intf_pins axi_noc_cips/HBM54_AXI] [get_bd_intf_pins HBM54_AXI]
  connect_bd_intf_net -intf_net Conn67 [get_bd_intf_pins axi_noc_cips/HBM55_AXI] [get_bd_intf_pins HBM55_AXI]
  connect_bd_intf_net -intf_net Conn68 [get_bd_intf_pins axi_noc_cips/HBM56_AXI] [get_bd_intf_pins HBM56_AXI]
  connect_bd_intf_net -intf_net Conn69 [get_bd_intf_pins axi_noc_cips/HBM57_AXI] [get_bd_intf_pins HBM57_AXI]
  connect_bd_intf_net -intf_net Conn70 [get_bd_intf_pins axi_noc_cips/HBM58_AXI] [get_bd_intf_pins HBM58_AXI]
  connect_bd_intf_net -intf_net Conn71 [get_bd_intf_pins axi_noc_cips/HBM59_AXI] [get_bd_intf_pins HBM59_AXI]
  connect_bd_intf_net -intf_net Conn72 [get_bd_intf_pins axi_noc_cips/HBM60_AXI] [get_bd_intf_pins HBM60_AXI]
  connect_bd_intf_net -intf_net Conn73 [get_bd_intf_pins axi_noc_cips/HBM61_AXI] [get_bd_intf_pins HBM61_AXI]
  connect_bd_intf_net -intf_net Conn74 [get_bd_intf_pins axi_noc_cips/HBM63_AXI] [get_bd_intf_pins HBM63_AXI]
  connect_bd_intf_net -intf_net Conn75 [get_bd_intf_pins axi_noc_cips/HBM62_AXI] [get_bd_intf_pins HBM62_AXI]
  connect_bd_intf_net -intf_net Conn79 [get_bd_intf_pins axi_noc_cips/S00_INI] [get_bd_intf_pins S00_INI]
  connect_bd_intf_net -intf_net Conn80 [get_bd_intf_pins axi_noc_cips/S01_INI] [get_bd_intf_pins S01_INI]
  connect_bd_intf_net -intf_net Conn81 [get_bd_intf_pins axi_noc_cips/S02_INI] [get_bd_intf_pins S02_INI]
  connect_bd_intf_net -intf_net Conn82 [get_bd_intf_pins axi_noc_cips/S03_INI] [get_bd_intf_pins S03_INI]
  connect_bd_intf_net -intf_net Conn83 [get_bd_intf_pins axi_noc_cips/S04_INI] [get_bd_intf_pins S04_INI]
  connect_bd_intf_net -intf_net Conn84 [get_bd_intf_pins axi_noc_cips/S05_INI] [get_bd_intf_pins S05_INI]
  connect_bd_intf_net -intf_net Conn85 [get_bd_intf_pins axi_noc_cips/S06_INI] [get_bd_intf_pins S06_INI]
  connect_bd_intf_net -intf_net Conn86 [get_bd_intf_pins axi_noc_cips/S07_INI] [get_bd_intf_pins S07_INI]
  connect_bd_intf_net -intf_net Conn87 [get_bd_intf_pins axi_noc_cips/S08_INI] [get_bd_intf_pins S08_INI]
  connect_bd_intf_net -intf_net Conn88 [get_bd_intf_pins axi_noc_cips/S09_INI] [get_bd_intf_pins S09_INI]
  connect_bd_intf_net -intf_net Conn89 [get_bd_intf_pins axi_noc_cips/S10_INI] [get_bd_intf_pins S10_INI]
  connect_bd_intf_net -intf_net Conn90 [get_bd_intf_pins axi_noc_cips/S11_INI] [get_bd_intf_pins S11_INI]
  connect_bd_intf_net -intf_net Conn91 [get_bd_intf_pins axi_noc_cips/S12_INI] [get_bd_intf_pins S12_INI]
  connect_bd_intf_net -intf_net Conn92 [get_bd_intf_pins axi_noc_cips/S13_INI] [get_bd_intf_pins S13_INI]
  connect_bd_intf_net -intf_net Conn93 [get_bd_intf_pins axi_noc_cips/S14_INI] [get_bd_intf_pins S14_INI]
  connect_bd_intf_net -intf_net Conn94 [get_bd_intf_pins axi_noc_cips/S15_INI] [get_bd_intf_pins S15_INI]
  connect_bd_intf_net -intf_net Conn95 [get_bd_intf_pins axi_noc_cips/S16_INI] [get_bd_intf_pins S16_INI]
  connect_bd_intf_net -intf_net Conn96 [get_bd_intf_pins axi_noc_cips/S17_INI] [get_bd_intf_pins S17_INI]
  connect_bd_intf_net -intf_net Conn97 [get_bd_intf_pins axi_noc_cips/S18_INI] [get_bd_intf_pins S18_INI]
  connect_bd_intf_net -intf_net Conn98 [get_bd_intf_pins axi_noc_cips/S19_INI] [get_bd_intf_pins S19_INI]
  connect_bd_intf_net -intf_net Conn99 [get_bd_intf_pins axi_noc_cips/S20_INI] [get_bd_intf_pins S20_INI1]
  connect_bd_intf_net -intf_net Conn100 [get_bd_intf_pins axi_noc_cips/S21_INI] [get_bd_intf_pins S21_INI1]
  connect_bd_intf_net -intf_net Conn101 [get_bd_intf_pins axi_noc_cips/S22_INI] [get_bd_intf_pins S22_INI1]
  connect_bd_intf_net -intf_net Conn102 [get_bd_intf_pins axi_noc_cips/S23_INI] [get_bd_intf_pins S23_INI1]
  connect_bd_intf_net -intf_net Conn103 [get_bd_intf_pins axi_noc_cips/M05_INI] [get_bd_intf_pins M05_INI]
  connect_bd_intf_net -intf_net Conn104 [get_bd_intf_pins axi_noc_cips/M04_INI] [get_bd_intf_pins M04_INI]
  connect_bd_intf_net -intf_net axi_noc_cips_M00_INI [get_bd_intf_pins axi_noc_cips/M00_INI] [get_bd_intf_pins axi_noc_mc_ddr4_0/S00_INI]
  connect_bd_intf_net -intf_net axi_noc_cips_M01_AXI [get_bd_intf_pins M02_AXI] [get_bd_intf_pins axi_noc_cips/M01_AXI]
  connect_bd_intf_net -intf_net axi_noc_cips_M01_INI [get_bd_intf_pins axi_noc_cips/M01_INI] [get_bd_intf_pins axi_noc_mc_ddr4_0/S01_INI]
  connect_bd_intf_net -intf_net axi_noc_cips_M02_INI [get_bd_intf_pins axi_noc_cips/M02_INI] [get_bd_intf_pins axi_noc_mc_ddr4_1/S00_INI]
  connect_bd_intf_net -intf_net axi_noc_cips_M03_INI [get_bd_intf_pins axi_noc_cips/M03_INI] [get_bd_intf_pins axi_noc_mc_ddr4_1/S01_INI]
  connect_bd_intf_net -intf_net axi_noc_cips_M06_INI [get_bd_intf_pins M06_INI] [get_bd_intf_pins axi_noc_cips/M06_INI]

  # Create port connections
  connect_bd_net -net aclk0_1  [get_bd_pins aclk0] \
  [get_bd_pins axi_noc_mc_ddr4_1/aclk0] \
  [get_bd_pins axi_noc_cips/aclk4]
  connect_bd_net -net aclk1_1  [get_bd_pins aclk1] \
  [get_bd_pins axi_noc_cips/aclk1]
  connect_bd_net -net aclk2_1  [get_bd_pins aclk2] \
  [get_bd_pins axi_noc_cips/aclk2]
  connect_bd_net -net aclk3_1  [get_bd_pins aclk3] \
  [get_bd_pins axi_noc_cips/aclk3]
  connect_bd_net -net aclk4_1  [get_bd_pins aclk4] \
  [get_bd_pins axi_noc_cips/aclk0]
  connect_bd_net -net aclk5_1  [get_bd_pins aclk5] \
  [get_bd_pins axi_noc_cips/aclk5]
  connect_bd_net -net aclk6_1  [get_bd_pins aclk6] \
  [get_bd_pins axi_noc_cips/aclk6]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: static_region
proc create_hier_cell_static_region { parentCell nameHier } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_static_region() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 sys_clk0_0

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:ddr4_rtl:1.0 CH0_DDR4_0_0

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:ddr4_rtl:1.0 CH0_DDR4_0_1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 hbm_ref_clk_0

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 sys_clk0_1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 hbm_ref_clk_1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 gt_pcie_refclk

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 smbus_0

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 gt_pciea1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S01_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S02_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S03_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S04_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S05_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S06_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S07_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S08_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S09_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S10_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S11_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S12_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S13_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S14_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S15_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S16_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S17_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S18_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S19_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S20_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S21_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S22_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S23_INI

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M05_INI

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M04_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS1

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS2

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS2

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS3

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS3

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS4

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS4

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS5

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS5

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS6

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS6

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS7

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS7

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS8

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS8

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS9

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS9

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS10

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS10

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS11

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS11

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS12

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS12

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS13

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS13

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS14

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS14

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S00_INIS15

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M00_INIS15

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI6

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI2

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI3

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI4

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S00_INI5

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI1

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI2

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI3

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI4

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_0

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_1

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_2

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_3

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_4

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_5

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_6

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_7

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_8

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_9

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_10

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_11

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_12

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_13

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_14

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_15

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_16

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_17

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_18

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_19

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_20

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_21

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_22

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_23

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_24

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_25

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_26

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_27

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_28

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_29

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_30

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_31

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_32

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_33

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_34

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_35

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_36

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_37

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_38

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_39

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_40

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_41

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_42

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_43

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_44

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_45

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_46

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_47

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_48

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_49

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_50

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_51

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_52

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_53

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_54

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_55

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_56

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_57

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_58

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_59

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_60

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_61

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_62

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 rp_intf_63


  # Create pins
  create_bd_pin -dir O -type clk pl0_ref_clk
  create_bd_pin -dir O -from 0 -to 0 -type rst resetn_pl_periph
  create_bd_pin -dir O -type clk clk_out1
  create_bd_pin -dir O -type clk pl3_ref_clk
  create_bd_pin -dir O -type rst pl3_resetn
  create_bd_pin -dir O -type clk clk_out2
  create_bd_pin -dir O -from 0 -to 0 -type rst peripheral_aresetn1
  create_bd_pin -dir O -type clk clk_out3
  create_bd_pin -dir O -from 0 -to 0 -type rst peripheral_aresetn2

  # Create instance: noc
  create_hier_cell_noc $hier_obj noc

  # Create instance: aved
  create_hier_cell_aved $hier_obj aved

  # Create instance: clk_wizard_0, and set properties
  set clk_wizard_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wizard:1.0 clk_wizard_0 ]
  set_property -dict [list \
    CONFIG.CLKOUT_DRIVES {No_buffer,BUFG,BUFG,BUFG,BUFG,BUFG,BUFG} \
    CONFIG.CLKOUT_DYN_PS {None,None,None,None,None,None,None} \
    CONFIG.CLKOUT_GROUPING {Auto,Auto,Auto,Auto,Auto,Auto,Auto} \
    CONFIG.CLKOUT_MATCHED_ROUTING {false,false,false,false,false,false,false} \
    CONFIG.CLKOUT_PORT {clk_out1,clk_out2,clk_out3,clk_out4,clk_out5,clk_out6,clk_out7} \
    CONFIG.CLKOUT_REQUESTED_DUTY_CYCLE {50.000,50.000,50.000,50.000,50.000,50.000,50.000} \
    CONFIG.CLKOUT_REQUESTED_OUT_FREQUENCY {400,100.000,100.000,100.000,100.000,100.000,100.000} \
    CONFIG.CLKOUT_REQUESTED_PHASE {0.000,0.000,0.000,0.000,0.000,0.000,0.000} \
    CONFIG.CLKOUT_USED {true,false,false,false,false,false,false} \
    CONFIG.RESET_TYPE {ACTIVE_LOW} \
    CONFIG.USE_DYN_RECONFIG {false} \
  ] $clk_wizard_0


  # Create instance: proc_sys_reset_1, and set properties
  set proc_sys_reset_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_1 ]

  # Create instance: dcmac_noc
  create_hier_cell_dcmac_noc $hier_obj dcmac_noc

  # Create instance: axi_noc_1, and set properties
  set axi_noc_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_1 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_1


  set_property -dict [ list \
   CONFIG.CATEGORY {ps_pcie} \
 ] [get_bd_intf_pins /static_region/axi_noc_1/M00_AXI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_AXI {read_bw {500} write_bw {500} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /static_region/axi_noc_1/S00_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {} \
 ] [get_bd_pins /static_region/axi_noc_1/aclk0]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI} \
 ] [get_bd_pins /static_region/axi_noc_1/aclk1]

  # Create instance: util_vector_logic_0, and set properties
  set util_vector_logic_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic:2.0 util_vector_logic_0 ]
  set_property -dict [list \
    CONFIG.C_OPERATION {not} \
    CONFIG.C_SIZE {1} \
  ] $util_vector_logic_0


  # Create instance: virt_noc
  create_hier_cell_virt_noc $hier_obj virt_noc

  # Create instance: clk_rst_shell
  create_hier_cell_clk_rst_shell $hier_obj clk_rst_shell

  # Create instance: dfx_decoupler_0, and set properties
  set dfx_decoupler_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:dfx_decoupler:1.0 dfx_decoupler_0 ]
  set_property -dict [list \
    CONFIG.ALL_PARAMS {INTF {intf_0 {ID 0 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH\
1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR\
{WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4\
PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH\
0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK\
{WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT\
1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_1 {ID 1 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH\
1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT\
1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH\
4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1}\
WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT\
1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER\
{WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_2 {ID 2 VLNV xilinx.com:interface:aximm_rtl:1.0\
PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH\
1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST\
{WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT\
0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_3 {ID 3 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_4 {ID 4 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID\
{WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT\
1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH\
3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER\
{WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1}\
ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH\
0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_5 {ID 5 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID\
{WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1\
PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK\
{WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT\
1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT\
1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS\
{WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_6 {ID 6 VLNV xilinx.com:interface:aximm_rtl:1.0\
PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH\
1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST\
{WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT\
0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_7 {ID 7 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_8 {ID 8 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID\
{WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT\
1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH\
3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER\
{WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1}\
ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH\
0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_9 {ID 9 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID\
{WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1\
PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK\
{WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT\
1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT\
1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS\
{WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_10 {ID 10 VLNV xilinx.com:interface:aximm_rtl:1.0\
PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH\
1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST\
{WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT\
0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_11 {ID 11 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_12 {ID 12 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_13 {ID 13 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_14 {ID 14 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_15 {ID 15 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_16 {ID 16 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_17 {ID 17 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_18 {ID 18 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_19 {ID 19 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_20 {ID 20 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_21 {ID 21 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_22 {ID 22 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_23 {ID 23 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_24 {ID 24 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_25 {ID 25 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_26 {ID 26 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_27 {ID 27 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_28 {ID 28 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_29 {ID 29 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_30 {ID 30 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_31 {ID 31 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_32 {ID 32 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_33 {ID 33 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_34 {ID 34 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_35 {ID 35 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_36 {ID 36 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_37 {ID 37 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_38 {ID 38 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_39 {ID 39 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_40 {ID 40 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_41 {ID 41 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_42 {ID 42 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_43 {ID 43 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_44 {ID 44 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_45 {ID 45 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_46 {ID 46 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_47 {ID 47 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_48 {ID 48 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_49 {ID 49 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_50 {ID 50 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_51 {ID 51 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_52 {ID 52 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_53 {ID 53 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_54 {ID 54 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_55 {ID 55 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_56 {ID 56 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_57 {ID 57 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_58 {ID 58 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_59 {ID 59 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_60 {ID 60 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_61 {ID 61 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL\
AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT\
1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH\
2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0}\
WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT\
0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION\
{WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}\
intf_62 {ID 62 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1} AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH\
1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH 1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1}\
AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1} AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH\
4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH 1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP\
{WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE {WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT\
1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0 PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH\
2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}} intf_63 {ID 63 VLNV xilinx.com:interface:aximm_rtl:1.0 PROTOCOL AXI4 SIGNALS {ARVALID {WIDTH 1 PRESENT 1} ARREADY {WIDTH 1 PRESENT 1}\
AWVALID {WIDTH 1 PRESENT 1} AWREADY {WIDTH 1 PRESENT 1} BVALID {WIDTH 1 PRESENT 1} BREADY {WIDTH 1 PRESENT 1} RVALID {WIDTH 1 PRESENT 1} RREADY {WIDTH 1 PRESENT 1} WVALID {WIDTH 1 PRESENT 1} WREADY {WIDTH\
1 PRESENT 1} AWID {WIDTH 0 PRESENT 0} AWADDR {WIDTH 64 PRESENT 1} AWLEN {WIDTH 8 PRESENT 1} AWSIZE {WIDTH 3 PRESENT 1} AWBURST {WIDTH 2 PRESENT 1} AWLOCK {WIDTH 1 PRESENT 1} AWCACHE {WIDTH 4 PRESENT 1}\
AWPROT {WIDTH 3 PRESENT 1} AWREGION {WIDTH 4 PRESENT 0} AWQOS {WIDTH 4 PRESENT 1} AWUSER {WIDTH 0 PRESENT 0} WID {WIDTH 0 PRESENT 0} WDATA {WIDTH 256 PRESENT 1} WSTRB {WIDTH 32 PRESENT 1} WLAST {WIDTH\
1 PRESENT 1} WUSER {WIDTH 0 PRESENT 0} BID {WIDTH 0 PRESENT 0} BRESP {WIDTH 2 PRESENT 1} BUSER {WIDTH 0 PRESENT 0} ARID {WIDTH 0 PRESENT 0} ARADDR {WIDTH 64 PRESENT 1} ARLEN {WIDTH 8 PRESENT 1} ARSIZE\
{WIDTH 3 PRESENT 1} ARBURST {WIDTH 2 PRESENT 1} ARLOCK {WIDTH 1 PRESENT 1} ARCACHE {WIDTH 4 PRESENT 1} ARPROT {WIDTH 3 PRESENT 1} ARREGION {WIDTH 4 PRESENT 0} ARQOS {WIDTH 4 PRESENT 1} ARUSER {WIDTH 0\
PRESENT 0} RID {WIDTH 0 PRESENT 0} RDATA {WIDTH 256 PRESENT 1} RRESP {WIDTH 2 PRESENT 0} RLAST {WIDTH 1 PRESENT 1} RUSER {WIDTH 0 PRESENT 0}}}}} \
    CONFIG.GUI_SELECT_VLNV {xilinx.com:interface:aximm_rtl:1.0} \
  ] $dfx_decoupler_0


  # Create interface connections
  connect_bd_intf_net -intf_net Conn1 [get_bd_intf_pins virt_noc/M00_INI] [get_bd_intf_pins M00_INI]
  connect_bd_intf_net -intf_net Conn2 [get_bd_intf_pins dfx_decoupler_0/rp_intf_0] [get_bd_intf_pins rp_intf_0]
  connect_bd_intf_net -intf_net Conn3 [get_bd_intf_pins dfx_decoupler_0/rp_intf_1] [get_bd_intf_pins rp_intf_1]
  connect_bd_intf_net -intf_net Conn4 [get_bd_intf_pins dfx_decoupler_0/rp_intf_2] [get_bd_intf_pins rp_intf_2]
  connect_bd_intf_net -intf_net Conn5 [get_bd_intf_pins noc/sys_clk0_0] [get_bd_intf_pins sys_clk0_0]
  connect_bd_intf_net -intf_net Conn6 [get_bd_intf_pins noc/CH0_DDR4_0_0] [get_bd_intf_pins CH0_DDR4_0_0]
  connect_bd_intf_net -intf_net Conn7 [get_bd_intf_pins noc/CH0_DDR4_0_1] [get_bd_intf_pins CH0_DDR4_0_1]
  connect_bd_intf_net -intf_net Conn8 [get_bd_intf_pins noc/hbm_ref_clk_0] [get_bd_intf_pins hbm_ref_clk_0]
  connect_bd_intf_net -intf_net Conn9 [get_bd_intf_pins noc/sys_clk0_1] [get_bd_intf_pins sys_clk0_1]
  connect_bd_intf_net -intf_net Conn10 [get_bd_intf_pins dfx_decoupler_0/rp_intf_3] [get_bd_intf_pins rp_intf_3]
  connect_bd_intf_net -intf_net Conn11 [get_bd_intf_pins dfx_decoupler_0/rp_intf_4] [get_bd_intf_pins rp_intf_4]
  connect_bd_intf_net -intf_net Conn12 [get_bd_intf_pins dfx_decoupler_0/rp_intf_5] [get_bd_intf_pins rp_intf_5]
  connect_bd_intf_net -intf_net Conn13 [get_bd_intf_pins dfx_decoupler_0/rp_intf_6] [get_bd_intf_pins rp_intf_6]
  connect_bd_intf_net -intf_net Conn14 [get_bd_intf_pins dfx_decoupler_0/rp_intf_7] [get_bd_intf_pins rp_intf_7]
  connect_bd_intf_net -intf_net Conn15 [get_bd_intf_pins dfx_decoupler_0/rp_intf_8] [get_bd_intf_pins rp_intf_8]
  connect_bd_intf_net -intf_net Conn16 [get_bd_intf_pins dfx_decoupler_0/rp_intf_9] [get_bd_intf_pins rp_intf_9]
  connect_bd_intf_net -intf_net Conn17 [get_bd_intf_pins dfx_decoupler_0/rp_intf_10] [get_bd_intf_pins rp_intf_10]
  connect_bd_intf_net -intf_net Conn18 [get_bd_intf_pins dfx_decoupler_0/rp_intf_11] [get_bd_intf_pins rp_intf_11]
  connect_bd_intf_net -intf_net Conn19 [get_bd_intf_pins dfx_decoupler_0/rp_intf_12] [get_bd_intf_pins rp_intf_12]
  connect_bd_intf_net -intf_net Conn20 [get_bd_intf_pins dfx_decoupler_0/rp_intf_13] [get_bd_intf_pins rp_intf_13]
  connect_bd_intf_net -intf_net Conn21 [get_bd_intf_pins dfx_decoupler_0/rp_intf_14] [get_bd_intf_pins rp_intf_14]
  connect_bd_intf_net -intf_net Conn22 [get_bd_intf_pins dfx_decoupler_0/rp_intf_15] [get_bd_intf_pins rp_intf_15]
  connect_bd_intf_net -intf_net Conn23 [get_bd_intf_pins dfx_decoupler_0/rp_intf_16] [get_bd_intf_pins rp_intf_16]
  connect_bd_intf_net -intf_net Conn24 [get_bd_intf_pins dfx_decoupler_0/rp_intf_17] [get_bd_intf_pins rp_intf_17]
  connect_bd_intf_net -intf_net Conn25 [get_bd_intf_pins dfx_decoupler_0/rp_intf_18] [get_bd_intf_pins rp_intf_18]
  connect_bd_intf_net -intf_net Conn26 [get_bd_intf_pins dfx_decoupler_0/rp_intf_19] [get_bd_intf_pins rp_intf_19]
  connect_bd_intf_net -intf_net Conn27 [get_bd_intf_pins dfx_decoupler_0/rp_intf_20] [get_bd_intf_pins rp_intf_20]
  connect_bd_intf_net -intf_net Conn28 [get_bd_intf_pins dfx_decoupler_0/rp_intf_21] [get_bd_intf_pins rp_intf_21]
  connect_bd_intf_net -intf_net Conn29 [get_bd_intf_pins dfx_decoupler_0/rp_intf_22] [get_bd_intf_pins rp_intf_22]
  connect_bd_intf_net -intf_net Conn30 [get_bd_intf_pins dfx_decoupler_0/rp_intf_23] [get_bd_intf_pins rp_intf_23]
  connect_bd_intf_net -intf_net Conn31 [get_bd_intf_pins dfx_decoupler_0/rp_intf_24] [get_bd_intf_pins rp_intf_24]
  connect_bd_intf_net -intf_net Conn32 [get_bd_intf_pins dfx_decoupler_0/rp_intf_25] [get_bd_intf_pins rp_intf_25]
  connect_bd_intf_net -intf_net Conn33 [get_bd_intf_pins dfx_decoupler_0/rp_intf_26] [get_bd_intf_pins rp_intf_26]
  connect_bd_intf_net -intf_net Conn34 [get_bd_intf_pins dfx_decoupler_0/rp_intf_27] [get_bd_intf_pins rp_intf_27]
  connect_bd_intf_net -intf_net Conn35 [get_bd_intf_pins dfx_decoupler_0/rp_intf_28] [get_bd_intf_pins rp_intf_28]
  connect_bd_intf_net -intf_net Conn36 [get_bd_intf_pins dfx_decoupler_0/rp_intf_29] [get_bd_intf_pins rp_intf_29]
  connect_bd_intf_net -intf_net Conn37 [get_bd_intf_pins dfx_decoupler_0/rp_intf_30] [get_bd_intf_pins rp_intf_30]
  connect_bd_intf_net -intf_net Conn38 [get_bd_intf_pins dfx_decoupler_0/rp_intf_31] [get_bd_intf_pins rp_intf_31]
  connect_bd_intf_net -intf_net Conn39 [get_bd_intf_pins dfx_decoupler_0/rp_intf_32] [get_bd_intf_pins rp_intf_32]
  connect_bd_intf_net -intf_net Conn40 [get_bd_intf_pins dfx_decoupler_0/rp_intf_33] [get_bd_intf_pins rp_intf_33]
  connect_bd_intf_net -intf_net Conn41 [get_bd_intf_pins dfx_decoupler_0/rp_intf_34] [get_bd_intf_pins rp_intf_34]
  connect_bd_intf_net -intf_net Conn42 [get_bd_intf_pins dfx_decoupler_0/rp_intf_35] [get_bd_intf_pins rp_intf_35]
  connect_bd_intf_net -intf_net Conn43 [get_bd_intf_pins dfx_decoupler_0/rp_intf_36] [get_bd_intf_pins rp_intf_36]
  connect_bd_intf_net -intf_net Conn44 [get_bd_intf_pins dfx_decoupler_0/rp_intf_37] [get_bd_intf_pins rp_intf_37]
  connect_bd_intf_net -intf_net Conn45 [get_bd_intf_pins dfx_decoupler_0/rp_intf_38] [get_bd_intf_pins rp_intf_38]
  connect_bd_intf_net -intf_net Conn46 [get_bd_intf_pins dfx_decoupler_0/rp_intf_39] [get_bd_intf_pins rp_intf_39]
  connect_bd_intf_net -intf_net Conn47 [get_bd_intf_pins dfx_decoupler_0/rp_intf_40] [get_bd_intf_pins rp_intf_40]
  connect_bd_intf_net -intf_net Conn48 [get_bd_intf_pins dfx_decoupler_0/rp_intf_41] [get_bd_intf_pins rp_intf_41]
  connect_bd_intf_net -intf_net Conn49 [get_bd_intf_pins dfx_decoupler_0/rp_intf_42] [get_bd_intf_pins rp_intf_42]
  connect_bd_intf_net -intf_net Conn50 [get_bd_intf_pins dfx_decoupler_0/rp_intf_43] [get_bd_intf_pins rp_intf_43]
  connect_bd_intf_net -intf_net Conn51 [get_bd_intf_pins dfx_decoupler_0/rp_intf_44] [get_bd_intf_pins rp_intf_44]
  connect_bd_intf_net -intf_net Conn52 [get_bd_intf_pins dfx_decoupler_0/rp_intf_45] [get_bd_intf_pins rp_intf_45]
  connect_bd_intf_net -intf_net Conn53 [get_bd_intf_pins dfx_decoupler_0/rp_intf_46] [get_bd_intf_pins rp_intf_46]
  connect_bd_intf_net -intf_net Conn54 [get_bd_intf_pins dfx_decoupler_0/rp_intf_47] [get_bd_intf_pins rp_intf_47]
  connect_bd_intf_net -intf_net Conn55 [get_bd_intf_pins dfx_decoupler_0/rp_intf_48] [get_bd_intf_pins rp_intf_48]
  connect_bd_intf_net -intf_net Conn56 [get_bd_intf_pins dfx_decoupler_0/rp_intf_49] [get_bd_intf_pins rp_intf_49]
  connect_bd_intf_net -intf_net Conn57 [get_bd_intf_pins dfx_decoupler_0/rp_intf_50] [get_bd_intf_pins rp_intf_50]
  connect_bd_intf_net -intf_net Conn58 [get_bd_intf_pins dfx_decoupler_0/rp_intf_51] [get_bd_intf_pins rp_intf_51]
  connect_bd_intf_net -intf_net Conn59 [get_bd_intf_pins dfx_decoupler_0/rp_intf_52] [get_bd_intf_pins rp_intf_52]
  connect_bd_intf_net -intf_net Conn60 [get_bd_intf_pins dfx_decoupler_0/rp_intf_53] [get_bd_intf_pins rp_intf_53]
  connect_bd_intf_net -intf_net Conn61 [get_bd_intf_pins dfx_decoupler_0/rp_intf_54] [get_bd_intf_pins rp_intf_54]
  connect_bd_intf_net -intf_net Conn62 [get_bd_intf_pins dfx_decoupler_0/rp_intf_55] [get_bd_intf_pins rp_intf_55]
  connect_bd_intf_net -intf_net Conn63 [get_bd_intf_pins dfx_decoupler_0/rp_intf_56] [get_bd_intf_pins rp_intf_56]
  connect_bd_intf_net -intf_net Conn64 [get_bd_intf_pins dfx_decoupler_0/rp_intf_57] [get_bd_intf_pins rp_intf_57]
  connect_bd_intf_net -intf_net Conn65 [get_bd_intf_pins dfx_decoupler_0/rp_intf_58] [get_bd_intf_pins rp_intf_58]
  connect_bd_intf_net -intf_net Conn66 [get_bd_intf_pins dfx_decoupler_0/rp_intf_59] [get_bd_intf_pins rp_intf_59]
  connect_bd_intf_net -intf_net Conn67 [get_bd_intf_pins dfx_decoupler_0/rp_intf_60] [get_bd_intf_pins rp_intf_60]
  connect_bd_intf_net -intf_net Conn68 [get_bd_intf_pins dfx_decoupler_0/rp_intf_61] [get_bd_intf_pins rp_intf_61]
  connect_bd_intf_net -intf_net Conn69 [get_bd_intf_pins dfx_decoupler_0/rp_intf_62] [get_bd_intf_pins rp_intf_62]
  connect_bd_intf_net -intf_net Conn70 [get_bd_intf_pins dfx_decoupler_0/rp_intf_63] [get_bd_intf_pins rp_intf_63]
  connect_bd_intf_net -intf_net Conn71 [get_bd_intf_pins noc/hbm_ref_clk_1] [get_bd_intf_pins hbm_ref_clk_1]
  connect_bd_intf_net -intf_net Conn72 [get_bd_intf_pins aved/gt_pcie_refclk] [get_bd_intf_pins gt_pcie_refclk]
  connect_bd_intf_net -intf_net Conn73 [get_bd_intf_pins aved/smbus_0] [get_bd_intf_pins smbus_0]
  connect_bd_intf_net -intf_net Conn74 [get_bd_intf_pins aved/gt_pciea1] [get_bd_intf_pins gt_pciea1]
  connect_bd_intf_net -intf_net Conn75 [get_bd_intf_pins noc/S00_INI] [get_bd_intf_pins S00_INI]
  connect_bd_intf_net -intf_net Conn76 [get_bd_intf_pins noc/S01_INI] [get_bd_intf_pins S01_INI]
  connect_bd_intf_net -intf_net Conn77 [get_bd_intf_pins noc/S02_INI] [get_bd_intf_pins S02_INI]
  connect_bd_intf_net -intf_net Conn78 [get_bd_intf_pins noc/S03_INI] [get_bd_intf_pins S03_INI]
  connect_bd_intf_net -intf_net Conn79 [get_bd_intf_pins noc/S04_INI] [get_bd_intf_pins S04_INI]
  connect_bd_intf_net -intf_net Conn80 [get_bd_intf_pins noc/S05_INI] [get_bd_intf_pins S05_INI]
  connect_bd_intf_net -intf_net Conn81 [get_bd_intf_pins noc/S06_INI] [get_bd_intf_pins S06_INI]
  connect_bd_intf_net -intf_net Conn82 [get_bd_intf_pins noc/S07_INI] [get_bd_intf_pins S07_INI]
  connect_bd_intf_net -intf_net Conn83 [get_bd_intf_pins noc/S08_INI] [get_bd_intf_pins S08_INI]
  connect_bd_intf_net -intf_net Conn84 [get_bd_intf_pins noc/S09_INI] [get_bd_intf_pins S09_INI]
  connect_bd_intf_net -intf_net Conn85 [get_bd_intf_pins noc/S10_INI] [get_bd_intf_pins S10_INI]
  connect_bd_intf_net -intf_net Conn86 [get_bd_intf_pins noc/S11_INI] [get_bd_intf_pins S11_INI]
  connect_bd_intf_net -intf_net Conn87 [get_bd_intf_pins dcmac_noc/S00_INIS] [get_bd_intf_pins S00_INIS]
  connect_bd_intf_net -intf_net Conn88 [get_bd_intf_pins virt_noc/S00_INI] [get_bd_intf_pins S00_INI1]
  connect_bd_intf_net -intf_net Conn89 [get_bd_intf_pins noc/M05_INI] [get_bd_intf_pins M05_INI]
  connect_bd_intf_net -intf_net Conn90 [get_bd_intf_pins noc/M04_INI] [get_bd_intf_pins M04_INI]
  connect_bd_intf_net -intf_net Conn91 [get_bd_intf_pins dcmac_noc/M00_INIS] [get_bd_intf_pins M00_INIS]
  connect_bd_intf_net -intf_net Conn92 [get_bd_intf_pins dcmac_noc/S00_INIS1] [get_bd_intf_pins S00_INIS1]
  connect_bd_intf_net -intf_net Conn93 [get_bd_intf_pins dcmac_noc/M00_INIS1] [get_bd_intf_pins M00_INIS1]
  connect_bd_intf_net -intf_net Conn94 [get_bd_intf_pins dcmac_noc/S00_INIS2] [get_bd_intf_pins S00_INIS2]
  connect_bd_intf_net -intf_net Conn95 [get_bd_intf_pins dcmac_noc/M00_INIS2] [get_bd_intf_pins M00_INIS2]
  connect_bd_intf_net -intf_net Conn96 [get_bd_intf_pins dcmac_noc/S00_INIS3] [get_bd_intf_pins S00_INIS3]
  connect_bd_intf_net -intf_net Conn97 [get_bd_intf_pins dcmac_noc/M00_INIS3] [get_bd_intf_pins M00_INIS3]
  connect_bd_intf_net -intf_net Conn98 [get_bd_intf_pins dcmac_noc/S00_INIS4] [get_bd_intf_pins S00_INIS4]
  connect_bd_intf_net -intf_net Conn99 [get_bd_intf_pins dcmac_noc/M00_INIS4] [get_bd_intf_pins M00_INIS4]
  connect_bd_intf_net -intf_net Conn100 [get_bd_intf_pins dcmac_noc/S00_INIS5] [get_bd_intf_pins S00_INIS5]
  connect_bd_intf_net -intf_net Conn101 [get_bd_intf_pins dcmac_noc/M00_INIS5] [get_bd_intf_pins M00_INIS5]
  connect_bd_intf_net -intf_net Conn102 [get_bd_intf_pins dcmac_noc/S00_INIS6] [get_bd_intf_pins S00_INIS6]
  connect_bd_intf_net -intf_net Conn103 [get_bd_intf_pins dcmac_noc/M00_INIS6] [get_bd_intf_pins M00_INIS6]
  connect_bd_intf_net -intf_net Conn104 [get_bd_intf_pins dcmac_noc/S00_INIS7] [get_bd_intf_pins S00_INIS7]
  connect_bd_intf_net -intf_net Conn105 [get_bd_intf_pins dcmac_noc/M00_INIS7] [get_bd_intf_pins M00_INIS7]
  connect_bd_intf_net -intf_net Conn106 [get_bd_intf_pins dcmac_noc/S00_INIS8] [get_bd_intf_pins S00_INIS8]
  connect_bd_intf_net -intf_net Conn107 [get_bd_intf_pins dcmac_noc/M00_INIS8] [get_bd_intf_pins M00_INIS8]
  connect_bd_intf_net -intf_net Conn108 [get_bd_intf_pins dcmac_noc/S00_INIS9] [get_bd_intf_pins S00_INIS9]
  connect_bd_intf_net -intf_net Conn109 [get_bd_intf_pins dcmac_noc/M00_INIS9] [get_bd_intf_pins M00_INIS9]
  connect_bd_intf_net -intf_net Conn110 [get_bd_intf_pins dcmac_noc/S00_INIS10] [get_bd_intf_pins S00_INIS10]
  connect_bd_intf_net -intf_net Conn111 [get_bd_intf_pins dcmac_noc/M00_INIS10] [get_bd_intf_pins M00_INIS10]
  connect_bd_intf_net -intf_net Conn112 [get_bd_intf_pins dcmac_noc/S00_INIS11] [get_bd_intf_pins S00_INIS11]
  connect_bd_intf_net -intf_net Conn113 [get_bd_intf_pins dcmac_noc/M00_INIS11] [get_bd_intf_pins M00_INIS11]
  connect_bd_intf_net -intf_net Conn114 [get_bd_intf_pins dcmac_noc/S00_INIS12] [get_bd_intf_pins S00_INIS12]
  connect_bd_intf_net -intf_net Conn115 [get_bd_intf_pins dcmac_noc/M00_INIS12] [get_bd_intf_pins M00_INIS12]
  connect_bd_intf_net -intf_net Conn116 [get_bd_intf_pins dcmac_noc/S00_INIS13] [get_bd_intf_pins S00_INIS13]
  connect_bd_intf_net -intf_net Conn117 [get_bd_intf_pins dcmac_noc/M00_INIS13] [get_bd_intf_pins M00_INIS13]
  connect_bd_intf_net -intf_net Conn118 [get_bd_intf_pins dcmac_noc/S00_INIS14] [get_bd_intf_pins S00_INIS14]
  connect_bd_intf_net -intf_net Conn119 [get_bd_intf_pins dcmac_noc/M00_INIS14] [get_bd_intf_pins M00_INIS14]
  connect_bd_intf_net -intf_net Conn120 [get_bd_intf_pins dcmac_noc/S00_INIS15] [get_bd_intf_pins S00_INIS15]
  connect_bd_intf_net -intf_net Conn121 [get_bd_intf_pins dcmac_noc/M00_INIS15] [get_bd_intf_pins M00_INIS15]
  connect_bd_intf_net -intf_net Conn122 [get_bd_intf_pins virt_noc/S00_INI1] [get_bd_intf_pins S00_INI2]
  connect_bd_intf_net -intf_net Conn123 [get_bd_intf_pins virt_noc/S00_INI2] [get_bd_intf_pins S00_INI3]
  connect_bd_intf_net -intf_net Conn124 [get_bd_intf_pins virt_noc/S00_INI3] [get_bd_intf_pins S00_INI4]
  connect_bd_intf_net -intf_net Conn125 [get_bd_intf_pins virt_noc/S00_INI4] [get_bd_intf_pins S00_INI5]
  connect_bd_intf_net -intf_net Conn126 [get_bd_intf_pins virt_noc/M00_INI1] [get_bd_intf_pins M00_INI1]
  connect_bd_intf_net -intf_net Conn127 [get_bd_intf_pins virt_noc/M00_INI2] [get_bd_intf_pins M00_INI2]
  connect_bd_intf_net -intf_net Conn128 [get_bd_intf_pins virt_noc/M00_INI3] [get_bd_intf_pins M00_INI3]
  connect_bd_intf_net -intf_net Conn129 [get_bd_intf_pins virt_noc/M00_INI4] [get_bd_intf_pins M00_INI4]
  connect_bd_intf_net -intf_net Conn130 [get_bd_intf_pins axi_noc_1/S00_INI] [get_bd_intf_pins S00_INI6]
  connect_bd_intf_net -intf_net S00_AXI_1 [get_bd_intf_pins noc/S00_AXI] [get_bd_intf_pins aved/CPM_PCIE_NOC_0]
  connect_bd_intf_net -intf_net S00_INI_1 [get_bd_intf_pins clk_rst_shell/S00_INI] [get_bd_intf_pins noc/M06_INI]
  connect_bd_intf_net -intf_net S01_AXI_1 [get_bd_intf_pins noc/S01_AXI] [get_bd_intf_pins aved/CPM_PCIE_NOC_1]
  connect_bd_intf_net -intf_net S02_AXI_1 [get_bd_intf_pins noc/S02_AXI] [get_bd_intf_pins aved/PMC_NOC_AXI_0]
  connect_bd_intf_net -intf_net S03_AXI_1 [get_bd_intf_pins noc/S03_AXI] [get_bd_intf_pins aved/LPD_AXI_NOC_0]
  connect_bd_intf_net -intf_net S12_INI_1 [get_bd_intf_pins S12_INI] [get_bd_intf_pins noc/S12_INI]
  connect_bd_intf_net -intf_net S13_INI_1 [get_bd_intf_pins S13_INI] [get_bd_intf_pins noc/S13_INI]
  connect_bd_intf_net -intf_net S14_INI_1 [get_bd_intf_pins S14_INI] [get_bd_intf_pins noc/S14_INI]
  connect_bd_intf_net -intf_net S15_INI_1 [get_bd_intf_pins S15_INI] [get_bd_intf_pins noc/S15_INI]
  connect_bd_intf_net -intf_net S16_INI_1 [get_bd_intf_pins S16_INI] [get_bd_intf_pins noc/S16_INI]
  connect_bd_intf_net -intf_net S17_INI_1 [get_bd_intf_pins S17_INI] [get_bd_intf_pins noc/S17_INI]
  connect_bd_intf_net -intf_net S18_INI_1 [get_bd_intf_pins S18_INI] [get_bd_intf_pins noc/S18_INI]
  connect_bd_intf_net -intf_net S19_INI_1 [get_bd_intf_pins S19_INI] [get_bd_intf_pins noc/S19_INI]
  connect_bd_intf_net -intf_net S20_INI1_1 [get_bd_intf_pins S20_INI] [get_bd_intf_pins noc/S20_INI1]
  connect_bd_intf_net -intf_net S21_INI1_1 [get_bd_intf_pins S21_INI] [get_bd_intf_pins noc/S21_INI1]
  connect_bd_intf_net -intf_net S22_INI1_1 [get_bd_intf_pins S22_INI] [get_bd_intf_pins noc/S22_INI1]
  connect_bd_intf_net -intf_net S23_INI1_1 [get_bd_intf_pins S23_INI] [get_bd_intf_pins noc/S23_INI1]
  connect_bd_intf_net -intf_net axi_noc_1_M00_AXI [get_bd_intf_pins axi_noc_1/M00_AXI] [get_bd_intf_pins aved/NOC_CPM_PCIE_0]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_0 [get_bd_intf_pins dfx_decoupler_0/s_intf_0] [get_bd_intf_pins noc/HBM00_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_1 [get_bd_intf_pins dfx_decoupler_0/s_intf_1] [get_bd_intf_pins noc/HBM01_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_2 [get_bd_intf_pins dfx_decoupler_0/s_intf_2] [get_bd_intf_pins noc/HBM02_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_3 [get_bd_intf_pins dfx_decoupler_0/s_intf_3] [get_bd_intf_pins noc/HBM03_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_4 [get_bd_intf_pins dfx_decoupler_0/s_intf_4] [get_bd_intf_pins noc/HBM04_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_5 [get_bd_intf_pins dfx_decoupler_0/s_intf_5] [get_bd_intf_pins noc/HBM05_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_6 [get_bd_intf_pins dfx_decoupler_0/s_intf_6] [get_bd_intf_pins noc/HBM06_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_7 [get_bd_intf_pins dfx_decoupler_0/s_intf_7] [get_bd_intf_pins noc/HBM07_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_8 [get_bd_intf_pins dfx_decoupler_0/s_intf_8] [get_bd_intf_pins noc/HBM08_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_9 [get_bd_intf_pins dfx_decoupler_0/s_intf_9] [get_bd_intf_pins noc/HBM09_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_10 [get_bd_intf_pins dfx_decoupler_0/s_intf_10] [get_bd_intf_pins noc/HBM10_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_11 [get_bd_intf_pins dfx_decoupler_0/s_intf_11] [get_bd_intf_pins noc/HBM11_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_12 [get_bd_intf_pins dfx_decoupler_0/s_intf_12] [get_bd_intf_pins noc/HBM12_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_13 [get_bd_intf_pins dfx_decoupler_0/s_intf_13] [get_bd_intf_pins noc/HBM13_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_14 [get_bd_intf_pins dfx_decoupler_0/s_intf_14] [get_bd_intf_pins noc/HBM14_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_15 [get_bd_intf_pins dfx_decoupler_0/s_intf_15] [get_bd_intf_pins noc/HBM15_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_16 [get_bd_intf_pins dfx_decoupler_0/s_intf_16] [get_bd_intf_pins noc/HBM16_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_17 [get_bd_intf_pins dfx_decoupler_0/s_intf_17] [get_bd_intf_pins noc/HBM17_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_18 [get_bd_intf_pins dfx_decoupler_0/s_intf_18] [get_bd_intf_pins noc/HBM18_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_19 [get_bd_intf_pins dfx_decoupler_0/s_intf_19] [get_bd_intf_pins noc/HBM19_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_20 [get_bd_intf_pins dfx_decoupler_0/s_intf_20] [get_bd_intf_pins noc/HBM20_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_21 [get_bd_intf_pins dfx_decoupler_0/s_intf_21] [get_bd_intf_pins noc/HBM21_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_22 [get_bd_intf_pins dfx_decoupler_0/s_intf_22] [get_bd_intf_pins noc/HBM22_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_23 [get_bd_intf_pins dfx_decoupler_0/s_intf_23] [get_bd_intf_pins noc/HBM23_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_24 [get_bd_intf_pins dfx_decoupler_0/s_intf_24] [get_bd_intf_pins noc/HBM24_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_25 [get_bd_intf_pins dfx_decoupler_0/s_intf_25] [get_bd_intf_pins noc/HBM25_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_26 [get_bd_intf_pins dfx_decoupler_0/s_intf_26] [get_bd_intf_pins noc/HBM26_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_27 [get_bd_intf_pins dfx_decoupler_0/s_intf_27] [get_bd_intf_pins noc/HBM27_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_28 [get_bd_intf_pins dfx_decoupler_0/s_intf_28] [get_bd_intf_pins noc/HBM28_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_29 [get_bd_intf_pins dfx_decoupler_0/s_intf_29] [get_bd_intf_pins noc/HBM29_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_30 [get_bd_intf_pins dfx_decoupler_0/s_intf_30] [get_bd_intf_pins noc/HBM30_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_31 [get_bd_intf_pins dfx_decoupler_0/s_intf_31] [get_bd_intf_pins noc/HBM31_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_32 [get_bd_intf_pins dfx_decoupler_0/s_intf_32] [get_bd_intf_pins noc/HBM32_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_33 [get_bd_intf_pins dfx_decoupler_0/s_intf_33] [get_bd_intf_pins noc/HBM33_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_34 [get_bd_intf_pins dfx_decoupler_0/s_intf_34] [get_bd_intf_pins noc/HBM34_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_35 [get_bd_intf_pins dfx_decoupler_0/s_intf_35] [get_bd_intf_pins noc/HBM35_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_36 [get_bd_intf_pins dfx_decoupler_0/s_intf_36] [get_bd_intf_pins noc/HBM36_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_37 [get_bd_intf_pins dfx_decoupler_0/s_intf_37] [get_bd_intf_pins noc/HBM37_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_38 [get_bd_intf_pins dfx_decoupler_0/s_intf_38] [get_bd_intf_pins noc/HBM38_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_39 [get_bd_intf_pins dfx_decoupler_0/s_intf_39] [get_bd_intf_pins noc/HBM39_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_40 [get_bd_intf_pins dfx_decoupler_0/s_intf_40] [get_bd_intf_pins noc/HBM40_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_41 [get_bd_intf_pins dfx_decoupler_0/s_intf_41] [get_bd_intf_pins noc/HBM41_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_42 [get_bd_intf_pins dfx_decoupler_0/s_intf_42] [get_bd_intf_pins noc/HBM42_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_43 [get_bd_intf_pins dfx_decoupler_0/s_intf_43] [get_bd_intf_pins noc/HBM43_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_44 [get_bd_intf_pins dfx_decoupler_0/s_intf_44] [get_bd_intf_pins noc/HBM44_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_45 [get_bd_intf_pins dfx_decoupler_0/s_intf_45] [get_bd_intf_pins noc/HBM45_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_46 [get_bd_intf_pins dfx_decoupler_0/s_intf_46] [get_bd_intf_pins noc/HBM46_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_47 [get_bd_intf_pins dfx_decoupler_0/s_intf_47] [get_bd_intf_pins noc/HBM47_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_48 [get_bd_intf_pins dfx_decoupler_0/s_intf_48] [get_bd_intf_pins noc/HBM48_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_49 [get_bd_intf_pins dfx_decoupler_0/s_intf_49] [get_bd_intf_pins noc/HBM49_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_50 [get_bd_intf_pins dfx_decoupler_0/s_intf_50] [get_bd_intf_pins noc/HBM50_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_51 [get_bd_intf_pins dfx_decoupler_0/s_intf_51] [get_bd_intf_pins noc/HBM51_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_52 [get_bd_intf_pins dfx_decoupler_0/s_intf_52] [get_bd_intf_pins noc/HBM52_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_53 [get_bd_intf_pins dfx_decoupler_0/s_intf_53] [get_bd_intf_pins noc/HBM53_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_54 [get_bd_intf_pins dfx_decoupler_0/s_intf_54] [get_bd_intf_pins noc/HBM54_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_55 [get_bd_intf_pins dfx_decoupler_0/s_intf_55] [get_bd_intf_pins noc/HBM55_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_56 [get_bd_intf_pins dfx_decoupler_0/s_intf_56] [get_bd_intf_pins noc/HBM56_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_57 [get_bd_intf_pins dfx_decoupler_0/s_intf_57] [get_bd_intf_pins noc/HBM57_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_58 [get_bd_intf_pins dfx_decoupler_0/s_intf_58] [get_bd_intf_pins noc/HBM58_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_59 [get_bd_intf_pins dfx_decoupler_0/s_intf_59] [get_bd_intf_pins noc/HBM59_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_60 [get_bd_intf_pins dfx_decoupler_0/s_intf_60] [get_bd_intf_pins noc/HBM60_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_61 [get_bd_intf_pins dfx_decoupler_0/s_intf_61] [get_bd_intf_pins noc/HBM61_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_62 [get_bd_intf_pins dfx_decoupler_0/s_intf_62] [get_bd_intf_pins noc/HBM62_AXI]
  connect_bd_intf_net -intf_net dfx_decoupler_0_s_intf_63 [get_bd_intf_pins dfx_decoupler_0/s_intf_63] [get_bd_intf_pins noc/HBM63_AXI]
  connect_bd_intf_net -intf_net noc_M00_AXI [get_bd_intf_pins noc/M00_AXI] [get_bd_intf_pins aved/s_axi_pcie_mgmt_slr0]
  connect_bd_intf_net -intf_net noc_M02_AXI [get_bd_intf_pins noc/M02_AXI] [get_bd_intf_pins aved/NOC_PMC_AXI_0]

  # Create port connections
  connect_bd_net -net aclk0_1  [get_bd_pins aved/pl0_ref_clk] \
  [get_bd_pins noc/aclk0] \
  [get_bd_pins pl0_ref_clk] \
  [get_bd_pins clk_rst_shell/pl0_ref_clk]
  connect_bd_net -net aclk1_1  [get_bd_pins aved/cpm_pcie_noc_axi1_clk] \
  [get_bd_pins noc/aclk1]
  connect_bd_net -net aclk2_1  [get_bd_pins aved/pmc_axi_noc_axi0_clk] \
  [get_bd_pins noc/aclk2]
  connect_bd_net -net aclk3_1  [get_bd_pins aved/lpd_axi_noc_clk] \
  [get_bd_pins noc/aclk3]
  connect_bd_net -net aclk4_1  [get_bd_pins aved/cpm_pcie_noc_axi0_clk] \
  [get_bd_pins noc/aclk4] \
  [get_bd_pins axi_noc_1/aclk0]
  connect_bd_net -net aclk6_1  [get_bd_pins aved/noc_pmc_axi_axi0_clk] \
  [get_bd_pins noc/aclk6]
  connect_bd_net -net aresetn_1  [get_bd_pins aved/pl0_resetn] \
  [get_bd_pins clk_rst_shell/aresetn]
  connect_bd_net -net aved_eos  [get_bd_pins aved/eos] \
  [get_bd_pins util_vector_logic_0/Op1]
  connect_bd_net -net aved_noc_cpm_pcie_axi0_clk  [get_bd_pins aved/noc_cpm_pcie_axi0_clk] \
  [get_bd_pins axi_noc_1/aclk1]
  connect_bd_net -net aved_pl3_ref_clk  [get_bd_pins aved/pl3_ref_clk] \
  [get_bd_pins clk_wizard_0/clk_in1] \
  [get_bd_pins pl3_ref_clk] \
  [get_bd_pins clk_rst_shell/refclk]
  connect_bd_net -net aved_pl3_resetn  [get_bd_pins aved/pl3_resetn] \
  [get_bd_pins pl3_resetn]
  connect_bd_net -net aved_resetn_pl_periph  [get_bd_pins aved/resetn_pl_periph] \
  [get_bd_pins resetn_pl_periph] \
  [get_bd_pins proc_sys_reset_1/ext_reset_in]
  connect_bd_net -net clk_rst_shell_clk_out1  [get_bd_pins clk_rst_shell/service_clk] \
  [get_bd_pins clk_out2]
  connect_bd_net -net clk_rst_shell_clk_out2  [get_bd_pins clk_rst_shell/slash_clk] \
  [get_bd_pins clk_out3]
  connect_bd_net -net clk_rst_shell_peripheral_aresetn  [get_bd_pins clk_rst_shell/service_arstn] \
  [get_bd_pins peripheral_aresetn1]
  connect_bd_net -net clk_rst_shell_peripheral_aresetn1  [get_bd_pins clk_rst_shell/slash_arstn] \
  [get_bd_pins peripheral_aresetn2]
  connect_bd_net -net clk_wizard_0_clk_out1  [get_bd_pins clk_wizard_0/clk_out1] \
  [get_bd_pins clk_out1] \
  [get_bd_pins noc/aclk5] \
  [get_bd_pins proc_sys_reset_1/slowest_sync_clk] \
  [get_bd_pins dfx_decoupler_0/intf_0_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_1_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_2_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_3_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_4_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_5_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_6_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_7_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_8_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_9_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_10_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_11_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_12_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_13_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_14_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_15_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_16_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_17_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_18_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_19_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_20_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_21_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_22_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_23_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_24_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_25_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_26_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_27_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_28_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_29_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_30_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_31_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_32_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_33_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_34_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_35_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_36_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_37_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_38_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_39_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_40_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_41_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_42_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_43_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_44_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_45_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_46_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_47_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_48_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_49_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_50_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_51_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_52_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_53_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_54_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_55_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_56_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_57_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_58_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_59_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_60_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_61_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_62_aclk] \
  [get_bd_pins dfx_decoupler_0/intf_63_aclk]
  connect_bd_net -net proc_sys_reset_1_peripheral_aresetn  [get_bd_pins proc_sys_reset_1/peripheral_aresetn] \
  [get_bd_pins dfx_decoupler_0/intf_0_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_1_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_2_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_3_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_4_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_5_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_6_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_7_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_8_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_9_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_10_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_11_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_12_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_13_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_14_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_15_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_16_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_17_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_18_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_19_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_20_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_21_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_22_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_23_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_24_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_25_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_26_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_27_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_28_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_29_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_30_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_31_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_32_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_33_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_34_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_35_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_36_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_37_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_38_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_39_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_40_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_41_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_42_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_43_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_44_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_45_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_46_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_47_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_48_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_49_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_50_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_51_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_52_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_53_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_54_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_55_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_56_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_57_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_58_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_59_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_60_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_61_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_62_arstn] \
  [get_bd_pins dfx_decoupler_0/intf_63_arstn]
  connect_bd_net -net util_vector_logic_0_Res  [get_bd_pins util_vector_logic_0/Res] \
  [get_bd_pins dfx_decoupler_0/decouple]

  # Restore current instance
  current_bd_instance $oldCurInst
}


# Procedure to create entire design; Provide argument to make
# procedure reusable. If parentCell is "", will use root.
proc create_root_design { parentCell } {

  variable script_folder
  variable design_name

  if { $parentCell eq "" } {
     set parentCell [get_bd_cells /]
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

  set_property -dict [list \
  SRC_RM_MAP./service_layer.service_layer {service_layer_inst_0} \
  SRC_RM_MAP./slash.slash_base {slash_base_inst_0} \
] [get_bd_designs $design_name]


  # Create interface ports
  set CH0_DDR4_0_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:ddr4_rtl:1.0 CH0_DDR4_0_0 ]

  set sys_clk0_0 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 sys_clk0_0 ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ {200000000} \
   ] $sys_clk0_0

  set CH0_DDR4_0_1 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:ddr4_rtl:1.0 CH0_DDR4_0_1 ]

  set sys_clk0_1 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 sys_clk0_1 ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ {200000000} \
   ] $sys_clk0_1

  set hbm_ref_clk_0 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 hbm_ref_clk_0 ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ {200000000} \
   ] $hbm_ref_clk_0

  set hbm_ref_clk_1 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 hbm_ref_clk_1 ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ {200000000} \
   ] $hbm_ref_clk_1

  set gt_pcie_refclk [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 gt_pcie_refclk ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ {100000000} \
   ] $gt_pcie_refclk

  set gt_pciea1 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 gt_pciea1 ]

  set smbus_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 smbus_0 ]

  set qsfp0_322mhz [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 qsfp0_322mhz ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ {322265625} \
   ] $qsfp0_322mhz

  set qsfp2_322mhz [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 qsfp2_322mhz ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ {322265625} \
   ] $qsfp2_322mhz

  set qsfp0_4x [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp0_4x ]

  set qsfp2_4x [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp2_4x ]

  set qsfp1_4x [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp1_4x ]

  set qsfp3_4x [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp3_4x ]


  # Create ports

  # Create instance: static_region
  create_hier_cell_static_region [current_bd_instance .] static_region

  # Create instance: slash, and set properties
  set slash [ create_bd_cell -type container -reference slash_base slash ]
  set_property -dict [list \
    CONFIG.ACTIVE_SIM_BD {slash_base.bd} \
    CONFIG.ACTIVE_SYNTH_BD {slash_base.bd} \
    CONFIG.ENABLE_DFX {false} \
    CONFIG.LIST_SIM_BD {slash_base.bd} \
    CONFIG.LIST_SYNTH_BD {slash_base.bd} \
    CONFIG.LOCK_PROPAGATE {false} \
  ] $slash


  set_property SELECTED_SIM_MODEL rtl  $slash
  set_property APERTURES {{0x40_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_00]
  set_property APERTURES {{0x40_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_01]
  set_property APERTURES {{0x40_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_02]
  set_property APERTURES {{0x40_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_03]
  set_property APERTURES {{0x40_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_04]
  set_property APERTURES {{0x40_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_05]
  set_property APERTURES {{0x40_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_06]
  set_property APERTURES {{0x40_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_07]
  set_property APERTURES {{0x41_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_08]
  set_property APERTURES {{0x41_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_09]
  set_property APERTURES {{0x41_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_10]
  set_property APERTURES {{0x41_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_11]
  set_property APERTURES {{0x41_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_12]
  set_property APERTURES {{0x41_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_13]
  set_property APERTURES {{0x41_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_14]
  set_property APERTURES {{0x41_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_15]
  set_property APERTURES {{0x42_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_16]
  set_property APERTURES {{0x42_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_17]
  set_property APERTURES {{0x42_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_18]
  set_property APERTURES {{0x42_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_19]
  set_property APERTURES {{0x42_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_20]
  set_property APERTURES {{0x42_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_21]
  set_property APERTURES {{0x42_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_22]
  set_property APERTURES {{0x42_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_23]
  set_property APERTURES {{0x43_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_24]
  set_property APERTURES {{0x43_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_25]
  set_property APERTURES {{0x43_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_26]
  set_property APERTURES {{0x43_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_27]
  set_property APERTURES {{0x43_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_28]
  set_property APERTURES {{0x43_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_29]
  set_property APERTURES {{0x43_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_30]
  set_property APERTURES {{0x43_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_31]
  set_property APERTURES {{0x44_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_32]
  set_property APERTURES {{0x44_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_33]
  set_property APERTURES {{0x44_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_34]
  set_property APERTURES {{0x44_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_35]
  set_property APERTURES {{0x44_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_36]
  set_property APERTURES {{0x44_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_37]
  set_property APERTURES {{0x44_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_38]
  set_property APERTURES {{0x44_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_39]
  set_property APERTURES {{0x45_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_40]
  set_property APERTURES {{0x45_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_41]
  set_property APERTURES {{0x45_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_42]
  set_property APERTURES {{0x45_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_43]
  set_property APERTURES {{0x45_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_44]
  set_property APERTURES {{0x45_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_45]
  set_property APERTURES {{0x45_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_46]
  set_property APERTURES {{0x45_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_47]
  set_property APERTURES {{0x46_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_48]
  set_property APERTURES {{0x46_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_49]
  set_property APERTURES {{0x46_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_50]
  set_property APERTURES {{0x46_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_51]
  set_property APERTURES {{0x46_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_52]
  set_property APERTURES {{0x46_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_53]
  set_property APERTURES {{0x46_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_54]
  set_property APERTURES {{0x46_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_55]
  set_property APERTURES {{0x47_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_56]
  set_property APERTURES {{0x47_0000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_57]
  set_property APERTURES {{0x47_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_58]
  set_property APERTURES {{0x47_4000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_59]
  set_property APERTURES {{0x47_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_60]
  set_property APERTURES {{0x47_8000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_61]
  set_property APERTURES {{0x47_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_62]
  set_property APERTURES {{0x47_C000_0000 1G}} [get_bd_intf_pins /slash/HBM_AXI_63]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/HBM_VNOC_INI_00]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/HBM_VNOC_INI_01]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/HBM_VNOC_INI_02]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/HBM_VNOC_INI_03]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/HBM_VNOC_INI_04]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/HBM_VNOC_INI_05]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/HBM_VNOC_INI_06]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/HBM_VNOC_INI_07]
  set_property APERTURES {{0x0 2G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/M00_INI]
  set_property APERTURES {{0x0 2G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/M01_INI]
  set_property APERTURES {{0x0 2G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/M02_INI]
  set_property APERTURES {{0x0 2G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_pins /slash/M03_INI]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_pins /slash/QDMA_SLAVE_BRIDGE_0]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_pins /slash/SL_VIRT_00]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_pins /slash/SL_VIRT_01]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_pins /slash/SL_VIRT_02]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_pins /slash/SL_VIRT_03]
  set_property APERTURES {{0x202_0000_0000 128M}} [get_bd_intf_pins /slash/S_AXILITE_INI]

  # Create instance: service_layer, and set properties
  set service_layer [ create_bd_cell -type container -reference service_layer service_layer ]
  set_property -dict [list \
    CONFIG.ACTIVE_SIM_BD {service_layer.bd} \
    CONFIG.ACTIVE_SYNTH_BD {service_layer.bd} \
    CONFIG.ENABLE_DFX {false} \
    CONFIG.LIST_SIM_BD {service_layer.bd} \
    CONFIG.LIST_SYNTH_BD {service_layer.bd} \
    CONFIG.LOCK_PROPAGATE {false} \
  ] $service_layer


  set_property SELECTED_SIM_MODEL rtl  $service_layer
  set_property APERTURES {{0xE000_0000 256M}} [get_bd_intf_pins /service_layer/M_QDMA_SLV_BRIDGE]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/M_VIRT_0]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/M_VIRT_1]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/M_VIRT_2]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/M_VIRT_3]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/SL2NOC_0]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/SL2NOC_1]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/SL2NOC_2]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/SL2NOC_3]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/SL2NOC_4]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/SL2NOC_5]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/SL2NOC_6]
  set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_pins /service_layer/SL2NOC_7]
  set_property APERTURES {{0x203_0000_0000 128M}} [get_bd_intf_pins /service_layer/S_AXILITE_INI]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_pins /service_layer/S_QDMA_SLV_BRIDGE]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_pins /service_layer/S_VIRT_00]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_pins /service_layer/S_VIRT_01]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_pins /service_layer/S_VIRT_02]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_pins /service_layer/S_VIRT_03]

  # Create interface connections
  connect_bd_intf_net -intf_net S_AXILITE_INI_1 [get_bd_intf_pins service_layer/S_AXILITE_INI] [get_bd_intf_pins static_region/M05_INI]
  connect_bd_intf_net -intf_net S_AXILITE_INI_2 [get_bd_intf_pins slash/S_AXILITE_INI] [get_bd_intf_pins static_region/M04_INI]
  connect_bd_intf_net -intf_net S_DCMAC_INIS0_1 [get_bd_intf_pins service_layer/S_DCMAC_INIS0] [get_bd_intf_pins static_region/M00_INIS]
  connect_bd_intf_net -intf_net S_DCMAC_INIS0_2 [get_bd_intf_pins slash/S_DCMAC_INIS0] [get_bd_intf_pins static_region/M00_INIS8]
  connect_bd_intf_net -intf_net S_DCMAC_INIS1_1 [get_bd_intf_pins service_layer/S_DCMAC_INIS1] [get_bd_intf_pins static_region/M00_INIS1]
  connect_bd_intf_net -intf_net S_DCMAC_INIS1_2 [get_bd_intf_pins slash/S_DCMAC_INIS1] [get_bd_intf_pins static_region/M00_INIS9]
  connect_bd_intf_net -intf_net S_DCMAC_INIS2_1 [get_bd_intf_pins service_layer/S_DCMAC_INIS2] [get_bd_intf_pins static_region/M00_INIS2]
  connect_bd_intf_net -intf_net S_DCMAC_INIS2_2 [get_bd_intf_pins slash/S_DCMAC_INIS2] [get_bd_intf_pins static_region/M00_INIS10]
  connect_bd_intf_net -intf_net S_DCMAC_INIS3_1 [get_bd_intf_pins service_layer/S_DCMAC_INIS3] [get_bd_intf_pins static_region/M00_INIS3]
  connect_bd_intf_net -intf_net S_DCMAC_INIS3_2 [get_bd_intf_pins slash/S_DCMAC_INIS3] [get_bd_intf_pins static_region/M00_INIS11]
  connect_bd_intf_net -intf_net S_DCMAC_INIS4_1 [get_bd_intf_pins service_layer/S_DCMAC_INIS4] [get_bd_intf_pins static_region/M00_INIS4]
  connect_bd_intf_net -intf_net S_DCMAC_INIS4_2 [get_bd_intf_pins slash/S_DCMAC_INIS4] [get_bd_intf_pins static_region/M00_INIS12]
  connect_bd_intf_net -intf_net S_DCMAC_INIS5_1 [get_bd_intf_pins service_layer/S_DCMAC_INIS5] [get_bd_intf_pins static_region/M00_INIS5]
  connect_bd_intf_net -intf_net S_DCMAC_INIS5_2 [get_bd_intf_pins slash/S_DCMAC_INIS5] [get_bd_intf_pins static_region/M00_INIS13]
  connect_bd_intf_net -intf_net S_DCMAC_INIS6_1 [get_bd_intf_pins service_layer/S_DCMAC_INIS6] [get_bd_intf_pins static_region/M00_INIS6]
  connect_bd_intf_net -intf_net S_DCMAC_INIS6_2 [get_bd_intf_pins slash/S_DCMAC_INIS6] [get_bd_intf_pins static_region/M00_INIS14]
  connect_bd_intf_net -intf_net S_DCMAC_INIS7_1 [get_bd_intf_pins service_layer/S_DCMAC_INIS7] [get_bd_intf_pins static_region/M00_INIS7]
  connect_bd_intf_net -intf_net S_DCMAC_INIS7_2 [get_bd_intf_pins slash/S_DCMAC_INIS7] [get_bd_intf_pins static_region/M00_INIS15]
  connect_bd_intf_net -intf_net S_QDMA_SLV_BRIDGE_1 [get_bd_intf_pins service_layer/S_QDMA_SLV_BRIDGE] [get_bd_intf_pins static_region/M00_INI4]
  connect_bd_intf_net -intf_net S_VIRT_00_1 [get_bd_intf_pins service_layer/S_VIRT_00] [get_bd_intf_pins static_region/M00_INI]
  connect_bd_intf_net -intf_net S_VIRT_01_1 [get_bd_intf_pins service_layer/S_VIRT_01] [get_bd_intf_pins static_region/M00_INI1]
  connect_bd_intf_net -intf_net S_VIRT_02_1 [get_bd_intf_pins service_layer/S_VIRT_02] [get_bd_intf_pins static_region/M00_INI2]
  connect_bd_intf_net -intf_net S_VIRT_03_1 [get_bd_intf_pins service_layer/S_VIRT_03] [get_bd_intf_pins static_region/M00_INI3]
  connect_bd_intf_net -intf_net gt_pcie_refclk_1 [get_bd_intf_ports gt_pcie_refclk] [get_bd_intf_pins static_region/gt_pcie_refclk]
  connect_bd_intf_net -intf_net hbm_ref_clk_0_1 [get_bd_intf_ports hbm_ref_clk_0] [get_bd_intf_pins static_region/hbm_ref_clk_0]
  connect_bd_intf_net -intf_net hbm_ref_clk_1_1 [get_bd_intf_ports hbm_ref_clk_1] [get_bd_intf_pins static_region/hbm_ref_clk_1]
  connect_bd_intf_net -intf_net qsfp0_322mhz_0_1 [get_bd_intf_ports qsfp0_322mhz] [get_bd_intf_pins service_layer/qsfp0_322mhz]
  connect_bd_intf_net -intf_net qsfp2_322mhz_0_1 [get_bd_intf_ports qsfp2_322mhz] [get_bd_intf_pins service_layer/qsfp2_322mhz]
  connect_bd_intf_net -intf_net service_layer_M00_INI [get_bd_intf_pins service_layer/SL2NOC_0] [get_bd_intf_pins static_region/S12_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI1 [get_bd_intf_pins service_layer/SL2NOC_1] [get_bd_intf_pins static_region/S13_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI2 [get_bd_intf_pins service_layer/SL2NOC_2] [get_bd_intf_pins static_region/S14_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI3 [get_bd_intf_pins service_layer/SL2NOC_3] [get_bd_intf_pins static_region/S15_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI4 [get_bd_intf_pins service_layer/SL2NOC_4] [get_bd_intf_pins static_region/S16_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI5 [get_bd_intf_pins service_layer/SL2NOC_5] [get_bd_intf_pins static_region/S17_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI6 [get_bd_intf_pins service_layer/SL2NOC_6] [get_bd_intf_pins static_region/S18_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI7 [get_bd_intf_pins service_layer/SL2NOC_7] [get_bd_intf_pins static_region/S19_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI12 [get_bd_intf_pins service_layer/M_VIRT_0] [get_bd_intf_pins static_region/S20_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI13 [get_bd_intf_pins service_layer/M_VIRT_1] [get_bd_intf_pins static_region/S21_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI14 [get_bd_intf_pins service_layer/M_VIRT_2] [get_bd_intf_pins static_region/S22_INI]
  connect_bd_intf_net -intf_net service_layer_M00_INI15 [get_bd_intf_pins service_layer/M_VIRT_3] [get_bd_intf_pins static_region/S23_INI]
  connect_bd_intf_net -intf_net service_layer_M_DCMAC_INIS0 [get_bd_intf_pins service_layer/M_DCMAC_INIS0] [get_bd_intf_pins static_region/S00_INIS8]
  connect_bd_intf_net -intf_net service_layer_M_DCMAC_INIS1 [get_bd_intf_pins service_layer/M_DCMAC_INIS1] [get_bd_intf_pins static_region/S00_INIS9]
  connect_bd_intf_net -intf_net service_layer_M_DCMAC_INIS2 [get_bd_intf_pins service_layer/M_DCMAC_INIS2] [get_bd_intf_pins static_region/S00_INIS10]
  connect_bd_intf_net -intf_net service_layer_M_DCMAC_INIS3 [get_bd_intf_pins service_layer/M_DCMAC_INIS3] [get_bd_intf_pins static_region/S00_INIS11]
  connect_bd_intf_net -intf_net service_layer_M_DCMAC_INIS4 [get_bd_intf_pins service_layer/M_DCMAC_INIS4] [get_bd_intf_pins static_region/S00_INIS12]
  connect_bd_intf_net -intf_net service_layer_M_DCMAC_INIS5 [get_bd_intf_pins service_layer/M_DCMAC_INIS5] [get_bd_intf_pins static_region/S00_INIS13]
  connect_bd_intf_net -intf_net service_layer_M_DCMAC_INIS6 [get_bd_intf_pins service_layer/M_DCMAC_INIS6] [get_bd_intf_pins static_region/S00_INIS14]
  connect_bd_intf_net -intf_net service_layer_M_DCMAC_INIS7 [get_bd_intf_pins service_layer/M_DCMAC_INIS7] [get_bd_intf_pins static_region/S00_INIS15]
  connect_bd_intf_net -intf_net service_layer_M_QDMA_SLV_BRIDGE [get_bd_intf_pins service_layer/M_QDMA_SLV_BRIDGE] [get_bd_intf_pins static_region/S00_INI6]
  connect_bd_intf_net -intf_net service_layer_qsfp0_4x [get_bd_intf_ports qsfp0_4x] [get_bd_intf_pins service_layer/qsfp0_4x]
  connect_bd_intf_net -intf_net service_layer_qsfp1_4x [get_bd_intf_ports qsfp1_4x] [get_bd_intf_pins service_layer/qsfp1_4x]
  connect_bd_intf_net -intf_net service_layer_qsfp2_4x [get_bd_intf_ports qsfp2_4x] [get_bd_intf_pins service_layer/qsfp2_4x]
  connect_bd_intf_net -intf_net service_layer_qsfp3_4x [get_bd_intf_ports qsfp3_4x] [get_bd_intf_pins service_layer/qsfp3_4x]
  connect_bd_intf_net -intf_net slash_HBM_VNOC_INI_00 [get_bd_intf_pins slash/HBM_VNOC_INI_00] [get_bd_intf_pins static_region/S04_INI]
  connect_bd_intf_net -intf_net slash_HBM_VNOC_INI_01 [get_bd_intf_pins slash/HBM_VNOC_INI_01] [get_bd_intf_pins static_region/S05_INI]
  connect_bd_intf_net -intf_net slash_HBM_VNOC_INI_02 [get_bd_intf_pins slash/HBM_VNOC_INI_02] [get_bd_intf_pins static_region/S06_INI]
  connect_bd_intf_net -intf_net slash_HBM_VNOC_INI_03 [get_bd_intf_pins slash/HBM_VNOC_INI_03] [get_bd_intf_pins static_region/S07_INI]
  connect_bd_intf_net -intf_net slash_HBM_VNOC_INI_04 [get_bd_intf_pins slash/HBM_VNOC_INI_04] [get_bd_intf_pins static_region/S08_INI]
  connect_bd_intf_net -intf_net slash_HBM_VNOC_INI_05 [get_bd_intf_pins slash/HBM_VNOC_INI_05] [get_bd_intf_pins static_region/S09_INI]
  connect_bd_intf_net -intf_net slash_HBM_VNOC_INI_06 [get_bd_intf_pins slash/HBM_VNOC_INI_06] [get_bd_intf_pins static_region/S10_INI]
  connect_bd_intf_net -intf_net slash_HBM_VNOC_INI_07 [get_bd_intf_pins slash/HBM_VNOC_INI_07] [get_bd_intf_pins static_region/S11_INI]
  connect_bd_intf_net -intf_net slash_M00_INI [get_bd_intf_pins slash/M00_INI] [get_bd_intf_pins static_region/S00_INI]
  connect_bd_intf_net -intf_net slash_M01_INI [get_bd_intf_pins slash/M01_INI] [get_bd_intf_pins static_region/S01_INI]
  connect_bd_intf_net -intf_net slash_M02_INI [get_bd_intf_pins slash/M02_INI] [get_bd_intf_pins static_region/S02_INI]
  connect_bd_intf_net -intf_net slash_M03_INI [get_bd_intf_pins slash/M03_INI] [get_bd_intf_pins static_region/S03_INI]
  connect_bd_intf_net -intf_net slash_M_AXI0 [get_bd_intf_pins slash/HBM_AXI_00] [get_bd_intf_pins static_region/rp_intf_0]
  connect_bd_intf_net -intf_net slash_M_AXI1 [get_bd_intf_pins slash/HBM_AXI_01] [get_bd_intf_pins static_region/rp_intf_1]
  connect_bd_intf_net -intf_net slash_M_AXI2 [get_bd_intf_pins slash/HBM_AXI_02] [get_bd_intf_pins static_region/rp_intf_2]
  connect_bd_intf_net -intf_net slash_M_AXI3 [get_bd_intf_pins slash/HBM_AXI_03] [get_bd_intf_pins static_region/rp_intf_3]
  connect_bd_intf_net -intf_net slash_M_AXI4 [get_bd_intf_pins slash/HBM_AXI_04] [get_bd_intf_pins static_region/rp_intf_4]
  connect_bd_intf_net -intf_net slash_M_AXI5 [get_bd_intf_pins slash/HBM_AXI_05] [get_bd_intf_pins static_region/rp_intf_5]
  connect_bd_intf_net -intf_net slash_M_AXI6 [get_bd_intf_pins slash/HBM_AXI_06] [get_bd_intf_pins static_region/rp_intf_6]
  connect_bd_intf_net -intf_net slash_M_AXI7 [get_bd_intf_pins slash/HBM_AXI_07] [get_bd_intf_pins static_region/rp_intf_7]
  connect_bd_intf_net -intf_net slash_M_AXI8 [get_bd_intf_pins slash/HBM_AXI_08] [get_bd_intf_pins static_region/rp_intf_8]
  connect_bd_intf_net -intf_net slash_M_AXI9 [get_bd_intf_pins slash/HBM_AXI_09] [get_bd_intf_pins static_region/rp_intf_9]
  connect_bd_intf_net -intf_net slash_M_AXI10 [get_bd_intf_pins slash/HBM_AXI_10] [get_bd_intf_pins static_region/rp_intf_10]
  connect_bd_intf_net -intf_net slash_M_AXI11 [get_bd_intf_pins slash/HBM_AXI_11] [get_bd_intf_pins static_region/rp_intf_11]
  connect_bd_intf_net -intf_net slash_M_AXI12 [get_bd_intf_pins slash/HBM_AXI_12] [get_bd_intf_pins static_region/rp_intf_12]
  connect_bd_intf_net -intf_net slash_M_AXI13 [get_bd_intf_pins slash/HBM_AXI_13] [get_bd_intf_pins static_region/rp_intf_13]
  connect_bd_intf_net -intf_net slash_M_AXI14 [get_bd_intf_pins slash/HBM_AXI_14] [get_bd_intf_pins static_region/rp_intf_14]
  connect_bd_intf_net -intf_net slash_M_AXI15 [get_bd_intf_pins slash/HBM_AXI_15] [get_bd_intf_pins static_region/rp_intf_15]
  connect_bd_intf_net -intf_net slash_M_AXI16 [get_bd_intf_pins slash/HBM_AXI_16] [get_bd_intf_pins static_region/rp_intf_16]
  connect_bd_intf_net -intf_net slash_M_AXI17 [get_bd_intf_pins slash/HBM_AXI_17] [get_bd_intf_pins static_region/rp_intf_17]
  connect_bd_intf_net -intf_net slash_M_AXI18 [get_bd_intf_pins slash/HBM_AXI_18] [get_bd_intf_pins static_region/rp_intf_18]
  connect_bd_intf_net -intf_net slash_M_AXI19 [get_bd_intf_pins slash/HBM_AXI_19] [get_bd_intf_pins static_region/rp_intf_19]
  connect_bd_intf_net -intf_net slash_M_AXI20 [get_bd_intf_pins slash/HBM_AXI_20] [get_bd_intf_pins static_region/rp_intf_20]
  connect_bd_intf_net -intf_net slash_M_AXI21 [get_bd_intf_pins slash/HBM_AXI_21] [get_bd_intf_pins static_region/rp_intf_21]
  connect_bd_intf_net -intf_net slash_M_AXI22 [get_bd_intf_pins slash/HBM_AXI_22] [get_bd_intf_pins static_region/rp_intf_22]
  connect_bd_intf_net -intf_net slash_M_AXI23 [get_bd_intf_pins slash/HBM_AXI_23] [get_bd_intf_pins static_region/rp_intf_23]
  connect_bd_intf_net -intf_net slash_M_AXI24 [get_bd_intf_pins slash/HBM_AXI_24] [get_bd_intf_pins static_region/rp_intf_24]
  connect_bd_intf_net -intf_net slash_M_AXI25 [get_bd_intf_pins slash/HBM_AXI_25] [get_bd_intf_pins static_region/rp_intf_25]
  connect_bd_intf_net -intf_net slash_M_AXI26 [get_bd_intf_pins slash/HBM_AXI_26] [get_bd_intf_pins static_region/rp_intf_26]
  connect_bd_intf_net -intf_net slash_M_AXI27 [get_bd_intf_pins slash/HBM_AXI_27] [get_bd_intf_pins static_region/rp_intf_27]
  connect_bd_intf_net -intf_net slash_M_AXI28 [get_bd_intf_pins slash/HBM_AXI_28] [get_bd_intf_pins static_region/rp_intf_28]
  connect_bd_intf_net -intf_net slash_M_AXI29 [get_bd_intf_pins slash/HBM_AXI_29] [get_bd_intf_pins static_region/rp_intf_29]
  connect_bd_intf_net -intf_net slash_M_AXI30 [get_bd_intf_pins slash/HBM_AXI_30] [get_bd_intf_pins static_region/rp_intf_30]
  connect_bd_intf_net -intf_net slash_M_AXI31 [get_bd_intf_pins slash/HBM_AXI_31] [get_bd_intf_pins static_region/rp_intf_31]
  connect_bd_intf_net -intf_net slash_M_AXI32 [get_bd_intf_pins slash/HBM_AXI_32] [get_bd_intf_pins static_region/rp_intf_32]
  connect_bd_intf_net -intf_net slash_M_AXI33 [get_bd_intf_pins slash/HBM_AXI_33] [get_bd_intf_pins static_region/rp_intf_33]
  connect_bd_intf_net -intf_net slash_M_AXI34 [get_bd_intf_pins slash/HBM_AXI_34] [get_bd_intf_pins static_region/rp_intf_34]
  connect_bd_intf_net -intf_net slash_M_AXI35 [get_bd_intf_pins slash/HBM_AXI_35] [get_bd_intf_pins static_region/rp_intf_35]
  connect_bd_intf_net -intf_net slash_M_AXI36 [get_bd_intf_pins slash/HBM_AXI_36] [get_bd_intf_pins static_region/rp_intf_36]
  connect_bd_intf_net -intf_net slash_M_AXI37 [get_bd_intf_pins slash/HBM_AXI_37] [get_bd_intf_pins static_region/rp_intf_37]
  connect_bd_intf_net -intf_net slash_M_AXI38 [get_bd_intf_pins slash/HBM_AXI_38] [get_bd_intf_pins static_region/rp_intf_38]
  connect_bd_intf_net -intf_net slash_M_AXI39 [get_bd_intf_pins slash/HBM_AXI_39] [get_bd_intf_pins static_region/rp_intf_39]
  connect_bd_intf_net -intf_net slash_M_AXI40 [get_bd_intf_pins slash/HBM_AXI_40] [get_bd_intf_pins static_region/rp_intf_40]
  connect_bd_intf_net -intf_net slash_M_AXI41 [get_bd_intf_pins slash/HBM_AXI_41] [get_bd_intf_pins static_region/rp_intf_41]
  connect_bd_intf_net -intf_net slash_M_AXI42 [get_bd_intf_pins slash/HBM_AXI_42] [get_bd_intf_pins static_region/rp_intf_42]
  connect_bd_intf_net -intf_net slash_M_AXI43 [get_bd_intf_pins slash/HBM_AXI_43] [get_bd_intf_pins static_region/rp_intf_43]
  connect_bd_intf_net -intf_net slash_M_AXI44 [get_bd_intf_pins slash/HBM_AXI_44] [get_bd_intf_pins static_region/rp_intf_44]
  connect_bd_intf_net -intf_net slash_M_AXI45 [get_bd_intf_pins slash/HBM_AXI_45] [get_bd_intf_pins static_region/rp_intf_45]
  connect_bd_intf_net -intf_net slash_M_AXI46 [get_bd_intf_pins slash/HBM_AXI_46] [get_bd_intf_pins static_region/rp_intf_46]
  connect_bd_intf_net -intf_net slash_M_AXI47 [get_bd_intf_pins slash/HBM_AXI_47] [get_bd_intf_pins static_region/rp_intf_47]
  connect_bd_intf_net -intf_net slash_M_AXI48 [get_bd_intf_pins slash/HBM_AXI_48] [get_bd_intf_pins static_region/rp_intf_48]
  connect_bd_intf_net -intf_net slash_M_AXI49 [get_bd_intf_pins slash/HBM_AXI_49] [get_bd_intf_pins static_region/rp_intf_49]
  connect_bd_intf_net -intf_net slash_M_AXI50 [get_bd_intf_pins slash/HBM_AXI_50] [get_bd_intf_pins static_region/rp_intf_50]
  connect_bd_intf_net -intf_net slash_M_AXI51 [get_bd_intf_pins slash/HBM_AXI_51] [get_bd_intf_pins static_region/rp_intf_51]
  connect_bd_intf_net -intf_net slash_M_AXI52 [get_bd_intf_pins slash/HBM_AXI_52] [get_bd_intf_pins static_region/rp_intf_52]
  connect_bd_intf_net -intf_net slash_M_AXI53 [get_bd_intf_pins slash/HBM_AXI_53] [get_bd_intf_pins static_region/rp_intf_53]
  connect_bd_intf_net -intf_net slash_M_AXI54 [get_bd_intf_pins slash/HBM_AXI_54] [get_bd_intf_pins static_region/rp_intf_54]
  connect_bd_intf_net -intf_net slash_M_AXI55 [get_bd_intf_pins slash/HBM_AXI_55] [get_bd_intf_pins static_region/rp_intf_55]
  connect_bd_intf_net -intf_net slash_M_AXI56 [get_bd_intf_pins slash/HBM_AXI_56] [get_bd_intf_pins static_region/rp_intf_56]
  connect_bd_intf_net -intf_net slash_M_AXI57 [get_bd_intf_pins slash/HBM_AXI_57] [get_bd_intf_pins static_region/rp_intf_57]
  connect_bd_intf_net -intf_net slash_M_AXI58 [get_bd_intf_pins slash/HBM_AXI_58] [get_bd_intf_pins static_region/rp_intf_58]
  connect_bd_intf_net -intf_net slash_M_AXI59 [get_bd_intf_pins slash/HBM_AXI_59] [get_bd_intf_pins static_region/rp_intf_59]
  connect_bd_intf_net -intf_net slash_M_AXI60 [get_bd_intf_pins slash/HBM_AXI_60] [get_bd_intf_pins static_region/rp_intf_60]
  connect_bd_intf_net -intf_net slash_M_AXI61 [get_bd_intf_pins slash/HBM_AXI_61] [get_bd_intf_pins static_region/rp_intf_61]
  connect_bd_intf_net -intf_net slash_M_AXI62 [get_bd_intf_pins slash/HBM_AXI_62] [get_bd_intf_pins static_region/rp_intf_62]
  connect_bd_intf_net -intf_net slash_M_AXI63 [get_bd_intf_pins slash/HBM_AXI_63] [get_bd_intf_pins static_region/rp_intf_63]
  connect_bd_intf_net -intf_net slash_M_DCMAC_INIS0 [get_bd_intf_pins slash/M_DCMAC_INIS0] [get_bd_intf_pins static_region/S00_INIS]
  connect_bd_intf_net -intf_net slash_M_DCMAC_INIS1 [get_bd_intf_pins slash/M_DCMAC_INIS1] [get_bd_intf_pins static_region/S00_INIS1]
  connect_bd_intf_net -intf_net slash_M_DCMAC_INIS2 [get_bd_intf_pins slash/M_DCMAC_INIS2] [get_bd_intf_pins static_region/S00_INIS2]
  connect_bd_intf_net -intf_net slash_M_DCMAC_INIS3 [get_bd_intf_pins slash/M_DCMAC_INIS3] [get_bd_intf_pins static_region/S00_INIS3]
  connect_bd_intf_net -intf_net slash_M_DCMAC_INIS4 [get_bd_intf_pins slash/M_DCMAC_INIS4] [get_bd_intf_pins static_region/S00_INIS4]
  connect_bd_intf_net -intf_net slash_M_DCMAC_INIS5 [get_bd_intf_pins slash/M_DCMAC_INIS5] [get_bd_intf_pins static_region/S00_INIS5]
  connect_bd_intf_net -intf_net slash_M_DCMAC_INIS6 [get_bd_intf_pins slash/M_DCMAC_INIS6] [get_bd_intf_pins static_region/S00_INIS6]
  connect_bd_intf_net -intf_net slash_M_DCMAC_INIS7 [get_bd_intf_pins slash/M_DCMAC_INIS7] [get_bd_intf_pins static_region/S00_INIS7]
  connect_bd_intf_net -intf_net slash_QDMA_SLAVE_BRIDGE_0 [get_bd_intf_pins slash/QDMA_SLAVE_BRIDGE_0] [get_bd_intf_pins static_region/S00_INI5]
  connect_bd_intf_net -intf_net slash_SL_VIRT_00 [get_bd_intf_pins slash/SL_VIRT_00] [get_bd_intf_pins static_region/S00_INI1]
  connect_bd_intf_net -intf_net slash_SL_VIRT_01 [get_bd_intf_pins slash/SL_VIRT_01] [get_bd_intf_pins static_region/S00_INI2]
  connect_bd_intf_net -intf_net slash_SL_VIRT_02 [get_bd_intf_pins slash/SL_VIRT_02] [get_bd_intf_pins static_region/S00_INI3]
  connect_bd_intf_net -intf_net slash_SL_VIRT_03 [get_bd_intf_pins slash/SL_VIRT_03] [get_bd_intf_pins static_region/S00_INI4]
  connect_bd_intf_net -intf_net static_region_CH0_DDR4_0_0 [get_bd_intf_ports CH0_DDR4_0_0] [get_bd_intf_pins static_region/CH0_DDR4_0_0]
  connect_bd_intf_net -intf_net static_region_CH0_DDR4_0_1 [get_bd_intf_ports CH0_DDR4_0_1] [get_bd_intf_pins static_region/CH0_DDR4_0_1]
  connect_bd_intf_net -intf_net static_region_gt_pciea1 [get_bd_intf_ports gt_pciea1] [get_bd_intf_pins static_region/gt_pciea1]
  connect_bd_intf_net -intf_net static_region_smbus_0 [get_bd_intf_ports smbus_0] [get_bd_intf_pins static_region/smbus_0]
  connect_bd_intf_net -intf_net sys_clk0_0_1 [get_bd_intf_ports sys_clk0_0] [get_bd_intf_pins static_region/sys_clk0_0]
  connect_bd_intf_net -intf_net sys_clk0_1_1 [get_bd_intf_ports sys_clk0_1] [get_bd_intf_pins static_region/sys_clk0_1]

  # Create port connections
  connect_bd_net -net arstn_1  [get_bd_pins static_region/peripheral_aresetn1] \
  [get_bd_pins service_layer/arstn]
  connect_bd_net -net arstn_2  [get_bd_pins static_region/peripheral_aresetn2] \
  [get_bd_pins slash/arstn]
  connect_bd_net -net aved_pl0_ref_clk -boundary_type upper  [get_bd_pins static_region/pl0_ref_clk]
  connect_bd_net -net clk_wizard_0_clk_out1  [get_bd_pins static_region/clk_out1] \
  [get_bd_pins slash/static_region_clk]
  connect_bd_net -net service_clk_1  [get_bd_pins static_region/clk_out2] \
  [get_bd_pins service_layer/service_clk]
  connect_bd_net -net static_region_clk_1 -boundary_type upper  [get_bd_pins static_region/pl3_ref_clk]
  connect_bd_net -net user_clk_1  [get_bd_pins static_region/clk_out3] \
  [get_bd_pins slash/user_clk]

  # Create address segments
  assign_bd_address -offset 0x00000000 -range 0x80000000 -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S00_INI/C0_DDR_LOW0] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S00_INI/C0_DDR_CH2] -force
  assign_bd_address -offset 0x00000000 -range 0x80000000 -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S01_INI/C1_DDR_LOW0] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S01_INI/C1_DDR_CH2] -force
  assign_bd_address -offset 0x00000000 -range 0x80000000 -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S00_INI/C0_DDR_LOW0] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S00_INI/C0_DDR_CH2] -force
  assign_bd_address -offset 0x00000000 -range 0x80000000 -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S01_INI/C1_DDR_LOW0] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S01_INI/C1_DDR_CH2] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_0/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM00_AXI/HBM0_PC0] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_1/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM01_AXI/HBM0_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_10/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM10_AXI/HBM2_PC1] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_11/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM11_AXI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_12/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM12_AXI/HBM3_PC0] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_13/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM13_AXI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_14/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM14_AXI/HBM3_PC1] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_15/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM15_AXI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_16/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM16_AXI/HBM4_PC0] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_17/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM17_AXI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_18/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM18_AXI/HBM4_PC1] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_19/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM19_AXI/HBM4_PC1] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_2/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM02_AXI/HBM0_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_20/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM20_AXI/HBM5_PC0] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_21/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM21_AXI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_22/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM22_AXI/HBM5_PC1] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_23/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM23_AXI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_24/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM24_AXI/HBM6_PC0] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_25/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM25_AXI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_26/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM26_AXI/HBM6_PC1] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_27/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM27_AXI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_28/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM28_AXI/HBM7_PC0] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_29/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM29_AXI/HBM7_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_3/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM03_AXI/HBM0_PC1] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_30/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM30_AXI/HBM7_PC1] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_31/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM31_AXI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_32/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM32_AXI/HBM8_PC0] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_33/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM33_AXI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_34/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM34_AXI/HBM8_PC1] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_35/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM35_AXI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_36/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM36_AXI/HBM9_PC0] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_37/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM37_AXI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_38/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM38_AXI/HBM9_PC1] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_39/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM39_AXI/HBM9_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_4/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM04_AXI/HBM1_PC0] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_40/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM40_AXI/HBM10_PC0] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_41/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM41_AXI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_42/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM42_AXI/HBM10_PC1] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_43/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM43_AXI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_44/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM44_AXI/HBM11_PC0] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_45/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM45_AXI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_46/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM46_AXI/HBM11_PC1] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_47/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM47_AXI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_48/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM48_AXI/HBM12_PC0] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_49/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM49_AXI/HBM12_PC0] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_5/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM05_AXI/HBM1_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_50/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM50_AXI/HBM12_PC1] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_51/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM51_AXI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_52/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM52_AXI/HBM13_PC0] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_53/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM53_AXI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_54/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM54_AXI/HBM13_PC1] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_55/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM55_AXI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_56/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM56_AXI/HBM14_PC0] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_57/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM57_AXI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_58/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM58_AXI/HBM14_PC1] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_59/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM59_AXI/HBM14_PC1] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_6/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM06_AXI/HBM1_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_60/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM60_AXI/HBM15_PC0] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_61/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM61_AXI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_62/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM62_AXI/HBM15_PC1] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_63/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM63_AXI/HBM15_PC1] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM0_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM0_PC1] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM12_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM14_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM15_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM1_PC0] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM1_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM2_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM4_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM7_PC0] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S04_INI/HBM9_PC1] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM0_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM0_PC1] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM12_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM14_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM15_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM1_PC0] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM1_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM2_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM4_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM7_PC0] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S05_INI/HBM9_PC1] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM0_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM0_PC1] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM12_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM14_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM15_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM1_PC0] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM1_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM2_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM4_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM7_PC0] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S06_INI/HBM9_PC1] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM0_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM0_PC1] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM12_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM14_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM15_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM1_PC0] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM1_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM2_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM4_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM7_PC0] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S07_INI/HBM9_PC1] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM0_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM0_PC1] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM12_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM14_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM15_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM1_PC0] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM1_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM2_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM4_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM7_PC0] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S08_INI/HBM9_PC1] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM0_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM0_PC1] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM12_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM14_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM15_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM1_PC0] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM1_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM2_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM4_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM7_PC0] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S09_INI/HBM9_PC1] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_7/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM07_AXI/HBM1_PC1] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM0_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM0_PC1] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM12_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM14_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM15_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM1_PC0] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM1_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM2_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM4_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM7_PC0] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S10_INI/HBM9_PC1] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM0_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM0_PC1] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM12_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM14_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM15_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM1_PC0] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM1_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM2_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM4_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM7_PC0] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S11_INI/HBM9_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_8/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM08_AXI/HBM2_PC0] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces slash/hbm_bandwidth_9/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_cips/HBM09_AXI/HBM2_PC0] -force
  assign_bd_address -offset 0x020800000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces slash/traffic_virt_0/Data_m_axi_gmem0] [get_bd_addr_segs service_layer/axi4_full_passthrough_0/s_axi/reg0] -force
  assign_bd_address -offset 0x020800000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces slash/traffic_virt_1/Data_m_axi_gmem0] [get_bd_addr_segs service_layer/axi4_full_passthrough_1/s_axi/reg0] -force
  assign_bd_address -offset 0x020800000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces slash/traffic_virt_2/Data_m_axi_gmem0] [get_bd_addr_segs service_layer/axi4_full_passthrough_2/s_axi/reg0] -force
  assign_bd_address -offset 0x020800000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces slash/traffic_virt_3/Data_m_axi_gmem0] [get_bd_addr_segs service_layer/axi4_full_passthrough_3/s_axi/reg0] -force
  assign_bd_address -offset 0x020800000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces slash/traffic_virt_4/Data_m_axi_gmem0] [get_bd_addr_segs service_layer/axi4_full_passthrough_4/s_axi/reg0] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces service_layer/axi4_full_passthrough_0/m_axi] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S00_INI/C0_DDR_CH2] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -with_name SEG_M_VIRT_1_Reg -target_address_space [get_bd_addr_spaces service_layer/axi4_full_passthrough_1/m_axi] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S01_INI/C1_DDR_CH2] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -with_name SEG_M_VIRT_2_Reg -target_address_space [get_bd_addr_spaces service_layer/axi4_full_passthrough_2/m_axi] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S00_INI/C0_DDR_CH2] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -with_name SEG_M_VIRT_3_Reg -target_address_space [get_bd_addr_spaces service_layer/axi4_full_passthrough_3/m_axi] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S01_INI/C1_DDR_CH2] -force
  assign_bd_address -offset 0xE0000000 -range 0x10000000 -target_address_space [get_bd_addr_spaces service_layer/axi4_full_passthrough_4/m_axi] [get_bd_addr_segs static_region/aved/cips/NOC_CPM_PCIE_0/pspmc_0_psv_noc_pcie_0] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces service_layer/eth_0/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S00_INI/C0_DDR_CH2] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces service_layer/eth_1/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S01_INI/C1_DDR_CH2] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces service_layer/eth_2/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S00_INI/C0_DDR_CH2] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces service_layer/eth_3/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S01_INI/C1_DDR_CH2] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces service_layer/eth_4/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S00_INI/C0_DDR_CH2] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces service_layer/eth_5/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S01_INI/C1_DDR_CH2] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces service_layer/eth_6/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S00_INI/C0_DDR_CH2] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces service_layer/eth_7/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S01_INI/C1_DDR_CH2] -force
  assign_bd_address -offset 0x020302040400 -range 0x00000100 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/qsfp_0_n_1/control_intf/axi_gpio_datapath/S_AXI/Reg] -force
  assign_bd_address -offset 0x020303040400 -range 0x00000100 -with_name SEG_axi_gpio_datapath_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/qsfp_2_n_3/control_intf/axi_gpio_datapath/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302040000 -range 0x00000100 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/qsfp_0_n_1/control_intf/axi_gpio_gt_control/S_AXI/Reg] -force
  assign_bd_address -offset 0x020300160000 -range 0x00000100 -with_name SEG_axi_gpio_gt_control_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/qsfp_2_n_3/control_intf/axi_gpio_gt_control/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302040200 -range 0x00000100 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/qsfp_0_n_1/control_intf/axi_gpio_monitor/S_AXI/Reg] -force
  assign_bd_address -offset 0x020303040200 -range 0x00000100 -with_name SEG_axi_gpio_monitor_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/qsfp_2_n_3/control_intf/axi_gpio_monitor/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302040600 -range 0x00000100 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/qsfp_0_n_1/control_intf/axi_gpio_reset_txrx/S_AXI/Reg] -force
  assign_bd_address -offset 0x020303040600 -range 0x00000100 -with_name SEG_axi_gpio_reset_txrx_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/qsfp_2_n_3/control_intf/axi_gpio_reset_txrx/S_AXI/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM0_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM0_PC1] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM12_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM14_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM15_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM1_PC0] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM1_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM2_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM4_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM7_PC0] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_cips/S00_AXI/HBM9_PC1] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S00_INI/C0_DDR_CH2] -force
  assign_bd_address -offset 0x000101220000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_slave_boot] -force
  assign_bd_address -offset 0x000102100000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_slave_boot_stream] -force
  assign_bd_address -offset 0x020400010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/clk_rst_shell/clk_wizard_service/s_axi_lite/Reg] -force
  assign_bd_address -offset 0x020400000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/clk_rst_shell/clk_wizard_slash/s_axi_lite/Reg] -force
  assign_bd_address -offset 0x020302000000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/qsfp_0_n_1/DCMAC_subsys/dcmac_0_core/s_axi/Reg] -force
  assign_bd_address -offset 0x020303000000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/qsfp_2_n_3/DCMAC_subsys/dcmac_1_core/s_axi/Reg] -force
  assign_bd_address -offset 0x020200480000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/ddr_bandwidth_64/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200490000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/ddr_bandwidth_65/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/ddr_bandwidth_66/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/ddr_bandwidth_67/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/eth_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/eth_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300020000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/eth_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300030000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/eth_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300040000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/eth_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300050000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/eth_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300060000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/eth_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300070000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/eth_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020101010000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/base_logic/gcq_m2r/S00_AXI/S00_AXI_Reg] -force
  assign_bd_address -offset 0x020200000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_10/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_11/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_12/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_13/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_14/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_15/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200100000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_16/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200110000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_17/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200120000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_18/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200130000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_19/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200140000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_20/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200150000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_21/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200160000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_22/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200170000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_23/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200180000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_24/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200190000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_25/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_26/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_27/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_28/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_29/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200020000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_30/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_31/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200200000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_32/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200210000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_33/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200220000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_34/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200230000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_35/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200240000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_36/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200250000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_37/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200260000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_38/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200270000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_39/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200030000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200280000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_40/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200290000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_41/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_42/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_43/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_44/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_45/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_46/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_47/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200300000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_48/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200310000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_49/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200040000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200320000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_50/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200330000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_51/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200340000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_52/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200350000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_53/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200360000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_54/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200370000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_55/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200380000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_56/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200390000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_57/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_58/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_59/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200050000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_60/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_61/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_62/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_63/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200400000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_64/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200410000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_65/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200420000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_66/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200430000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_67/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200440000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_68/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200450000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_69/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200060000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200460000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_70/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200470000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_71/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200070000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200080000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_8/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200090000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/hbm_bandwidth_9/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020101000000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/base_logic/hw_discovery/s_axi_ctrl_pf0/reg0] -force
  assign_bd_address -offset 0x020101040000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/clock_reset/pcie_mgmt_pdi_reset/pcie_mgmt_pdi_reset_gpio/S_AXI/Reg] -force
  assign_bd_address -offset 0x0202004C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_producer_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_producer_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300090000 -range 0x00010000 -with_name SEG_traffic_producer_1_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/traffic_producer_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_producer_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000A0000 -range 0x00010000 -with_name SEG_traffic_producer_2_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/traffic_producer_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200500000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_producer_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000B0000 -range 0x00010000 -with_name SEG_traffic_producer_3_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/traffic_producer_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_producer_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200510000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_producer_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000D0000 -range 0x00010000 -with_name SEG_traffic_producer_5_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/traffic_producer_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200530000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_producer_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000E0000 -range 0x00010000 -with_name SEG_traffic_producer_6_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/traffic_producer_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200520000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_producer_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000F0000 -range 0x00010000 -with_name SEG_traffic_producer_7_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs service_layer/traffic_producer_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200540000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_virt_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200550000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_virt_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200560000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_virt_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200570000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_virt_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200580000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs slash/traffic_virt_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020101001000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/base_logic/uuid_rom/S_AXI/reg0] -force
  assign_bd_address -offset 0x020303040400 -range 0x00000100 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/qsfp_2_n_3/control_intf/axi_gpio_datapath/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302040400 -range 0x00000100 -with_name SEG_axi_gpio_datapath_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/qsfp_0_n_1/control_intf/axi_gpio_datapath/S_AXI/Reg] -force
  assign_bd_address -offset 0x020300160000 -range 0x00000100 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/qsfp_2_n_3/control_intf/axi_gpio_gt_control/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302040000 -range 0x00000100 -with_name SEG_axi_gpio_gt_control_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/qsfp_0_n_1/control_intf/axi_gpio_gt_control/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302040200 -range 0x00000100 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/qsfp_0_n_1/control_intf/axi_gpio_monitor/S_AXI/Reg] -force
  assign_bd_address -offset 0x020303040200 -range 0x00000100 -with_name SEG_axi_gpio_monitor_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/qsfp_2_n_3/control_intf/axi_gpio_monitor/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302040600 -range 0x00000100 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/qsfp_0_n_1/control_intf/axi_gpio_reset_txrx/S_AXI/Reg] -force
  assign_bd_address -offset 0x020303040600 -range 0x00000100 -with_name SEG_axi_gpio_reset_txrx_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/qsfp_2_n_3/control_intf/axi_gpio_reset_txrx/S_AXI/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM0_PC0] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM0_PC1] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM10_PC0] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM10_PC1] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM11_PC0] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM11_PC1] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM12_PC0] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM12_PC1] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM13_PC0] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM13_PC1] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM14_PC0] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM14_PC1] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM15_PC0] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM15_PC1] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM1_PC0] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM1_PC1] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM2_PC0] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM2_PC1] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM3_PC0] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM3_PC1] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM4_PC0] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM4_PC1] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM5_PC0] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM5_PC1] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM6_PC0] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM6_PC1] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM7_PC0] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM7_PC1] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM8_PC0] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM8_PC1] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM9_PC0] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_cips/S01_AXI/HBM9_PC1] -force
  assign_bd_address -offset 0x050080000000 -range 0x80000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S01_INI/C1_DDR_CH1] -force
  assign_bd_address -offset 0x00000000 -range 0x80000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S01_INI/C1_DDR_LOW0] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S01_INI/C1_DDR_CH2] -force
  assign_bd_address -offset 0x000101220000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_slave_boot] -force
  assign_bd_address -offset 0x000102100000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_slave_boot_stream] -force
  assign_bd_address -offset 0x020400010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/clk_rst_shell/clk_wizard_service/s_axi_lite/Reg] -force
  assign_bd_address -offset 0x020400000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/clk_rst_shell/clk_wizard_slash/s_axi_lite/Reg] -force
  assign_bd_address -offset 0x020302000000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/qsfp_0_n_1/DCMAC_subsys/dcmac_0_core/s_axi/Reg] -force
  assign_bd_address -offset 0x020303000000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/qsfp_2_n_3/DCMAC_subsys/dcmac_1_core/s_axi/Reg] -force
  assign_bd_address -offset 0x020200480000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/ddr_bandwidth_64/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200490000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/ddr_bandwidth_65/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/ddr_bandwidth_66/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/ddr_bandwidth_67/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/eth_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/eth_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300020000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/eth_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300030000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/eth_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300040000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/eth_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300050000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/eth_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300060000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/eth_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300070000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/eth_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020101010000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/base_logic/gcq_m2r/S00_AXI/S00_AXI_Reg] -force
  assign_bd_address -offset 0x020200000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_10/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_11/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_12/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_13/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_14/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_15/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200100000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_16/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200110000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_17/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200120000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_18/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200130000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_19/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200140000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_20/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200150000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_21/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200160000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_22/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200170000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_23/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200180000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_24/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200190000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_25/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_26/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_27/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_28/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_29/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200020000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_30/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_31/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200200000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_32/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200210000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_33/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200220000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_34/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200230000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_35/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200240000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_36/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200250000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_37/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200260000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_38/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200270000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_39/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200030000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200280000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_40/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200290000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_41/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_42/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_43/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_44/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_45/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_46/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_47/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200300000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_48/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200310000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_49/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200040000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200320000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_50/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200330000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_51/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200340000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_52/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200350000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_53/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200360000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_54/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200370000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_55/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200380000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_56/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200390000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_57/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_58/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_59/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200050000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_60/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_61/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_62/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_63/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200400000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_64/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200410000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_65/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200420000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_66/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200430000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_67/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200440000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_68/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200450000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_69/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200060000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200460000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_70/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200470000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_71/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200070000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200080000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_8/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200090000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/hbm_bandwidth_9/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020101000000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/base_logic/hw_discovery/s_axi_ctrl_pf0/reg0] -force
  assign_bd_address -offset 0x020101040000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/clock_reset/pcie_mgmt_pdi_reset/pcie_mgmt_pdi_reset_gpio/S_AXI/Reg] -force
  assign_bd_address -offset 0x0202004C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_producer_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_producer_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300090000 -range 0x00010000 -with_name SEG_traffic_producer_1_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/traffic_producer_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_producer_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000A0000 -range 0x00010000 -with_name SEG_traffic_producer_2_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/traffic_producer_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200500000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_producer_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000B0000 -range 0x00010000 -with_name SEG_traffic_producer_3_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/traffic_producer_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_producer_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200510000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_producer_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000D0000 -range 0x00010000 -with_name SEG_traffic_producer_5_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/traffic_producer_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200530000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_producer_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000E0000 -range 0x00010000 -with_name SEG_traffic_producer_6_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/traffic_producer_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200520000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_producer_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000F0000 -range 0x00010000 -with_name SEG_traffic_producer_7_Reg_1 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs service_layer/traffic_producer_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200540000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_virt_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200550000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_virt_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200560000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_virt_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200570000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_virt_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200580000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs slash/traffic_virt_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020101001000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/base_logic/uuid_rom/S_AXI/reg0] -force
  assign_bd_address -offset 0x00000000 -range 0x80000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/LPD_AXI_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S00_INI/C0_DDR_LOW0] -force
  assign_bd_address -offset 0x80044000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/M_AXI_LPD] [get_bd_addr_segs static_region/aved/base_logic/axi_smbus_rpu/S_AXI/Reg] -force
  assign_bd_address -offset 0x80010000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/M_AXI_LPD] [get_bd_addr_segs static_region/aved/base_logic/gcq_m2r/S01_AXI/S01_AXI_Reg] -force
  assign_bd_address -offset 0x050080000000 -range 0x80000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/PMC_NOC_AXI_0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S00_INI/C0_DDR_CH1] -force
  assign_bd_address -offset 0x00000000 -range 0x80000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/PMC_NOC_AXI_0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S00_INI/C0_DDR_LOW0] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/PMC_NOC_AXI_0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_1/S00_INI/C0_DDR_CH2] -force

  # Exclude Address Segments
  exclude_bd_addr_seg -offset 0x000600000000 -range 0x000200000000 -target_address_space [get_bd_addr_spaces service_layer/axi4_full_passthrough_4/m_axi] [get_bd_addr_segs static_region/aved/cips/NOC_CPM_PCIE_0/pspmc_0_psv_noc_pcie_1]
  exclude_bd_addr_seg -offset 0x008000000000 -range 0x004000000000 -target_address_space [get_bd_addr_spaces service_layer/axi4_full_passthrough_4/m_axi] [get_bd_addr_segs static_region/aved/cips/NOC_CPM_PCIE_0/pspmc_0_psv_noc_pcie_2]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S00_INI/C0_DDR_CH1]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S01_INI/C1_DDR_CH1]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S00_INI/C0_DDR_CH1]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces slash/ddr_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S01_INI/C1_DDR_CH1]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S00_INI/C0_DDR_CH1]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S00_INI/C0_DDR_LOW0]
  exclude_bd_addr_seg -offset 0xFFA80000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_0]
  exclude_bd_addr_seg -offset 0xFFA90000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_1]
  exclude_bd_addr_seg -offset 0xFFAA0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_2]
  exclude_bd_addr_seg -offset 0xFFAB0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_3]
  exclude_bd_addr_seg -offset 0xFFAC0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_4]
  exclude_bd_addr_seg -offset 0xFFAD0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_5]
  exclude_bd_addr_seg -offset 0xFFAE0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_6]
  exclude_bd_addr_seg -offset 0xFFAF0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_7]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_apu_0]
  exclude_bd_addr_seg -offset 0x000100800000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_0]
  exclude_bd_addr_seg -offset 0x000100D10000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a720_cti]
  exclude_bd_addr_seg -offset 0x000100D00000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a720_dbg]
  exclude_bd_addr_seg -offset 0x000100D30000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a720_etm]
  exclude_bd_addr_seg -offset 0x000100D20000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a720_pmu]
  exclude_bd_addr_seg -offset 0x000100D50000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a721_cti]
  exclude_bd_addr_seg -offset 0x000100D40000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a721_dbg]
  exclude_bd_addr_seg -offset 0x000100D70000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a721_etm]
  exclude_bd_addr_seg -offset 0x000100D60000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a721_pmu]
  exclude_bd_addr_seg -offset 0x000100CA0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_apu_cti]
  exclude_bd_addr_seg -offset 0x000100C60000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_apu_ela]
  exclude_bd_addr_seg -offset 0x000100C30000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_apu_etf]
  exclude_bd_addr_seg -offset 0x000100C20000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_apu_fun]
  exclude_bd_addr_seg -offset 0x000100F80000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_atm]
  exclude_bd_addr_seg -offset 0x000100FA0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_cti2a]
  exclude_bd_addr_seg -offset 0x000100FD0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_cti2d]
  exclude_bd_addr_seg -offset 0x000100F40000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_ela2a]
  exclude_bd_addr_seg -offset 0x000100F50000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_ela2b]
  exclude_bd_addr_seg -offset 0x000100F60000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_ela2c]
  exclude_bd_addr_seg -offset 0x000100F70000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_ela2d]
  exclude_bd_addr_seg -offset 0x000100F20000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_fun]
  exclude_bd_addr_seg -offset 0x000100F00000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_rom]
  exclude_bd_addr_seg -offset 0x000100B80000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_fpd_atm]
  exclude_bd_addr_seg -offset 0x000100B70000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_fpd_stm]
  exclude_bd_addr_seg -offset 0x000100980000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_lpd_atm]
  exclude_bd_addr_seg -offset 0xFC000000 -range 0x01000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_cpm]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_crf_0]
  exclude_bd_addr_seg -offset 0xFF5E0000 -range 0x00300000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_crl_0]
  exclude_bd_addr_seg -offset 0x000101260000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_crp_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_afi_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_afi_2]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_cci_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_gpv_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_maincci_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_slave_xmpu_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_slcr_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_slcr_secure_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_smmu_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_smmutcu_0]
  exclude_bd_addr_seg -offset 0xFF0B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_gpio_2]
  exclude_bd_addr_seg -offset 0xFF020000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_i2c_0]
  exclude_bd_addr_seg -offset 0xFF030000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_i2c_1]
  exclude_bd_addr_seg -offset 0xFF360000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_3]
  exclude_bd_addr_seg -offset 0xFF370000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_4]
  exclude_bd_addr_seg -offset 0xFF380000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_5]
  exclude_bd_addr_seg -offset 0xFF3A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_6]
  exclude_bd_addr_seg -offset 0xFF320000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_pmc]
  exclude_bd_addr_seg -offset 0xFF390000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_pmc_nobuf]
  exclude_bd_addr_seg -offset 0xFF310000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_psm]
  exclude_bd_addr_seg -offset 0xFF9B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_afi_0]
  exclude_bd_addr_seg -offset 0xFF0A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_iou_secure_slcr_0]
  exclude_bd_addr_seg -offset 0xFF080000 -range 0x00020000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_iou_slcr_0]
  exclude_bd_addr_seg -offset 0xFF410000 -range 0x00100000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_slcr_0]
  exclude_bd_addr_seg -offset 0xFF510000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_slcr_secure_0]
  exclude_bd_addr_seg -offset 0xFF990000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_xppu_0]
  exclude_bd_addr_seg -offset 0xFF960000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ocm_ctrl]
  exclude_bd_addr_seg -offset 0xFFFC0000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ocm_ram_0]
  exclude_bd_addr_seg -offset 0xFF980000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ocm_xmpu_0]
  exclude_bd_addr_seg -offset 0x0001011E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_aes]
  exclude_bd_addr_seg -offset 0x0001011F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_bbram_ctrl]
  exclude_bd_addr_seg -offset 0x0001012D0000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_cfi_cframe_0]
  exclude_bd_addr_seg -offset 0x0001012B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_cfu_apb_0]
  exclude_bd_addr_seg -offset 0x0001011C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_dma_0]
  exclude_bd_addr_seg -offset 0x0001011D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_dma_1]
  exclude_bd_addr_seg -offset 0x000101250000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_efuse_cache]
  exclude_bd_addr_seg -offset 0x000101240000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_efuse_ctrl]
  exclude_bd_addr_seg -offset 0x000101110000 -range 0x00050000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_global_0]
  exclude_bd_addr_seg -offset 0x000101020000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_gpio_0]
  exclude_bd_addr_seg -offset 0x000100280000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_iomodule_0]
  exclude_bd_addr_seg -offset 0x000101010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ospi_0]
  exclude_bd_addr_seg -offset 0x000100310000 -range 0x00008000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ppu1_mdm_0]
  exclude_bd_addr_seg -offset 0xC0000000 -range 0x20000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_qspi_ospi_flash_0]
  exclude_bd_addr_seg -offset 0x000102000000 -range 0x00020000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ram]
  exclude_bd_addr_seg -offset 0x000100240000 -range 0x00020000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ram_data_cntlr]
  exclude_bd_addr_seg -offset 0x000100200000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ram_instr_cntlr]
  exclude_bd_addr_seg -offset 0x000106000000 -range 0x02000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ram_npi]
  exclude_bd_addr_seg -offset 0x000101200000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_rsa]
  exclude_bd_addr_seg -offset 0x0001012A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_rtc_0]
  exclude_bd_addr_seg -offset 0x000101040000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_sd_0]
  exclude_bd_addr_seg -offset 0x000101210000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_sha]
  exclude_bd_addr_seg -offset 0x000101270000 -range 0x00030000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_sysmon_0]
  exclude_bd_addr_seg -offset 0x000100083000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_tmr_inject_0]
  exclude_bd_addr_seg -offset 0x000100283000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_tmr_manager_0]
  exclude_bd_addr_seg -offset 0x000101230000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_trng]
  exclude_bd_addr_seg -offset 0x0001012F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_xmpu_0]
  exclude_bd_addr_seg -offset 0x000101310000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_xppu_0]
  exclude_bd_addr_seg -offset 0x000101300000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_xppu_npi_0]
  exclude_bd_addr_seg -offset 0xFFC90000 -range 0x0000F000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_psm_global_reg]
  exclude_bd_addr_seg -offset 0xFFE90000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_r5_1_atcm_global]
  exclude_bd_addr_seg -offset 0xFFEB0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_r5_1_btcm_global]
  exclude_bd_addr_seg -offset 0xFFE00000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_r5_tcm_ram_global]
  exclude_bd_addr_seg -offset 0xFF9A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_rpu_0]
  exclude_bd_addr_seg -offset 0xFF000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_sbsauart_0]
  exclude_bd_addr_seg -offset 0xFF010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_sbsauart_1]
  exclude_bd_addr_seg -offset 0xFF130000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_scntr_0]
  exclude_bd_addr_seg -offset 0xFF140000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_scntrs_0]
  exclude_bd_addr_seg -offset 0xFF040000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_spi_0]
  exclude_bd_addr_seg -offset 0xFF0E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ttc_0]
  exclude_bd_addr_seg -offset 0xFF0F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ttc_1]
  exclude_bd_addr_seg -offset 0xFF100000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ttc_2]
  exclude_bd_addr_seg -offset 0xFF110000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_0] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ttc_3]
  exclude_bd_addr_seg -offset 0xFFA80000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_0]
  exclude_bd_addr_seg -offset 0xFFA90000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_1]
  exclude_bd_addr_seg -offset 0xFFAA0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_2]
  exclude_bd_addr_seg -offset 0xFFAB0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_3]
  exclude_bd_addr_seg -offset 0xFFAC0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_4]
  exclude_bd_addr_seg -offset 0xFFAD0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_5]
  exclude_bd_addr_seg -offset 0xFFAE0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_6]
  exclude_bd_addr_seg -offset 0xFFAF0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_adma_7]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_apu_0]
  exclude_bd_addr_seg -offset 0x000100800000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_0]
  exclude_bd_addr_seg -offset 0x000100D10000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a720_cti]
  exclude_bd_addr_seg -offset 0x000100D00000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a720_dbg]
  exclude_bd_addr_seg -offset 0x000100D30000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a720_etm]
  exclude_bd_addr_seg -offset 0x000100D20000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a720_pmu]
  exclude_bd_addr_seg -offset 0x000100D50000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a721_cti]
  exclude_bd_addr_seg -offset 0x000100D40000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a721_dbg]
  exclude_bd_addr_seg -offset 0x000100D70000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a721_etm]
  exclude_bd_addr_seg -offset 0x000100D60000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_a721_pmu]
  exclude_bd_addr_seg -offset 0x000100CA0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_apu_cti]
  exclude_bd_addr_seg -offset 0x000100C60000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_apu_ela]
  exclude_bd_addr_seg -offset 0x000100C30000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_apu_etf]
  exclude_bd_addr_seg -offset 0x000100C20000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_apu_fun]
  exclude_bd_addr_seg -offset 0x000100F80000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_atm]
  exclude_bd_addr_seg -offset 0x000100FA0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_cti2a]
  exclude_bd_addr_seg -offset 0x000100FD0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_cti2d]
  exclude_bd_addr_seg -offset 0x000100F40000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_ela2a]
  exclude_bd_addr_seg -offset 0x000100F50000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_ela2b]
  exclude_bd_addr_seg -offset 0x000100F60000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_ela2c]
  exclude_bd_addr_seg -offset 0x000100F70000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_ela2d]
  exclude_bd_addr_seg -offset 0x000100F20000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_fun]
  exclude_bd_addr_seg -offset 0x000100F00000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_cpm_rom]
  exclude_bd_addr_seg -offset 0x000100B80000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_fpd_atm]
  exclude_bd_addr_seg -offset 0x000100B70000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_fpd_stm]
  exclude_bd_addr_seg -offset 0x000100980000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_coresight_lpd_atm]
  exclude_bd_addr_seg -offset 0xFC000000 -range 0x01000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_cpm]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_crf_0]
  exclude_bd_addr_seg -offset 0xFF5E0000 -range 0x00300000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_crl_0]
  exclude_bd_addr_seg -offset 0x000101260000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_crp_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_afi_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_afi_2]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_cci_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_gpv_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_maincci_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_slave_xmpu_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_slcr_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_slcr_secure_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_smmu_0]
  exclude_bd_addr_seg -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_fpd_smmutcu_0]
  exclude_bd_addr_seg -offset 0xFF0B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_gpio_2]
  exclude_bd_addr_seg -offset 0xFF020000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_i2c_0]
  exclude_bd_addr_seg -offset 0xFF030000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_i2c_1]
  exclude_bd_addr_seg -offset 0xFF360000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_3]
  exclude_bd_addr_seg -offset 0xFF370000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_4]
  exclude_bd_addr_seg -offset 0xFF380000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_5]
  exclude_bd_addr_seg -offset 0xFF3A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_6]
  exclude_bd_addr_seg -offset 0xFF320000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_pmc]
  exclude_bd_addr_seg -offset 0xFF390000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_pmc_nobuf]
  exclude_bd_addr_seg -offset 0xFF310000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ipi_psm]
  exclude_bd_addr_seg -offset 0xFF9B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_afi_0]
  exclude_bd_addr_seg -offset 0xFF0A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_iou_secure_slcr_0]
  exclude_bd_addr_seg -offset 0xFF080000 -range 0x00020000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_iou_slcr_0]
  exclude_bd_addr_seg -offset 0xFF410000 -range 0x00100000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_slcr_0]
  exclude_bd_addr_seg -offset 0xFF510000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_slcr_secure_0]
  exclude_bd_addr_seg -offset 0xFF990000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_lpd_xppu_0]
  exclude_bd_addr_seg -offset 0xFF960000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ocm_ctrl]
  exclude_bd_addr_seg -offset 0xFFFC0000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ocm_ram_0]
  exclude_bd_addr_seg -offset 0xFF980000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ocm_xmpu_0]
  exclude_bd_addr_seg -offset 0x0001011E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_aes]
  exclude_bd_addr_seg -offset 0x0001011F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_bbram_ctrl]
  exclude_bd_addr_seg -offset 0x0001012D0000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_cfi_cframe_0]
  exclude_bd_addr_seg -offset 0x0001012B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_cfu_apb_0]
  exclude_bd_addr_seg -offset 0x0001011C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_dma_0]
  exclude_bd_addr_seg -offset 0x0001011D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_dma_1]
  exclude_bd_addr_seg -offset 0x000101250000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_efuse_cache]
  exclude_bd_addr_seg -offset 0x000101240000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_efuse_ctrl]
  exclude_bd_addr_seg -offset 0x000101110000 -range 0x00050000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_global_0]
  exclude_bd_addr_seg -offset 0x000101020000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_gpio_0]
  exclude_bd_addr_seg -offset 0x000100280000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_iomodule_0]
  exclude_bd_addr_seg -offset 0x000101010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ospi_0]
  exclude_bd_addr_seg -offset 0x000100310000 -range 0x00008000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ppu1_mdm_0]
  exclude_bd_addr_seg -offset 0xC0000000 -range 0x20000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_qspi_ospi_flash_0]
  exclude_bd_addr_seg -offset 0x000102000000 -range 0x00020000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ram]
  exclude_bd_addr_seg -offset 0x000100240000 -range 0x00020000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ram_data_cntlr]
  exclude_bd_addr_seg -offset 0x000100200000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ram_instr_cntlr]
  exclude_bd_addr_seg -offset 0x000106000000 -range 0x02000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_ram_npi]
  exclude_bd_addr_seg -offset 0x000101200000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_rsa]
  exclude_bd_addr_seg -offset 0x0001012A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_rtc_0]
  exclude_bd_addr_seg -offset 0x000101040000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_sd_0]
  exclude_bd_addr_seg -offset 0x000101210000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_sha]
  exclude_bd_addr_seg -offset 0x000101270000 -range 0x00030000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_sysmon_0]
  exclude_bd_addr_seg -offset 0x000100083000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_tmr_inject_0]
  exclude_bd_addr_seg -offset 0x000100283000 -range 0x00001000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_tmr_manager_0]
  exclude_bd_addr_seg -offset 0x000101230000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_trng]
  exclude_bd_addr_seg -offset 0x0001012F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_xmpu_0]
  exclude_bd_addr_seg -offset 0x000101310000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_xppu_0]
  exclude_bd_addr_seg -offset 0x000101300000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_pmc_xppu_npi_0]
  exclude_bd_addr_seg -offset 0xFFC90000 -range 0x0000F000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_psm_global_reg]
  exclude_bd_addr_seg -offset 0xFFE90000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_r5_1_atcm_global]
  exclude_bd_addr_seg -offset 0xFFEB0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_r5_1_btcm_global]
  exclude_bd_addr_seg -offset 0xFFE00000 -range 0x00040000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_r5_tcm_ram_global]
  exclude_bd_addr_seg -offset 0xFF9A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_rpu_0]
  exclude_bd_addr_seg -offset 0xFF000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_sbsauart_0]
  exclude_bd_addr_seg -offset 0xFF010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_sbsauart_1]
  exclude_bd_addr_seg -offset 0xFF130000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_scntr_0]
  exclude_bd_addr_seg -offset 0xFF140000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_scntrs_0]
  exclude_bd_addr_seg -offset 0xFF040000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_spi_0]
  exclude_bd_addr_seg -offset 0xFF0E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ttc_0]
  exclude_bd_addr_seg -offset 0xFF0F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ttc_1]
  exclude_bd_addr_seg -offset 0xFF100000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ttc_2]
  exclude_bd_addr_seg -offset 0xFF110000 -range 0x00010000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/CPM_PCIE_NOC_1] [get_bd_addr_segs static_region/aved/cips/NOC_PMC_AXI_0/pspmc_0_psv_ttc_3]
  exclude_bd_addr_seg -offset 0x050080000000 -range 0x80000000 -target_address_space [get_bd_addr_spaces static_region/aved/cips/LPD_AXI_NOC_0] [get_bd_addr_segs static_region/noc/axi_noc_mc_ddr4_0/S00_INI/C0_DDR_CH1]


  # Restore current instance
  current_bd_instance $oldCurInst

  validate_bd_design
  save_bd_design
}
# End of create_root_design()


##################################################################
# MAIN FLOW
##################################################################

create_root_design ""


