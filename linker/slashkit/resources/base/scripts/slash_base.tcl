
################################################################
# This is a generated script based on design: slash_base
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
# source slash_base_script.tcl

# If there is no project opened, this script will create a
# project, but make sure you do not have an existing project
# <./myproj/project_1.xpr> in the current working folder.

set list_projs [get_projects -quiet]
if { $list_projs eq "" } {
   create_project project_1 myproj -part xcv80-lsva4737-2MHP-e-S
}


# CHANGE DESIGN NAME HERE
variable design_name
set design_name slash_base

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
xilinx.com:hls:hbm_bandwidth:1.0\
xilinx.com:ip:smartconnect:1.0\
xilinx.com:ip:axi_noc:1.1\
xilinx.com:ip:axis_noc:1.0\
xilinx.com:hls:traffic_producer:1.0\
xilinx.com:ip:xlconstant:1.1\
xilinx.com:ip:c_shift_ram:12.0\
xilinx.com:inline_hdl:ilreduced_logic:1.0\
xilinx.com:ip:util_ds_buf:2.2\
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

if { $bCheckIPsPassed != 1 } {
  common::send_gid_msg -ssname BD::TCL -id 2023 -severity "WARNING" "Will not continue with creation of design due to the error(s) above."
  return 3
}

##################################################################
# DESIGN PROCs
##################################################################



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


  # Create interface ports
  set HBM_AXI_00 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_00 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_00

  set HBM_AXI_01 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_01 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_01

  set HBM_AXI_10 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_10 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_10

  set HBM_AXI_11 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_11 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_11

  set HBM_AXI_12 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_12 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_12

  set HBM_AXI_13 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_13 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_13

  set HBM_AXI_14 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_14 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_14

  set HBM_AXI_15 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_15 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_15

  set HBM_AXI_16 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_16 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_16

  set HBM_AXI_17 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_17 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_17

  set HBM_AXI_18 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_18 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_18

  set HBM_AXI_19 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_19 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_19

  set HBM_AXI_02 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_02 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_02

  set HBM_AXI_20 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_20 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_20

  set HBM_AXI_21 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_21 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_21

  set HBM_AXI_22 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_22 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_22

  set HBM_AXI_23 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_23 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_23

  set HBM_AXI_24 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_24 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_24

  set HBM_AXI_25 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_25 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_25

  set HBM_AXI_26 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_26 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_26

  set HBM_AXI_27 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_27 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_27

  set HBM_AXI_28 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_28 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_28

  set HBM_AXI_29 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_29 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_29

  set HBM_AXI_03 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_03 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_03

  set HBM_AXI_30 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_30 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_30

  set HBM_AXI_31 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_31 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_31

  set HBM_AXI_32 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_32 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_32

  set HBM_AXI_33 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_33 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_33

  set HBM_AXI_34 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_34 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_34

  set HBM_AXI_35 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_35 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_35

  set HBM_AXI_36 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_36 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_36

  set HBM_AXI_37 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_37 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_37

  set HBM_AXI_38 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_38 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_38

  set HBM_AXI_39 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_39 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_39

  set HBM_AXI_04 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_04 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_04

  set HBM_AXI_40 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_40 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_40

  set HBM_AXI_41 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_41 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_41

  set HBM_AXI_42 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_42 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_42

  set HBM_AXI_43 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_43 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_43

  set HBM_AXI_44 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_44 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_44

  set HBM_AXI_45 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_45 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_45

  set HBM_AXI_46 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_46 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_46

  set HBM_AXI_47 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_47 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_47

  set HBM_AXI_48 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_48 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_48

  set HBM_AXI_49 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_49 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_49

  set HBM_AXI_05 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_05 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_05

  set HBM_AXI_50 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_50 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_50

  set HBM_AXI_51 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_51 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_51

  set HBM_AXI_52 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_52 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_52

  set HBM_AXI_53 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_53 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_53

  set HBM_AXI_54 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_54 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_54

  set HBM_AXI_55 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_55 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_55

  set HBM_AXI_56 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_56 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_56

  set HBM_AXI_57 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_57 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_57

  set HBM_AXI_58 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_58 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_58

  set HBM_AXI_59 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_59 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_59

  set HBM_AXI_06 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_06 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_06

  set HBM_AXI_60 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_60 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_60

  set HBM_AXI_61 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_61 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_61

  set HBM_AXI_62 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_62 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_62

  set HBM_AXI_63 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_63 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_63

  set HBM_AXI_07 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_07 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_07

  set HBM_AXI_08 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_08 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_08

  set HBM_AXI_09 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HBM_AXI_09 ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {64} \
   CONFIG.DATA_WIDTH {256} \
   CONFIG.FREQ_HZ {400000000} \
   CONFIG.HAS_BURST {1} \
   CONFIG.HAS_RRESP {0} \
   CONFIG.NUM_READ_OUTSTANDING {16} \
   CONFIG.NUM_WRITE_OUTSTANDING {16} \
   CONFIG.PROTOCOL {AXI4} \
   CONFIG.READ_WRITE_MODE {READ_WRITE} \
   ] $HBM_AXI_09

  set M00_INI [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M00_INI ]

  set M01_INI [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M01_INI ]

  set M02_INI [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M02_INI ]

  set M03_INI [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M03_INI ]

  set HBM_VNOC_INI_00 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 HBM_VNOC_INI_00 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $HBM_VNOC_INI_00

  set HBM_VNOC_INI_01 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 HBM_VNOC_INI_01 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $HBM_VNOC_INI_01

  set HBM_VNOC_INI_02 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 HBM_VNOC_INI_02 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $HBM_VNOC_INI_02

  set HBM_VNOC_INI_03 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 HBM_VNOC_INI_03 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $HBM_VNOC_INI_03

  set HBM_VNOC_INI_04 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 HBM_VNOC_INI_04 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $HBM_VNOC_INI_04

  set HBM_VNOC_INI_05 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 HBM_VNOC_INI_05 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $HBM_VNOC_INI_05

  set HBM_VNOC_INI_06 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 HBM_VNOC_INI_06 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $HBM_VNOC_INI_06

  set HBM_VNOC_INI_07 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 HBM_VNOC_INI_07 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $HBM_VNOC_INI_07

  set M_DCMAC_INIS0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS0 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS0

  set M_DCMAC_INIS1 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS1 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS1

  set M_DCMAC_INIS2 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS2 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS2

  set M_DCMAC_INIS3 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS3 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS3

  set M_DCMAC_INIS4 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS4 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS4

  set M_DCMAC_INIS5 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS5 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS5

  set M_DCMAC_INIS6 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS6 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS6

  set M_DCMAC_INIS7 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS7 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS7

  set S_DCMAC_INIS0 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS0 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS0

  set S_DCMAC_INIS1 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS1 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS1

  set S_DCMAC_INIS2 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS2 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS2

  set S_DCMAC_INIS3 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS3 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS3

  set S_DCMAC_INIS4 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS4 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS4

  set S_DCMAC_INIS5 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS5 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS5

  set S_DCMAC_INIS6 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS6 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS6

  set S_DCMAC_INIS7 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS7 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS7

  set S_AXILITE_INI [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S_AXILITE_INI ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_AXILITE_INI
  set_property APERTURES {{0x202_0000_0000 128M}} [get_bd_intf_ports S_AXILITE_INI]

  set SL_VIRT_00 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL_VIRT_00 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL_VIRT_00

  set SL_VIRT_01 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL_VIRT_01 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL_VIRT_01

  set SL_VIRT_02 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL_VIRT_02 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL_VIRT_02

  set SL_VIRT_03 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL_VIRT_03 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL_VIRT_03

  set QDMA_SLAVE_BRIDGE_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 QDMA_SLAVE_BRIDGE_0 ]
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
   ] $QDMA_SLAVE_BRIDGE_0


  # Create ports
  set arstn [ create_bd_port -dir I -type rst arstn ]
  set static_region_clk [ create_bd_port -dir I -type clk -freq_hz 400000000 static_region_clk ]
  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {HBM_AXI_63:HBM_AXI_62:HBM_AXI_61:HBM_AXI_60:HBM_AXI_59:HBM_AXI_58:HBM_AXI_57:HBM_AXI_56:HBM_AXI_55:HBM_AXI_54:HBM_AXI_53:HBM_AXI_52:HBM_AXI_51:HBM_AXI_50:HBM_AXI_49:HBM_AXI_48:HBM_AXI_47:HBM_AXI_46:HBM_AXI_45:HBM_AXI_44:HBM_AXI_43:HBM_AXI_42:HBM_AXI_41:HBM_AXI_40:HBM_AXI_39:HBM_AXI_38:HBM_AXI_37:HBM_AXI_36:HBM_AXI_35:HBM_AXI_34:HBM_AXI_33:HBM_AXI_32:HBM_AXI_31:HBM_AXI_30:HBM_AXI_29:HBM_AXI_28:HBM_AXI_27:HBM_AXI_26:HBM_AXI_25:HBM_AXI_24:HBM_AXI_23:HBM_AXI_22:HBM_AXI_21:HBM_AXI_20:HBM_AXI_19:HBM_AXI_18:HBM_AXI_17:HBM_AXI_16:HBM_AXI_15:HBM_AXI_14:HBM_AXI_13:HBM_AXI_12:HBM_AXI_11:HBM_AXI_10:HBM_AXI_09:HBM_AXI_08:HBM_AXI_07:HBM_AXI_06:HBM_AXI_05:HBM_AXI_04:HBM_AXI_03:HBM_AXI_02:HBM_AXI_01:HBM_AXI_00} \
   CONFIG.CLK_DOMAIN {top_clk_wizard_0_0_clk_out1} \
 ] $static_region_clk
  set user_clk [ create_bd_port -dir I -type clk -freq_hz 200000000 user_clk ]

  # Create instance: ddr_bandwidth_64, and set properties
  set ddr_bandwidth_64 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 ddr_bandwidth_64 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $ddr_bandwidth_64


  # Create instance: ddr_bandwidth_65, and set properties
  set ddr_bandwidth_65 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 ddr_bandwidth_65 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $ddr_bandwidth_65


  # Create instance: ddr_bandwidth_66, and set properties
  set ddr_bandwidth_66 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 ddr_bandwidth_66 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $ddr_bandwidth_66


  # Create instance: ddr_bandwidth_67, and set properties
  set ddr_bandwidth_67 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 ddr_bandwidth_67 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $ddr_bandwidth_67


  # Create instance: hbm_bandwidth_0, and set properties
  set hbm_bandwidth_0 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_0 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_0


  # Create instance: hbm_bandwidth_1, and set properties
  set hbm_bandwidth_1 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_1 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_1


  # Create instance: hbm_bandwidth_10, and set properties
  set hbm_bandwidth_10 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_10 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_10


  # Create instance: hbm_bandwidth_11, and set properties
  set hbm_bandwidth_11 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_11 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_11


  # Create instance: hbm_bandwidth_12, and set properties
  set hbm_bandwidth_12 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_12 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_12


  # Create instance: hbm_bandwidth_13, and set properties
  set hbm_bandwidth_13 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_13 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_13


  # Create instance: hbm_bandwidth_14, and set properties
  set hbm_bandwidth_14 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_14 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_14


  # Create instance: hbm_bandwidth_15, and set properties
  set hbm_bandwidth_15 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_15 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_15


  # Create instance: hbm_bandwidth_16, and set properties
  set hbm_bandwidth_16 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_16 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_16


  # Create instance: hbm_bandwidth_17, and set properties
  set hbm_bandwidth_17 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_17 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_17


  # Create instance: hbm_bandwidth_18, and set properties
  set hbm_bandwidth_18 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_18 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_18


  # Create instance: hbm_bandwidth_19, and set properties
  set hbm_bandwidth_19 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_19 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_19


  # Create instance: hbm_bandwidth_2, and set properties
  set hbm_bandwidth_2 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_2 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_2


  # Create instance: hbm_bandwidth_20, and set properties
  set hbm_bandwidth_20 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_20 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_20


  # Create instance: hbm_bandwidth_21, and set properties
  set hbm_bandwidth_21 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_21 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_21


  # Create instance: hbm_bandwidth_22, and set properties
  set hbm_bandwidth_22 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_22 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_22


  # Create instance: hbm_bandwidth_23, and set properties
  set hbm_bandwidth_23 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_23 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_23


  # Create instance: hbm_bandwidth_24, and set properties
  set hbm_bandwidth_24 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_24 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_24


  # Create instance: hbm_bandwidth_25, and set properties
  set hbm_bandwidth_25 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_25 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_25


  # Create instance: hbm_bandwidth_26, and set properties
  set hbm_bandwidth_26 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_26 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_26


  # Create instance: hbm_bandwidth_27, and set properties
  set hbm_bandwidth_27 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_27 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_27


  # Create instance: hbm_bandwidth_28, and set properties
  set hbm_bandwidth_28 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_28 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_28


  # Create instance: hbm_bandwidth_29, and set properties
  set hbm_bandwidth_29 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_29 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_29


  # Create instance: hbm_bandwidth_3, and set properties
  set hbm_bandwidth_3 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_3 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_3


  # Create instance: hbm_bandwidth_30, and set properties
  set hbm_bandwidth_30 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_30 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_30


  # Create instance: hbm_bandwidth_31, and set properties
  set hbm_bandwidth_31 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_31 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_31


  # Create instance: hbm_bandwidth_32, and set properties
  set hbm_bandwidth_32 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_32 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_32


  # Create instance: hbm_bandwidth_33, and set properties
  set hbm_bandwidth_33 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_33 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_33


  # Create instance: hbm_bandwidth_34, and set properties
  set hbm_bandwidth_34 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_34 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_34


  # Create instance: hbm_bandwidth_35, and set properties
  set hbm_bandwidth_35 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_35 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_35


  # Create instance: hbm_bandwidth_36, and set properties
  set hbm_bandwidth_36 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_36 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_36


  # Create instance: hbm_bandwidth_37, and set properties
  set hbm_bandwidth_37 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_37 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_37


  # Create instance: hbm_bandwidth_38, and set properties
  set hbm_bandwidth_38 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_38 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_38


  # Create instance: hbm_bandwidth_39, and set properties
  set hbm_bandwidth_39 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_39 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_39


  # Create instance: hbm_bandwidth_4, and set properties
  set hbm_bandwidth_4 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_4 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_4


  # Create instance: hbm_bandwidth_40, and set properties
  set hbm_bandwidth_40 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_40 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_40


  # Create instance: hbm_bandwidth_41, and set properties
  set hbm_bandwidth_41 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_41 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_41


  # Create instance: hbm_bandwidth_42, and set properties
  set hbm_bandwidth_42 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_42 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_42


  # Create instance: hbm_bandwidth_43, and set properties
  set hbm_bandwidth_43 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_43 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_43


  # Create instance: hbm_bandwidth_44, and set properties
  set hbm_bandwidth_44 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_44 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_44


  # Create instance: hbm_bandwidth_45, and set properties
  set hbm_bandwidth_45 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_45 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_45


  # Create instance: hbm_bandwidth_46, and set properties
  set hbm_bandwidth_46 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_46 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_46


  # Create instance: hbm_bandwidth_47, and set properties
  set hbm_bandwidth_47 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_47 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_47


  # Create instance: hbm_bandwidth_48, and set properties
  set hbm_bandwidth_48 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_48 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_48


  # Create instance: hbm_bandwidth_49, and set properties
  set hbm_bandwidth_49 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_49 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_49


  # Create instance: hbm_bandwidth_5, and set properties
  set hbm_bandwidth_5 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_5 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_5


  # Create instance: hbm_bandwidth_50, and set properties
  set hbm_bandwidth_50 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_50 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_50


  # Create instance: hbm_bandwidth_51, and set properties
  set hbm_bandwidth_51 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_51 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_51


  # Create instance: hbm_bandwidth_52, and set properties
  set hbm_bandwidth_52 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_52 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_52


  # Create instance: hbm_bandwidth_53, and set properties
  set hbm_bandwidth_53 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_53 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_53


  # Create instance: hbm_bandwidth_54, and set properties
  set hbm_bandwidth_54 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_54 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_54


  # Create instance: hbm_bandwidth_55, and set properties
  set hbm_bandwidth_55 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_55 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_55


  # Create instance: hbm_bandwidth_56, and set properties
  set hbm_bandwidth_56 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_56 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_56


  # Create instance: hbm_bandwidth_57, and set properties
  set hbm_bandwidth_57 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_57 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_57


  # Create instance: hbm_bandwidth_58, and set properties
  set hbm_bandwidth_58 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_58 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_58


  # Create instance: hbm_bandwidth_59, and set properties
  set hbm_bandwidth_59 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_59 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_59


  # Create instance: hbm_bandwidth_6, and set properties
  set hbm_bandwidth_6 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_6 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_6


  # Create instance: hbm_bandwidth_60, and set properties
  set hbm_bandwidth_60 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_60 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_60


  # Create instance: hbm_bandwidth_61, and set properties
  set hbm_bandwidth_61 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_61 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_61


  # Create instance: hbm_bandwidth_62, and set properties
  set hbm_bandwidth_62 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_62 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_62


  # Create instance: hbm_bandwidth_63, and set properties
  set hbm_bandwidth_63 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_63 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_63


  # Create instance: hbm_bandwidth_7, and set properties
  set hbm_bandwidth_7 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_7 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_7


  # Create instance: hbm_bandwidth_8, and set properties
  set hbm_bandwidth_8 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_8 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_8


  # Create instance: hbm_bandwidth_9, and set properties
  set hbm_bandwidth_9 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_9 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_9


  # Create instance: smartconnect_0, and set properties
  set smartconnect_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_0 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MI {16} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect_0


  # Create instance: ddr_noc_0, and set properties
  set ddr_noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 ddr_noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $ddr_noc_0


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /ddr_noc_0/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /ddr_noc_0/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /ddr_noc_0/aclk0]

  # Create instance: ddr_noc_1, and set properties
  set ddr_noc_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 ddr_noc_1 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $ddr_noc_1


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /ddr_noc_1/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /ddr_noc_1/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /ddr_noc_1/aclk0]

  # Create instance: ddr_noc_2, and set properties
  set ddr_noc_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 ddr_noc_2 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $ddr_noc_2


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /ddr_noc_2/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /ddr_noc_2/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /ddr_noc_2/aclk0]

  # Create instance: ddr_noc_3, and set properties
  set ddr_noc_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 ddr_noc_3 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $ddr_noc_3


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /ddr_noc_3/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /ddr_noc_3/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /ddr_noc_3/aclk0]

  # Create instance: hbm_bandwidth_64, and set properties
  set hbm_bandwidth_64 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_64 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_64


  # Create instance: hbm_bandwidth_65, and set properties
  set hbm_bandwidth_65 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_65 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_65


  # Create instance: hbm_bandwidth_66, and set properties
  set hbm_bandwidth_66 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_66 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_66


  # Create instance: hbm_bandwidth_67, and set properties
  set hbm_bandwidth_67 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_67 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_67


  # Create instance: hbm_bandwidth_68, and set properties
  set hbm_bandwidth_68 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_68 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_68


  # Create instance: hbm_bandwidth_69, and set properties
  set hbm_bandwidth_69 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_69 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_69


  # Create instance: hbm_bandwidth_70, and set properties
  set hbm_bandwidth_70 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_70 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_70


  # Create instance: hbm_bandwidth_71, and set properties
  set hbm_bandwidth_71 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_71 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $hbm_bandwidth_71


  # Create instance: hbm_vnoc_00, and set properties
  set hbm_vnoc_00 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 hbm_vnoc_00 ]
  set_property -dict [list \
    CONFIG.NSI_NAMES {} \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $hbm_vnoc_00


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /hbm_vnoc_00/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /hbm_vnoc_00/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /hbm_vnoc_00/aclk0]

  # Create instance: hbm_vnoc_01, and set properties
  set hbm_vnoc_01 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 hbm_vnoc_01 ]
  set_property -dict [list \
    CONFIG.NSI_NAMES {} \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $hbm_vnoc_01


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /hbm_vnoc_01/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /hbm_vnoc_01/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /hbm_vnoc_01/aclk0]

  # Create instance: hbm_vnoc_02, and set properties
  set hbm_vnoc_02 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 hbm_vnoc_02 ]
  set_property -dict [list \
    CONFIG.NSI_NAMES {} \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $hbm_vnoc_02


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /hbm_vnoc_02/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /hbm_vnoc_02/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /hbm_vnoc_02/aclk0]

  # Create instance: hbm_vnoc_03, and set properties
  set hbm_vnoc_03 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 hbm_vnoc_03 ]
  set_property -dict [list \
    CONFIG.NSI_NAMES {} \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $hbm_vnoc_03


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /hbm_vnoc_03/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /hbm_vnoc_03/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /hbm_vnoc_03/aclk0]

  # Create instance: hbm_vnoc_04, and set properties
  set hbm_vnoc_04 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 hbm_vnoc_04 ]
  set_property -dict [list \
    CONFIG.NSI_NAMES {} \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $hbm_vnoc_04


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /hbm_vnoc_04/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /hbm_vnoc_04/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /hbm_vnoc_04/aclk0]

  # Create instance: hbm_vnoc_05, and set properties
  set hbm_vnoc_05 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 hbm_vnoc_05 ]
  set_property -dict [list \
    CONFIG.NSI_NAMES {} \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $hbm_vnoc_05


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /hbm_vnoc_05/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /hbm_vnoc_05/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /hbm_vnoc_05/aclk0]

  # Create instance: hbm_vnoc_06, and set properties
  set hbm_vnoc_06 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 hbm_vnoc_06 ]
  set_property -dict [list \
    CONFIG.NSI_NAMES {} \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $hbm_vnoc_06


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /hbm_vnoc_06/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /hbm_vnoc_06/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /hbm_vnoc_06/aclk0]

  # Create instance: hbm_vnoc_07, and set properties
  set hbm_vnoc_07 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 hbm_vnoc_07 ]
  set_property -dict [list \
    CONFIG.NSI_NAMES {} \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $hbm_vnoc_07


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /hbm_vnoc_07/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /hbm_vnoc_07/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /hbm_vnoc_07/aclk0]

  # Create instance: dcmac_axis_noc_0, and set properties
  set dcmac_axis_noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dcmac_axis_noc_0


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dcmac_axis_noc_0/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_0/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_0/aclk0]

  # Create instance: dcmac_axis_noc_1, and set properties
  set dcmac_axis_noc_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_1 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dcmac_axis_noc_1


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dcmac_axis_noc_1/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS {write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_1/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_1/aclk0]

  # Create instance: dcmac_axis_noc_2, and set properties
  set dcmac_axis_noc_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_2 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dcmac_axis_noc_2


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dcmac_axis_noc_2/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS {write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_2/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_2/aclk0]

  # Create instance: dcmac_axis_noc_3, and set properties
  set dcmac_axis_noc_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_3 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dcmac_axis_noc_3


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dcmac_axis_noc_3/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS {write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_3/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_3/aclk0]

  # Create instance: dcmac_axis_noc_4, and set properties
  set dcmac_axis_noc_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_4 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dcmac_axis_noc_4


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dcmac_axis_noc_4/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS {write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_4/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_4/aclk0]

  # Create instance: dcmac_axis_noc_5, and set properties
  set dcmac_axis_noc_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_5 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dcmac_axis_noc_5


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dcmac_axis_noc_5/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS {write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_5/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_5/aclk0]

  # Create instance: dcmac_axis_noc_6, and set properties
  set dcmac_axis_noc_6 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_6 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dcmac_axis_noc_6


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dcmac_axis_noc_6/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS {write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_6/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_6/aclk0]

  # Create instance: dcmac_axis_noc_7, and set properties
  set dcmac_axis_noc_7 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_7 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dcmac_axis_noc_7


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dcmac_axis_noc_7/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS {write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_7/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_7/aclk0]

  # Create instance: dcmac_axis_noc_s_0, and set properties
  set dcmac_axis_noc_s_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_s_0 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_axis_noc_s_0


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_0/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_0/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_s_0/aclk0]

  # Create instance: dcmac_axis_noc_s_1, and set properties
  set dcmac_axis_noc_s_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_s_1 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_axis_noc_s_1


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_1/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_1/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_s_1/aclk0]

  # Create instance: dcmac_axis_noc_s_2, and set properties
  set dcmac_axis_noc_s_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_s_2 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_axis_noc_s_2


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_2/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_2/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_s_2/aclk0]

  # Create instance: dcmac_axis_noc_s_3, and set properties
  set dcmac_axis_noc_s_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_s_3 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_axis_noc_s_3


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_3/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_3/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_s_3/aclk0]

  # Create instance: dcmac_axis_noc_s_4, and set properties
  set dcmac_axis_noc_s_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_s_4 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_axis_noc_s_4


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_4/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_4/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_s_4/aclk0]

  # Create instance: dcmac_axis_noc_s_5, and set properties
  set dcmac_axis_noc_s_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_s_5 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_axis_noc_s_5


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_5/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_5/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_s_5/aclk0]

  # Create instance: dcmac_axis_noc_s_6, and set properties
  set dcmac_axis_noc_s_6 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_s_6 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_axis_noc_s_6


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_6/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_6/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_s_6/aclk0]

  # Create instance: dcmac_axis_noc_s_7, and set properties
  set dcmac_axis_noc_s_7 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dcmac_axis_noc_s_7 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dcmac_axis_noc_s_7


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_7/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dcmac_axis_noc_s_7/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dcmac_axis_noc_s_7/aclk0]

  # Create instance: traffic_producer_0, and set properties
  set traffic_producer_0 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_0 ]

  # Create instance: traffic_producer_1, and set properties
  set traffic_producer_1 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_1 ]

  # Create instance: traffic_producer_2, and set properties
  set traffic_producer_2 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_2 ]

  # Create instance: traffic_producer_3, and set properties
  set traffic_producer_3 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_3 ]

  # Create instance: traffic_producer_4, and set properties
  set traffic_producer_4 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_4 ]

  # Create instance: traffic_producer_5, and set properties
  set traffic_producer_5 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_5 ]

  # Create instance: traffic_producer_6, and set properties
  set traffic_producer_6 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_6 ]

  # Create instance: traffic_producer_7, and set properties
  set traffic_producer_7 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_7 ]

  # Create instance: xlconstant_0, and set properties
  set xlconstant_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_0 ]

  # Create instance: traffic_virt_0, and set properties
  set traffic_virt_0 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 traffic_virt_0 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {512} $traffic_virt_0


  # Create instance: traffic_virt_1, and set properties
  set traffic_virt_1 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 traffic_virt_1 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {512} $traffic_virt_1


  # Create instance: traffic_virt_2, and set properties
  set traffic_virt_2 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 traffic_virt_2 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {512} $traffic_virt_2


  # Create instance: traffic_virt_3, and set properties
  set traffic_virt_3 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 traffic_virt_3 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {512} $traffic_virt_3


  # Create instance: traffic_virt_4, and set properties
  set traffic_virt_4 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 traffic_virt_4 ]
  set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {128} $traffic_virt_4


  # Create instance: smartconnect_1, and set properties
  set smartconnect_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_1 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_MI {16} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect_1


  # Create instance: smartconnect_2, and set properties
  set smartconnect_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_2 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MI {16} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect_2


  # Create instance: smartconnect_3, and set properties
  set smartconnect_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_3 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MI {16} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect_3


  # Create instance: smartconnect_4, and set properties
  set smartconnect_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_4 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MI {16} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect_4


  # Create instance: smartconnect_5, and set properties
  set smartconnect_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_5 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MI {14} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect_5


  # Create instance: axi_noc_0, and set properties
  set axi_noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_0


  set_property -dict [ list \
   CONFIG.APERTURES {{0x202_0000_0000 128M}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_0/M00_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXI {read_bw {5} write_bw {5} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /axi_noc_0/S00_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI} \
 ] [get_bd_pins /axi_noc_0/aclk0]

  # Create instance: hbm_sc_00, and set properties
  set hbm_sc_00 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_00 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_00


  # Create instance: hbm_sc_01, and set properties
  set hbm_sc_01 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_01 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_01


  # Create instance: hbm_sc_02, and set properties
  set hbm_sc_02 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_02 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_02


  # Create instance: hbm_sc_03, and set properties
  set hbm_sc_03 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_03 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_03


  # Create instance: hbm_sc_04, and set properties
  set hbm_sc_04 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_04 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_04


  # Create instance: hbm_sc_05, and set properties
  set hbm_sc_05 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_05 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_05


  # Create instance: hbm_sc_06, and set properties
  set hbm_sc_06 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_06 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_06


  # Create instance: hbm_sc_07, and set properties
  set hbm_sc_07 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_07 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_07


  # Create instance: hbm_sc_08, and set properties
  set hbm_sc_08 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_08 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_08


  # Create instance: hbm_sc_09, and set properties
  set hbm_sc_09 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_09 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_09


  # Create instance: hbm_sc_10, and set properties
  set hbm_sc_10 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_10 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_10


  # Create instance: hbm_sc_11, and set properties
  set hbm_sc_11 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_11 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_11


  # Create instance: hbm_sc_12, and set properties
  set hbm_sc_12 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_12 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_12


  # Create instance: hbm_sc_13, and set properties
  set hbm_sc_13 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_13 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_13


  # Create instance: hbm_sc_14, and set properties
  set hbm_sc_14 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_14 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_14


  # Create instance: hbm_sc_15, and set properties
  set hbm_sc_15 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_15 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_15


  # Create instance: hbm_sc_16, and set properties
  set hbm_sc_16 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_16 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_16


  # Create instance: hbm_sc_17, and set properties
  set hbm_sc_17 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_17 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_17


  # Create instance: hbm_sc_18, and set properties
  set hbm_sc_18 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_18 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_18


  # Create instance: hbm_sc_19, and set properties
  set hbm_sc_19 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_19 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_19


  # Create instance: hbm_sc_20, and set properties
  set hbm_sc_20 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_20 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_20


  # Create instance: hbm_sc_21, and set properties
  set hbm_sc_21 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_21 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_21


  # Create instance: hbm_sc_22, and set properties
  set hbm_sc_22 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_22 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_22


  # Create instance: hbm_sc_23, and set properties
  set hbm_sc_23 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_23 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_23


  # Create instance: hbm_sc_24, and set properties
  set hbm_sc_24 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_24 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_24


  # Create instance: hbm_sc_25, and set properties
  set hbm_sc_25 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_25 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_25


  # Create instance: hbm_sc_26, and set properties
  set hbm_sc_26 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_26 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_26


  # Create instance: hbm_sc_27, and set properties
  set hbm_sc_27 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_27 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_27


  # Create instance: hbm_sc_28, and set properties
  set hbm_sc_28 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_28 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_28


  # Create instance: hbm_sc_29, and set properties
  set hbm_sc_29 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_29 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_29


  # Create instance: hbm_sc_30, and set properties
  set hbm_sc_30 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_30 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_30


  # Create instance: hbm_sc_31, and set properties
  set hbm_sc_31 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_31 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_31


  # Create instance: hbm_sc_32, and set properties
  set hbm_sc_32 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_32 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_32


  # Create instance: hbm_sc_33, and set properties
  set hbm_sc_33 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_33 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_33


  # Create instance: hbm_sc_34, and set properties
  set hbm_sc_34 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_34 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_34


  # Create instance: hbm_sc_35, and set properties
  set hbm_sc_35 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_35 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_35


  # Create instance: hbm_sc_36, and set properties
  set hbm_sc_36 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_36 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_36


  # Create instance: hbm_sc_37, and set properties
  set hbm_sc_37 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_37 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_37


  # Create instance: hbm_sc_38, and set properties
  set hbm_sc_38 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_38 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_38


  # Create instance: hbm_sc_39, and set properties
  set hbm_sc_39 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_39 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_39


  # Create instance: hbm_sc_40, and set properties
  set hbm_sc_40 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_40 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_40


  # Create instance: hbm_sc_41, and set properties
  set hbm_sc_41 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_41 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_41


  # Create instance: hbm_sc_42, and set properties
  set hbm_sc_42 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_42 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_42


  # Create instance: hbm_sc_43, and set properties
  set hbm_sc_43 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_43 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_43


  # Create instance: hbm_sc_44, and set properties
  set hbm_sc_44 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_44 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_44


  # Create instance: hbm_sc_45, and set properties
  set hbm_sc_45 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_45 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_45


  # Create instance: hbm_sc_46, and set properties
  set hbm_sc_46 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_46 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_46


  # Create instance: hbm_sc_47, and set properties
  set hbm_sc_47 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_47 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_47


  # Create instance: hbm_sc_48, and set properties
  set hbm_sc_48 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_48 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_48


  # Create instance: hbm_sc_49, and set properties
  set hbm_sc_49 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_49 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_49


  # Create instance: hbm_sc_50, and set properties
  set hbm_sc_50 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_50 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_50


  # Create instance: hbm_sc_51, and set properties
  set hbm_sc_51 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_51 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_51


  # Create instance: hbm_sc_52, and set properties
  set hbm_sc_52 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_52 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_52


  # Create instance: hbm_sc_53, and set properties
  set hbm_sc_53 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_53 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_53


  # Create instance: hbm_sc_54, and set properties
  set hbm_sc_54 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_54 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_54


  # Create instance: hbm_sc_55, and set properties
  set hbm_sc_55 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_55 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_55


  # Create instance: hbm_sc_56, and set properties
  set hbm_sc_56 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_56 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_56


  # Create instance: hbm_sc_57, and set properties
  set hbm_sc_57 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_57 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_57


  # Create instance: hbm_sc_58, and set properties
  set hbm_sc_58 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_58 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_58


  # Create instance: hbm_sc_59, and set properties
  set hbm_sc_59 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_59 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_59


  # Create instance: hbm_sc_60, and set properties
  set hbm_sc_60 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_60 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_60


  # Create instance: hbm_sc_61, and set properties
  set hbm_sc_61 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_61 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_61


  # Create instance: hbm_sc_62, and set properties
  set hbm_sc_62 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_62 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_62


  # Create instance: hbm_sc_63, and set properties
  set hbm_sc_63 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_63 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_SI {1} \
  ] $hbm_sc_63


  # Create instance: noc_virt_00, and set properties
  set noc_virt_00 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 noc_virt_00 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $noc_virt_00


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /noc_virt_00/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /noc_virt_00/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /noc_virt_00/aclk0]

  # Create instance: qdma_slave_bridge_noc, and set properties
  set qdma_slave_bridge_noc [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 qdma_slave_bridge_noc ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $qdma_slave_bridge_noc


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /qdma_slave_bridge_noc/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /qdma_slave_bridge_noc/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /qdma_slave_bridge_noc/aclk0]

  # Create instance: noc_virt_02, and set properties
  set noc_virt_02 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 noc_virt_02 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $noc_virt_02


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /noc_virt_02/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /noc_virt_02/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /noc_virt_02/aclk0]

  # Create instance: noc_virt_03, and set properties
  set noc_virt_03 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 noc_virt_03 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $noc_virt_03


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /noc_virt_03/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /noc_virt_03/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /noc_virt_03/aclk0]

  # Create instance: axi_noc_1, and set properties
  set axi_noc_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 axi_noc_1 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $axi_noc_1


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /axi_noc_1/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_1/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /axi_noc_1/aclk0]

  # Create instance: c_shift_ram_0, and set properties
  set c_shift_ram_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:c_shift_ram:12.0 c_shift_ram_0 ]
  set_property -dict [list \
    CONFIG.Depth {1} \
    CONFIG.Width {1} \
  ] $c_shift_ram_0


  # Create instance: ilreduced_logic_0, and set properties
  set ilreduced_logic_0 [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilreduced_logic:1.0 ilreduced_logic_0 ]
  set_property -dict [list \
    CONFIG.C_OPERATION {or} \
    CONFIG.C_SIZE {1} \
  ] $ilreduced_logic_0


  # Create instance: util_ds_buf_0, and set properties
  set util_ds_buf_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_ds_buf:2.2 util_ds_buf_0 ]
  set_property CONFIG.C_BUF_TYPE {BUFG_FABRIC} $util_ds_buf_0


  # Create interface connections
  connect_bd_intf_net -intf_net S00_INIS_0_1 [get_bd_intf_ports S_DCMAC_INIS0] [get_bd_intf_pins dcmac_axis_noc_s_0/S00_INIS]
  connect_bd_intf_net -intf_net S00_INIS_1_1 [get_bd_intf_ports S_DCMAC_INIS1] [get_bd_intf_pins dcmac_axis_noc_s_1/S00_INIS]
  connect_bd_intf_net -intf_net S00_INIS_2_1 [get_bd_intf_ports S_DCMAC_INIS2] [get_bd_intf_pins dcmac_axis_noc_s_2/S00_INIS]
  connect_bd_intf_net -intf_net S00_INIS_3_1 [get_bd_intf_ports S_DCMAC_INIS3] [get_bd_intf_pins dcmac_axis_noc_s_3/S00_INIS]
  connect_bd_intf_net -intf_net S00_INIS_4_1 [get_bd_intf_ports S_DCMAC_INIS4] [get_bd_intf_pins dcmac_axis_noc_s_4/S00_INIS]
  connect_bd_intf_net -intf_net S00_INIS_5_1 [get_bd_intf_ports S_DCMAC_INIS5] [get_bd_intf_pins dcmac_axis_noc_s_5/S00_INIS]
  connect_bd_intf_net -intf_net S00_INIS_6_1 [get_bd_intf_ports S_DCMAC_INIS6] [get_bd_intf_pins dcmac_axis_noc_s_6/S00_INIS]
  connect_bd_intf_net -intf_net S00_INIS_7_1 [get_bd_intf_ports S_DCMAC_INIS7] [get_bd_intf_pins dcmac_axis_noc_s_7/S00_INIS]
  connect_bd_intf_net -intf_net S_AXILITE_INI_1 [get_bd_intf_ports S_AXILITE_INI] [get_bd_intf_pins axi_noc_0/S00_INI]
  connect_bd_intf_net -intf_net axi_noc_0_M00_AXI [get_bd_intf_pins axi_noc_0/M00_AXI] [get_bd_intf_pins smartconnect_0/S00_AXI]
  connect_bd_intf_net -intf_net axi_noc_1_M00_INI [get_bd_intf_ports QDMA_SLAVE_BRIDGE_0] [get_bd_intf_pins axi_noc_1/M00_INI]
  connect_bd_intf_net -intf_net dcmac_axis_noc_0_M00_INIS [get_bd_intf_ports M_DCMAC_INIS0] [get_bd_intf_pins dcmac_axis_noc_0/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_1_M00_INIS [get_bd_intf_ports M_DCMAC_INIS1] [get_bd_intf_pins dcmac_axis_noc_1/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_2_M00_INIS [get_bd_intf_ports M_DCMAC_INIS2] [get_bd_intf_pins dcmac_axis_noc_2/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_3_M00_INIS [get_bd_intf_ports M_DCMAC_INIS3] [get_bd_intf_pins dcmac_axis_noc_3/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_4_M00_INIS [get_bd_intf_ports M_DCMAC_INIS4] [get_bd_intf_pins dcmac_axis_noc_4/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_5_M00_INIS [get_bd_intf_ports M_DCMAC_INIS5] [get_bd_intf_pins dcmac_axis_noc_5/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_6_M00_INIS [get_bd_intf_ports M_DCMAC_INIS6] [get_bd_intf_pins dcmac_axis_noc_6/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_7_M00_INIS [get_bd_intf_ports M_DCMAC_INIS7] [get_bd_intf_pins dcmac_axis_noc_7/M00_INIS]
  connect_bd_intf_net -intf_net ddr_bandwidth_64_m_axi_gmem0 [get_bd_intf_pins ddr_bandwidth_64/m_axi_gmem0] [get_bd_intf_pins ddr_noc_0/S00_AXI]
  connect_bd_intf_net -intf_net ddr_bandwidth_65_m_axi_gmem0 [get_bd_intf_pins ddr_bandwidth_65/m_axi_gmem0] [get_bd_intf_pins ddr_noc_1/S00_AXI]
  connect_bd_intf_net -intf_net ddr_bandwidth_66_m_axi_gmem0 [get_bd_intf_pins ddr_bandwidth_66/m_axi_gmem0] [get_bd_intf_pins ddr_noc_2/S00_AXI]
  connect_bd_intf_net -intf_net ddr_bandwidth_67_m_axi_gmem0 [get_bd_intf_pins ddr_bandwidth_67/m_axi_gmem0] [get_bd_intf_pins ddr_noc_3/S00_AXI]
  connect_bd_intf_net -intf_net ddr_noc_0_M00_INI [get_bd_intf_ports M00_INI] [get_bd_intf_pins ddr_noc_0/M00_INI]
  connect_bd_intf_net -intf_net ddr_noc_1_M00_INI [get_bd_intf_ports M01_INI] [get_bd_intf_pins ddr_noc_1/M00_INI]
  connect_bd_intf_net -intf_net ddr_noc_2_M00_INI [get_bd_intf_ports M02_INI] [get_bd_intf_pins ddr_noc_2/M00_INI]
  connect_bd_intf_net -intf_net ddr_noc_3_M00_INI [get_bd_intf_ports M03_INI] [get_bd_intf_pins ddr_noc_3/M00_INI]
  connect_bd_intf_net -intf_net hbm_bandwidth_0_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_0/m_axi_gmem0] [get_bd_intf_pins hbm_sc_00/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_10_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_10/m_axi_gmem0] [get_bd_intf_pins hbm_sc_10/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_11_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_11/m_axi_gmem0] [get_bd_intf_pins hbm_sc_11/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_12_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_12/m_axi_gmem0] [get_bd_intf_pins hbm_sc_12/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_13_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_13/m_axi_gmem0] [get_bd_intf_pins hbm_sc_13/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_14_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_14/m_axi_gmem0] [get_bd_intf_pins hbm_sc_14/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_15_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_15/m_axi_gmem0] [get_bd_intf_pins hbm_sc_15/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_16_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_16/m_axi_gmem0] [get_bd_intf_pins hbm_sc_16/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_17_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_17/m_axi_gmem0] [get_bd_intf_pins hbm_sc_17/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_18_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_18/m_axi_gmem0] [get_bd_intf_pins hbm_sc_18/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_19_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_19/m_axi_gmem0] [get_bd_intf_pins hbm_sc_19/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_1_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_1/m_axi_gmem0] [get_bd_intf_pins hbm_sc_01/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_20_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_20/m_axi_gmem0] [get_bd_intf_pins hbm_sc_20/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_21_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_21/m_axi_gmem0] [get_bd_intf_pins hbm_sc_21/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_22_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_22/m_axi_gmem0] [get_bd_intf_pins hbm_sc_22/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_23_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_23/m_axi_gmem0] [get_bd_intf_pins hbm_sc_23/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_24_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_24/m_axi_gmem0] [get_bd_intf_pins hbm_sc_24/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_25_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_25/m_axi_gmem0] [get_bd_intf_pins hbm_sc_25/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_26_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_26/m_axi_gmem0] [get_bd_intf_pins hbm_sc_26/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_27_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_27/m_axi_gmem0] [get_bd_intf_pins hbm_sc_27/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_28_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_28/m_axi_gmem0] [get_bd_intf_pins hbm_sc_28/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_29_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_29/m_axi_gmem0] [get_bd_intf_pins hbm_sc_29/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_2_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_2/m_axi_gmem0] [get_bd_intf_pins hbm_sc_02/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_30_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_30/m_axi_gmem0] [get_bd_intf_pins hbm_sc_30/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_31_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_31/m_axi_gmem0] [get_bd_intf_pins hbm_sc_31/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_32_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_32/m_axi_gmem0] [get_bd_intf_pins hbm_sc_32/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_33_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_33/m_axi_gmem0] [get_bd_intf_pins hbm_sc_33/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_34_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_34/m_axi_gmem0] [get_bd_intf_pins hbm_sc_34/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_35_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_35/m_axi_gmem0] [get_bd_intf_pins hbm_sc_35/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_36_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_36/m_axi_gmem0] [get_bd_intf_pins hbm_sc_36/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_37_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_37/m_axi_gmem0] [get_bd_intf_pins hbm_sc_37/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_38_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_38/m_axi_gmem0] [get_bd_intf_pins hbm_sc_38/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_39_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_39/m_axi_gmem0] [get_bd_intf_pins hbm_sc_39/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_3_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_3/m_axi_gmem0] [get_bd_intf_pins hbm_sc_03/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_40_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_40/m_axi_gmem0] [get_bd_intf_pins hbm_sc_40/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_41_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_41/m_axi_gmem0] [get_bd_intf_pins hbm_sc_41/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_42_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_42/m_axi_gmem0] [get_bd_intf_pins hbm_sc_42/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_43_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_43/m_axi_gmem0] [get_bd_intf_pins hbm_sc_43/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_44_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_44/m_axi_gmem0] [get_bd_intf_pins hbm_sc_44/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_45_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_45/m_axi_gmem0] [get_bd_intf_pins hbm_sc_45/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_46_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_46/m_axi_gmem0] [get_bd_intf_pins hbm_sc_46/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_47_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_47/m_axi_gmem0] [get_bd_intf_pins hbm_sc_47/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_48_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_48/m_axi_gmem0] [get_bd_intf_pins hbm_sc_48/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_49_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_49/m_axi_gmem0] [get_bd_intf_pins hbm_sc_49/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_4_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_4/m_axi_gmem0] [get_bd_intf_pins hbm_sc_04/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_50_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_50/m_axi_gmem0] [get_bd_intf_pins hbm_sc_50/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_51_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_51/m_axi_gmem0] [get_bd_intf_pins hbm_sc_51/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_52_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_52/m_axi_gmem0] [get_bd_intf_pins hbm_sc_52/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_53_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_53/m_axi_gmem0] [get_bd_intf_pins hbm_sc_53/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_54_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_54/m_axi_gmem0] [get_bd_intf_pins hbm_sc_54/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_55_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_55/m_axi_gmem0] [get_bd_intf_pins hbm_sc_55/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_56_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_56/m_axi_gmem0] [get_bd_intf_pins hbm_sc_56/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_57_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_57/m_axi_gmem0] [get_bd_intf_pins hbm_sc_57/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_58_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_58/m_axi_gmem0] [get_bd_intf_pins hbm_sc_58/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_59_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_59/m_axi_gmem0] [get_bd_intf_pins hbm_sc_59/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_5_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_5/m_axi_gmem0] [get_bd_intf_pins hbm_sc_05/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_60_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_60/m_axi_gmem0] [get_bd_intf_pins hbm_sc_60/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_61_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_61/m_axi_gmem0] [get_bd_intf_pins hbm_sc_61/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_62_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_62/m_axi_gmem0] [get_bd_intf_pins hbm_sc_62/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_63_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_63/m_axi_gmem0] [get_bd_intf_pins hbm_sc_63/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_64_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_64/m_axi_gmem0] [get_bd_intf_pins hbm_vnoc_00/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_65_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_65/m_axi_gmem0] [get_bd_intf_pins hbm_vnoc_01/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_66_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_66/m_axi_gmem0] [get_bd_intf_pins hbm_vnoc_02/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_67_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_67/m_axi_gmem0] [get_bd_intf_pins hbm_vnoc_03/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_68_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_68/m_axi_gmem0] [get_bd_intf_pins hbm_vnoc_04/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_69_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_69/m_axi_gmem0] [get_bd_intf_pins hbm_vnoc_05/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_6_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_6/m_axi_gmem0] [get_bd_intf_pins hbm_sc_06/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_70_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_70/m_axi_gmem0] [get_bd_intf_pins hbm_vnoc_06/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_71_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_71/m_axi_gmem0] [get_bd_intf_pins hbm_vnoc_07/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_7_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_7/m_axi_gmem0] [get_bd_intf_pins hbm_sc_07/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_8_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_8/m_axi_gmem0] [get_bd_intf_pins hbm_sc_08/S00_AXI]
  connect_bd_intf_net -intf_net hbm_bandwidth_9_m_axi_gmem0 [get_bd_intf_pins hbm_bandwidth_9/m_axi_gmem0] [get_bd_intf_pins hbm_sc_09/S00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_01_M00_AXI [get_bd_intf_ports HBM_AXI_01] [get_bd_intf_pins hbm_sc_01/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_02_M00_AXI [get_bd_intf_ports HBM_AXI_02] [get_bd_intf_pins hbm_sc_02/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_03_M00_AXI [get_bd_intf_ports HBM_AXI_03] [get_bd_intf_pins hbm_sc_03/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_04_M00_AXI [get_bd_intf_ports HBM_AXI_04] [get_bd_intf_pins hbm_sc_04/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_05_M00_AXI [get_bd_intf_ports HBM_AXI_05] [get_bd_intf_pins hbm_sc_05/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_06_M00_AXI [get_bd_intf_ports HBM_AXI_06] [get_bd_intf_pins hbm_sc_06/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_07_M00_AXI [get_bd_intf_ports HBM_AXI_07] [get_bd_intf_pins hbm_sc_07/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_08_M00_AXI [get_bd_intf_ports HBM_AXI_08] [get_bd_intf_pins hbm_sc_08/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_09_M00_AXI [get_bd_intf_ports HBM_AXI_09] [get_bd_intf_pins hbm_sc_09/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_0_M00_AXI [get_bd_intf_ports HBM_AXI_00] [get_bd_intf_pins hbm_sc_00/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_10_M00_AXI [get_bd_intf_ports HBM_AXI_10] [get_bd_intf_pins hbm_sc_10/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_11_M00_AXI [get_bd_intf_ports HBM_AXI_11] [get_bd_intf_pins hbm_sc_11/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_12_M00_AXI [get_bd_intf_ports HBM_AXI_12] [get_bd_intf_pins hbm_sc_12/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_13_M00_AXI [get_bd_intf_ports HBM_AXI_13] [get_bd_intf_pins hbm_sc_13/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_14_M00_AXI [get_bd_intf_ports HBM_AXI_14] [get_bd_intf_pins hbm_sc_14/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_15_M00_AXI [get_bd_intf_ports HBM_AXI_15] [get_bd_intf_pins hbm_sc_15/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_16_M00_AXI [get_bd_intf_ports HBM_AXI_16] [get_bd_intf_pins hbm_sc_16/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_17_M00_AXI [get_bd_intf_ports HBM_AXI_17] [get_bd_intf_pins hbm_sc_17/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_18_M00_AXI [get_bd_intf_ports HBM_AXI_18] [get_bd_intf_pins hbm_sc_18/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_19_M00_AXI [get_bd_intf_ports HBM_AXI_19] [get_bd_intf_pins hbm_sc_19/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_20_M00_AXI [get_bd_intf_ports HBM_AXI_20] [get_bd_intf_pins hbm_sc_20/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_21_M00_AXI [get_bd_intf_ports HBM_AXI_21] [get_bd_intf_pins hbm_sc_21/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_22_M00_AXI [get_bd_intf_ports HBM_AXI_22] [get_bd_intf_pins hbm_sc_22/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_23_M00_AXI [get_bd_intf_ports HBM_AXI_23] [get_bd_intf_pins hbm_sc_23/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_24_M00_AXI [get_bd_intf_ports HBM_AXI_24] [get_bd_intf_pins hbm_sc_24/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_25_M00_AXI [get_bd_intf_ports HBM_AXI_25] [get_bd_intf_pins hbm_sc_25/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_26_M00_AXI [get_bd_intf_ports HBM_AXI_26] [get_bd_intf_pins hbm_sc_26/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_27_M00_AXI [get_bd_intf_ports HBM_AXI_27] [get_bd_intf_pins hbm_sc_27/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_28_M00_AXI [get_bd_intf_ports HBM_AXI_28] [get_bd_intf_pins hbm_sc_28/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_29_M00_AXI [get_bd_intf_ports HBM_AXI_29] [get_bd_intf_pins hbm_sc_29/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_30_M00_AXI [get_bd_intf_ports HBM_AXI_30] [get_bd_intf_pins hbm_sc_30/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_31_M00_AXI [get_bd_intf_ports HBM_AXI_31] [get_bd_intf_pins hbm_sc_31/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_32_M00_AXI [get_bd_intf_ports HBM_AXI_32] [get_bd_intf_pins hbm_sc_32/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_33_M00_AXI [get_bd_intf_ports HBM_AXI_33] [get_bd_intf_pins hbm_sc_33/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_34_M00_AXI [get_bd_intf_ports HBM_AXI_34] [get_bd_intf_pins hbm_sc_34/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_35_M00_AXI [get_bd_intf_ports HBM_AXI_35] [get_bd_intf_pins hbm_sc_35/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_36_M00_AXI [get_bd_intf_ports HBM_AXI_36] [get_bd_intf_pins hbm_sc_36/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_37_M00_AXI [get_bd_intf_ports HBM_AXI_37] [get_bd_intf_pins hbm_sc_37/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_38_M00_AXI [get_bd_intf_ports HBM_AXI_38] [get_bd_intf_pins hbm_sc_38/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_39_M00_AXI [get_bd_intf_ports HBM_AXI_39] [get_bd_intf_pins hbm_sc_39/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_40_M00_AXI [get_bd_intf_ports HBM_AXI_40] [get_bd_intf_pins hbm_sc_40/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_41_M00_AXI [get_bd_intf_ports HBM_AXI_41] [get_bd_intf_pins hbm_sc_41/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_42_M00_AXI [get_bd_intf_ports HBM_AXI_42] [get_bd_intf_pins hbm_sc_42/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_43_M00_AXI [get_bd_intf_ports HBM_AXI_43] [get_bd_intf_pins hbm_sc_43/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_44_M00_AXI [get_bd_intf_ports HBM_AXI_44] [get_bd_intf_pins hbm_sc_44/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_45_M00_AXI [get_bd_intf_ports HBM_AXI_45] [get_bd_intf_pins hbm_sc_45/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_46_M00_AXI [get_bd_intf_ports HBM_AXI_46] [get_bd_intf_pins hbm_sc_46/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_47_M00_AXI [get_bd_intf_ports HBM_AXI_47] [get_bd_intf_pins hbm_sc_47/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_48_M00_AXI [get_bd_intf_ports HBM_AXI_48] [get_bd_intf_pins hbm_sc_48/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_49_M00_AXI [get_bd_intf_ports HBM_AXI_49] [get_bd_intf_pins hbm_sc_49/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_50_M00_AXI [get_bd_intf_ports HBM_AXI_50] [get_bd_intf_pins hbm_sc_50/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_51_M00_AXI [get_bd_intf_ports HBM_AXI_51] [get_bd_intf_pins hbm_sc_51/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_52_M00_AXI [get_bd_intf_ports HBM_AXI_52] [get_bd_intf_pins hbm_sc_52/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_53_M00_AXI [get_bd_intf_ports HBM_AXI_53] [get_bd_intf_pins hbm_sc_53/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_54_M00_AXI [get_bd_intf_ports HBM_AXI_54] [get_bd_intf_pins hbm_sc_54/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_55_M00_AXI [get_bd_intf_ports HBM_AXI_55] [get_bd_intf_pins hbm_sc_55/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_56_M00_AXI [get_bd_intf_ports HBM_AXI_56] [get_bd_intf_pins hbm_sc_56/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_57_M00_AXI [get_bd_intf_ports HBM_AXI_57] [get_bd_intf_pins hbm_sc_57/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_58_M00_AXI [get_bd_intf_ports HBM_AXI_58] [get_bd_intf_pins hbm_sc_58/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_59_M00_AXI [get_bd_intf_ports HBM_AXI_59] [get_bd_intf_pins hbm_sc_59/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_60_M00_AXI [get_bd_intf_ports HBM_AXI_60] [get_bd_intf_pins hbm_sc_60/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_61_M00_AXI [get_bd_intf_ports HBM_AXI_61] [get_bd_intf_pins hbm_sc_61/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_62_M00_AXI [get_bd_intf_ports HBM_AXI_62] [get_bd_intf_pins hbm_sc_62/M00_AXI]
  connect_bd_intf_net -intf_net hbm_sc_63_M00_AXI [get_bd_intf_ports HBM_AXI_63] [get_bd_intf_pins hbm_sc_63/M00_AXI]
  connect_bd_intf_net -intf_net hbm_vnoc_00_M00_INI [get_bd_intf_ports HBM_VNOC_INI_00] [get_bd_intf_pins hbm_vnoc_00/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_01_M00_INI [get_bd_intf_ports HBM_VNOC_INI_01] [get_bd_intf_pins hbm_vnoc_01/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_02_M00_INI [get_bd_intf_ports HBM_VNOC_INI_02] [get_bd_intf_pins hbm_vnoc_02/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_03_M00_INI [get_bd_intf_ports HBM_VNOC_INI_03] [get_bd_intf_pins hbm_vnoc_03/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_04_M00_INI [get_bd_intf_ports HBM_VNOC_INI_04] [get_bd_intf_pins hbm_vnoc_04/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_05_M00_INI [get_bd_intf_ports HBM_VNOC_INI_05] [get_bd_intf_pins hbm_vnoc_05/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_06_M00_INI [get_bd_intf_ports HBM_VNOC_INI_06] [get_bd_intf_pins hbm_vnoc_06/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_07_M00_INI [get_bd_intf_ports HBM_VNOC_INI_07] [get_bd_intf_pins hbm_vnoc_07/M00_INI]
  connect_bd_intf_net -intf_net noc_virt_00_M00_INI [get_bd_intf_ports SL_VIRT_00] [get_bd_intf_pins noc_virt_00/M00_INI]
  connect_bd_intf_net -intf_net noc_virt_01_M00_INI [get_bd_intf_ports SL_VIRT_01] [get_bd_intf_pins qdma_slave_bridge_noc/M00_INI]
  connect_bd_intf_net -intf_net noc_virt_02_M00_INI [get_bd_intf_ports SL_VIRT_02] [get_bd_intf_pins noc_virt_02/M00_INI]
  connect_bd_intf_net -intf_net noc_virt_03_M00_INI [get_bd_intf_ports SL_VIRT_03] [get_bd_intf_pins noc_virt_03/M00_INI]
  connect_bd_intf_net -intf_net smartconnect_0_M00_AXI [get_bd_intf_pins hbm_bandwidth_0/s_axi_control] [get_bd_intf_pins smartconnect_0/M00_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M01_AXI [get_bd_intf_pins hbm_bandwidth_1/s_axi_control] [get_bd_intf_pins smartconnect_0/M01_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M02_AXI [get_bd_intf_pins hbm_bandwidth_2/s_axi_control] [get_bd_intf_pins smartconnect_0/M02_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M03_AXI [get_bd_intf_pins hbm_bandwidth_3/s_axi_control] [get_bd_intf_pins smartconnect_0/M03_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M04_AXI [get_bd_intf_pins hbm_bandwidth_4/s_axi_control] [get_bd_intf_pins smartconnect_0/M04_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M05_AXI [get_bd_intf_pins hbm_bandwidth_5/s_axi_control] [get_bd_intf_pins smartconnect_0/M05_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M06_AXI [get_bd_intf_pins hbm_bandwidth_6/s_axi_control] [get_bd_intf_pins smartconnect_0/M06_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M07_AXI [get_bd_intf_pins hbm_bandwidth_7/s_axi_control] [get_bd_intf_pins smartconnect_0/M07_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M08_AXI [get_bd_intf_pins hbm_bandwidth_8/s_axi_control] [get_bd_intf_pins smartconnect_0/M08_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M09_AXI [get_bd_intf_pins hbm_bandwidth_9/s_axi_control] [get_bd_intf_pins smartconnect_0/M09_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M10_AXI [get_bd_intf_pins hbm_bandwidth_10/s_axi_control] [get_bd_intf_pins smartconnect_0/M10_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M11_AXI [get_bd_intf_pins hbm_bandwidth_11/s_axi_control] [get_bd_intf_pins smartconnect_0/M11_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M12_AXI [get_bd_intf_pins hbm_bandwidth_12/s_axi_control] [get_bd_intf_pins smartconnect_0/M12_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M13_AXI [get_bd_intf_pins hbm_bandwidth_13/s_axi_control] [get_bd_intf_pins smartconnect_0/M13_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M14_AXI [get_bd_intf_pins hbm_bandwidth_14/s_axi_control] [get_bd_intf_pins smartconnect_0/M14_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M15_AXI [get_bd_intf_pins smartconnect_0/M15_AXI] [get_bd_intf_pins smartconnect_1/S00_AXI]
  connect_bd_intf_net -intf_net smartconnect_2_M00_AXI [get_bd_intf_pins smartconnect_1/M00_AXI] [get_bd_intf_pins hbm_bandwidth_15/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M01_AXI [get_bd_intf_pins smartconnect_1/M01_AXI] [get_bd_intf_pins hbm_bandwidth_16/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M02_AXI [get_bd_intf_pins smartconnect_1/M02_AXI] [get_bd_intf_pins hbm_bandwidth_17/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M03_AXI [get_bd_intf_pins smartconnect_1/M03_AXI] [get_bd_intf_pins hbm_bandwidth_18/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M04_AXI [get_bd_intf_pins smartconnect_1/M04_AXI] [get_bd_intf_pins hbm_bandwidth_19/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M05_AXI [get_bd_intf_pins smartconnect_1/M05_AXI] [get_bd_intf_pins hbm_bandwidth_20/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M06_AXI [get_bd_intf_pins smartconnect_1/M06_AXI] [get_bd_intf_pins hbm_bandwidth_21/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M07_AXI [get_bd_intf_pins smartconnect_1/M07_AXI] [get_bd_intf_pins hbm_bandwidth_22/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M08_AXI [get_bd_intf_pins smartconnect_1/M08_AXI] [get_bd_intf_pins hbm_bandwidth_23/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M09_AXI [get_bd_intf_pins smartconnect_1/M09_AXI] [get_bd_intf_pins hbm_bandwidth_24/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M10_AXI [get_bd_intf_pins smartconnect_1/M10_AXI] [get_bd_intf_pins hbm_bandwidth_25/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M11_AXI [get_bd_intf_pins smartconnect_1/M11_AXI] [get_bd_intf_pins hbm_bandwidth_26/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M12_AXI [get_bd_intf_pins smartconnect_1/M12_AXI] [get_bd_intf_pins hbm_bandwidth_27/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M13_AXI [get_bd_intf_pins smartconnect_1/M13_AXI] [get_bd_intf_pins hbm_bandwidth_28/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M14_AXI [get_bd_intf_pins smartconnect_1/M14_AXI] [get_bd_intf_pins hbm_bandwidth_29/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_2_M15_AXI [get_bd_intf_pins smartconnect_1/M15_AXI] [get_bd_intf_pins smartconnect_2/S00_AXI]
  connect_bd_intf_net -intf_net smartconnect_3_M00_AXI [get_bd_intf_pins smartconnect_2/M00_AXI] [get_bd_intf_pins hbm_bandwidth_30/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M01_AXI [get_bd_intf_pins smartconnect_2/M01_AXI] [get_bd_intf_pins hbm_bandwidth_31/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M02_AXI [get_bd_intf_pins smartconnect_2/M02_AXI] [get_bd_intf_pins hbm_bandwidth_32/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M03_AXI [get_bd_intf_pins smartconnect_2/M03_AXI] [get_bd_intf_pins hbm_bandwidth_33/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M04_AXI [get_bd_intf_pins smartconnect_2/M04_AXI] [get_bd_intf_pins hbm_bandwidth_34/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M05_AXI [get_bd_intf_pins smartconnect_2/M05_AXI] [get_bd_intf_pins hbm_bandwidth_35/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M06_AXI [get_bd_intf_pins smartconnect_2/M06_AXI] [get_bd_intf_pins hbm_bandwidth_36/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M07_AXI [get_bd_intf_pins smartconnect_2/M07_AXI] [get_bd_intf_pins hbm_bandwidth_37/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M08_AXI [get_bd_intf_pins smartconnect_2/M08_AXI] [get_bd_intf_pins hbm_bandwidth_38/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M09_AXI [get_bd_intf_pins smartconnect_2/M09_AXI] [get_bd_intf_pins hbm_bandwidth_39/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M10_AXI [get_bd_intf_pins smartconnect_2/M10_AXI] [get_bd_intf_pins hbm_bandwidth_40/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M11_AXI [get_bd_intf_pins smartconnect_2/M11_AXI] [get_bd_intf_pins hbm_bandwidth_41/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M12_AXI [get_bd_intf_pins smartconnect_2/M12_AXI] [get_bd_intf_pins hbm_bandwidth_42/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M13_AXI [get_bd_intf_pins smartconnect_2/M13_AXI] [get_bd_intf_pins hbm_bandwidth_43/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M14_AXI [get_bd_intf_pins smartconnect_2/M14_AXI] [get_bd_intf_pins hbm_bandwidth_44/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_3_M15_AXI [get_bd_intf_pins smartconnect_3/S00_AXI] [get_bd_intf_pins smartconnect_2/M15_AXI]
  connect_bd_intf_net -intf_net smartconnect_3_M15_AXI1 [get_bd_intf_pins smartconnect_3/M15_AXI] [get_bd_intf_pins smartconnect_4/S00_AXI]
  connect_bd_intf_net -intf_net smartconnect_4_M00_AXI [get_bd_intf_pins smartconnect_3/M00_AXI] [get_bd_intf_pins hbm_bandwidth_45/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M00_AXI1 [get_bd_intf_pins smartconnect_4/M00_AXI] [get_bd_intf_pins hbm_bandwidth_60/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M01_AXI [get_bd_intf_pins smartconnect_3/M01_AXI] [get_bd_intf_pins hbm_bandwidth_46/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M01_AXI1 [get_bd_intf_pins smartconnect_4/M01_AXI] [get_bd_intf_pins hbm_bandwidth_61/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M02_AXI [get_bd_intf_pins smartconnect_3/M02_AXI] [get_bd_intf_pins hbm_bandwidth_47/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M02_AXI1 [get_bd_intf_pins smartconnect_4/M02_AXI] [get_bd_intf_pins hbm_bandwidth_62/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M03_AXI [get_bd_intf_pins smartconnect_3/M03_AXI] [get_bd_intf_pins hbm_bandwidth_48/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M03_AXI1 [get_bd_intf_pins smartconnect_4/M03_AXI] [get_bd_intf_pins hbm_bandwidth_63/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M04_AXI [get_bd_intf_pins smartconnect_3/M04_AXI] [get_bd_intf_pins hbm_bandwidth_49/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M04_AXI1 [get_bd_intf_pins smartconnect_4/M04_AXI] [get_bd_intf_pins hbm_bandwidth_64/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M05_AXI [get_bd_intf_pins smartconnect_3/M05_AXI] [get_bd_intf_pins hbm_bandwidth_50/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M05_AXI1 [get_bd_intf_pins smartconnect_4/M05_AXI] [get_bd_intf_pins hbm_bandwidth_65/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M06_AXI [get_bd_intf_pins smartconnect_3/M06_AXI] [get_bd_intf_pins hbm_bandwidth_51/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M06_AXI1 [get_bd_intf_pins smartconnect_4/M06_AXI] [get_bd_intf_pins hbm_bandwidth_66/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M07_AXI [get_bd_intf_pins smartconnect_3/M07_AXI] [get_bd_intf_pins hbm_bandwidth_52/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M07_AXI1 [get_bd_intf_pins smartconnect_4/M07_AXI] [get_bd_intf_pins hbm_bandwidth_67/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M08_AXI [get_bd_intf_pins smartconnect_3/M08_AXI] [get_bd_intf_pins hbm_bandwidth_53/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M08_AXI1 [get_bd_intf_pins smartconnect_4/M08_AXI] [get_bd_intf_pins hbm_bandwidth_68/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M09_AXI [get_bd_intf_pins smartconnect_3/M09_AXI] [get_bd_intf_pins hbm_bandwidth_54/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M09_AXI1 [get_bd_intf_pins smartconnect_4/M09_AXI] [get_bd_intf_pins hbm_bandwidth_69/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M10_AXI [get_bd_intf_pins smartconnect_3/M10_AXI] [get_bd_intf_pins hbm_bandwidth_55/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M10_AXI1 [get_bd_intf_pins smartconnect_4/M10_AXI] [get_bd_intf_pins hbm_bandwidth_70/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M11_AXI [get_bd_intf_pins smartconnect_3/M11_AXI] [get_bd_intf_pins hbm_bandwidth_56/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M11_AXI1 [get_bd_intf_pins smartconnect_4/M11_AXI] [get_bd_intf_pins hbm_bandwidth_71/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M12_AXI [get_bd_intf_pins smartconnect_3/M12_AXI] [get_bd_intf_pins hbm_bandwidth_57/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M12_AXI1 [get_bd_intf_pins smartconnect_4/M12_AXI] [get_bd_intf_pins ddr_bandwidth_64/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M13_AXI [get_bd_intf_pins smartconnect_3/M13_AXI] [get_bd_intf_pins hbm_bandwidth_58/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M13_AXI1 [get_bd_intf_pins smartconnect_4/M13_AXI] [get_bd_intf_pins ddr_bandwidth_65/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M14_AXI [get_bd_intf_pins smartconnect_3/M14_AXI] [get_bd_intf_pins hbm_bandwidth_59/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M14_AXI1 [get_bd_intf_pins smartconnect_4/M14_AXI] [get_bd_intf_pins ddr_bandwidth_66/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_4_M15_AXI [get_bd_intf_pins smartconnect_5/S00_AXI] [get_bd_intf_pins smartconnect_4/M15_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M00_AXI [get_bd_intf_pins smartconnect_5/M00_AXI] [get_bd_intf_pins ddr_bandwidth_67/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_5_M01_AXI [get_bd_intf_pins traffic_producer_0/s_axi_control] [get_bd_intf_pins smartconnect_5/M01_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M02_AXI [get_bd_intf_pins traffic_producer_1/s_axi_control] [get_bd_intf_pins smartconnect_5/M02_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M03_AXI [get_bd_intf_pins traffic_producer_2/s_axi_control] [get_bd_intf_pins smartconnect_5/M03_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M04_AXI [get_bd_intf_pins traffic_producer_3/s_axi_control] [get_bd_intf_pins smartconnect_5/M04_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M05_AXI [get_bd_intf_pins traffic_producer_4/s_axi_control] [get_bd_intf_pins smartconnect_5/M05_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M06_AXI [get_bd_intf_pins traffic_producer_5/s_axi_control] [get_bd_intf_pins smartconnect_5/M06_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M07_AXI [get_bd_intf_pins traffic_producer_6/s_axi_control] [get_bd_intf_pins smartconnect_5/M07_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M08_AXI [get_bd_intf_pins traffic_producer_7/s_axi_control] [get_bd_intf_pins smartconnect_5/M08_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M09_AXI [get_bd_intf_pins traffic_virt_0/s_axi_control] [get_bd_intf_pins smartconnect_5/M09_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M10_AXI [get_bd_intf_pins traffic_virt_1/s_axi_control] [get_bd_intf_pins smartconnect_5/M10_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M11_AXI [get_bd_intf_pins traffic_virt_2/s_axi_control] [get_bd_intf_pins smartconnect_5/M11_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M12_AXI [get_bd_intf_pins traffic_virt_3/s_axi_control] [get_bd_intf_pins smartconnect_5/M12_AXI]
  connect_bd_intf_net -intf_net smartconnect_5_M13_AXI [get_bd_intf_pins traffic_virt_4/s_axi_control] [get_bd_intf_pins smartconnect_5/M13_AXI]
  connect_bd_intf_net -intf_net traffic_producer_0_axis_out [get_bd_intf_pins traffic_producer_0/axis_out] [get_bd_intf_pins dcmac_axis_noc_0/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_1_axis_out [get_bd_intf_pins traffic_producer_1/axis_out] [get_bd_intf_pins dcmac_axis_noc_1/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_2_axis_out [get_bd_intf_pins traffic_producer_2/axis_out] [get_bd_intf_pins dcmac_axis_noc_2/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_3_axis_out [get_bd_intf_pins traffic_producer_3/axis_out] [get_bd_intf_pins dcmac_axis_noc_3/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_4_axis_out [get_bd_intf_pins traffic_producer_4/axis_out] [get_bd_intf_pins dcmac_axis_noc_4/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_5_axis_out [get_bd_intf_pins traffic_producer_5/axis_out] [get_bd_intf_pins dcmac_axis_noc_5/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_6_axis_out [get_bd_intf_pins traffic_producer_6/axis_out] [get_bd_intf_pins dcmac_axis_noc_6/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_7_axis_out [get_bd_intf_pins traffic_producer_7/axis_out] [get_bd_intf_pins dcmac_axis_noc_7/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_virt_0_m_axi_gmem0 [get_bd_intf_pins noc_virt_00/S00_AXI] [get_bd_intf_pins traffic_virt_0/m_axi_gmem0]
  connect_bd_intf_net -intf_net traffic_virt_1_m_axi_gmem0 [get_bd_intf_pins qdma_slave_bridge_noc/S00_AXI] [get_bd_intf_pins traffic_virt_1/m_axi_gmem0]
  connect_bd_intf_net -intf_net traffic_virt_2_m_axi_gmem0 [get_bd_intf_pins noc_virt_02/S00_AXI] [get_bd_intf_pins traffic_virt_2/m_axi_gmem0]
  connect_bd_intf_net -intf_net traffic_virt_3_m_axi_gmem0 [get_bd_intf_pins noc_virt_03/S00_AXI] [get_bd_intf_pins traffic_virt_3/m_axi_gmem0]
  connect_bd_intf_net -intf_net traffic_virt_4_m_axi_gmem0 [get_bd_intf_pins axi_noc_1/S00_AXI] [get_bd_intf_pins traffic_virt_4/m_axi_gmem0]

  # Create port connections
  connect_bd_net -net arstn_1  [get_bd_ports arstn] \
  [get_bd_pins c_shift_ram_0/D]
  connect_bd_net -net c_shift_ram_0_Q  [get_bd_pins c_shift_ram_0/Q] \
  [get_bd_pins util_ds_buf_0/BUFG_FABRIC_I]
  connect_bd_net -net clk_wizard_0_clk_out1  [get_bd_ports user_clk] \
  [get_bd_pins ddr_noc_0/aclk0] \
  [get_bd_pins ddr_noc_3/aclk0] \
  [get_bd_pins ddr_noc_2/aclk0] \
  [get_bd_pins ddr_noc_1/aclk0] \
  [get_bd_pins hbm_vnoc_00/aclk0] \
  [get_bd_pins hbm_vnoc_01/aclk0] \
  [get_bd_pins hbm_vnoc_02/aclk0] \
  [get_bd_pins hbm_vnoc_03/aclk0] \
  [get_bd_pins hbm_vnoc_04/aclk0] \
  [get_bd_pins hbm_vnoc_05/aclk0] \
  [get_bd_pins hbm_vnoc_06/aclk0] \
  [get_bd_pins hbm_vnoc_07/aclk0] \
  [get_bd_pins dcmac_axis_noc_0/aclk0] \
  [get_bd_pins dcmac_axis_noc_1/aclk0] \
  [get_bd_pins dcmac_axis_noc_2/aclk0] \
  [get_bd_pins dcmac_axis_noc_3/aclk0] \
  [get_bd_pins dcmac_axis_noc_4/aclk0] \
  [get_bd_pins dcmac_axis_noc_5/aclk0] \
  [get_bd_pins dcmac_axis_noc_6/aclk0] \
  [get_bd_pins dcmac_axis_noc_7/aclk0] \
  [get_bd_pins dcmac_axis_noc_s_0/aclk0] \
  [get_bd_pins dcmac_axis_noc_s_1/aclk0] \
  [get_bd_pins dcmac_axis_noc_s_2/aclk0] \
  [get_bd_pins dcmac_axis_noc_s_3/aclk0] \
  [get_bd_pins dcmac_axis_noc_s_4/aclk0] \
  [get_bd_pins dcmac_axis_noc_s_5/aclk0] \
  [get_bd_pins dcmac_axis_noc_s_6/aclk0] \
  [get_bd_pins dcmac_axis_noc_s_7/aclk0] \
  [get_bd_pins traffic_producer_0/ap_clk] \
  [get_bd_pins traffic_producer_1/ap_clk] \
  [get_bd_pins traffic_producer_2/ap_clk] \
  [get_bd_pins traffic_producer_3/ap_clk] \
  [get_bd_pins traffic_producer_4/ap_clk] \
  [get_bd_pins traffic_producer_5/ap_clk] \
  [get_bd_pins traffic_producer_6/ap_clk] \
  [get_bd_pins traffic_producer_7/ap_clk] \
  [get_bd_pins ddr_bandwidth_64/ap_clk] \
  [get_bd_pins ddr_bandwidth_65/ap_clk] \
  [get_bd_pins ddr_bandwidth_66/ap_clk] \
  [get_bd_pins ddr_bandwidth_67/ap_clk] \
  [get_bd_pins hbm_bandwidth_0/ap_clk] \
  [get_bd_pins hbm_bandwidth_10/ap_clk] \
  [get_bd_pins hbm_bandwidth_11/ap_clk] \
  [get_bd_pins hbm_bandwidth_12/ap_clk] \
  [get_bd_pins hbm_bandwidth_13/ap_clk] \
  [get_bd_pins hbm_bandwidth_14/ap_clk] \
  [get_bd_pins hbm_bandwidth_15/ap_clk] \
  [get_bd_pins hbm_bandwidth_16/ap_clk] \
  [get_bd_pins hbm_bandwidth_17/ap_clk] \
  [get_bd_pins hbm_bandwidth_18/ap_clk] \
  [get_bd_pins hbm_bandwidth_19/ap_clk] \
  [get_bd_pins hbm_bandwidth_1/ap_clk] \
  [get_bd_pins hbm_bandwidth_20/ap_clk] \
  [get_bd_pins hbm_bandwidth_21/ap_clk] \
  [get_bd_pins hbm_bandwidth_22/ap_clk] \
  [get_bd_pins hbm_bandwidth_23/ap_clk] \
  [get_bd_pins hbm_bandwidth_24/ap_clk] \
  [get_bd_pins hbm_bandwidth_25/ap_clk] \
  [get_bd_pins hbm_bandwidth_26/ap_clk] \
  [get_bd_pins hbm_bandwidth_27/ap_clk] \
  [get_bd_pins hbm_bandwidth_28/ap_clk] \
  [get_bd_pins hbm_bandwidth_29/ap_clk] \
  [get_bd_pins hbm_bandwidth_2/ap_clk] \
  [get_bd_pins hbm_bandwidth_30/ap_clk] \
  [get_bd_pins hbm_bandwidth_31/ap_clk] \
  [get_bd_pins hbm_bandwidth_32/ap_clk] \
  [get_bd_pins hbm_bandwidth_33/ap_clk] \
  [get_bd_pins hbm_bandwidth_34/ap_clk] \
  [get_bd_pins hbm_bandwidth_35/ap_clk] \
  [get_bd_pins hbm_bandwidth_36/ap_clk] \
  [get_bd_pins hbm_bandwidth_37/ap_clk] \
  [get_bd_pins hbm_bandwidth_38/ap_clk] \
  [get_bd_pins hbm_bandwidth_39/ap_clk] \
  [get_bd_pins hbm_bandwidth_3/ap_clk] \
  [get_bd_pins hbm_bandwidth_40/ap_clk] \
  [get_bd_pins hbm_bandwidth_41/ap_clk] \
  [get_bd_pins hbm_bandwidth_42/ap_clk] \
  [get_bd_pins hbm_bandwidth_43/ap_clk] \
  [get_bd_pins hbm_bandwidth_44/ap_clk] \
  [get_bd_pins hbm_bandwidth_45/ap_clk] \
  [get_bd_pins hbm_bandwidth_46/ap_clk] \
  [get_bd_pins hbm_bandwidth_47/ap_clk] \
  [get_bd_pins hbm_bandwidth_48/ap_clk] \
  [get_bd_pins hbm_bandwidth_49/ap_clk] \
  [get_bd_pins hbm_bandwidth_4/ap_clk] \
  [get_bd_pins hbm_bandwidth_50/ap_clk] \
  [get_bd_pins hbm_bandwidth_51/ap_clk] \
  [get_bd_pins hbm_bandwidth_52/ap_clk] \
  [get_bd_pins hbm_bandwidth_53/ap_clk] \
  [get_bd_pins hbm_bandwidth_54/ap_clk] \
  [get_bd_pins hbm_bandwidth_55/ap_clk] \
  [get_bd_pins hbm_bandwidth_56/ap_clk] \
  [get_bd_pins hbm_bandwidth_57/ap_clk] \
  [get_bd_pins hbm_bandwidth_58/ap_clk] \
  [get_bd_pins hbm_bandwidth_59/ap_clk] \
  [get_bd_pins hbm_bandwidth_5/ap_clk] \
  [get_bd_pins hbm_bandwidth_60/ap_clk] \
  [get_bd_pins hbm_bandwidth_61/ap_clk] \
  [get_bd_pins hbm_bandwidth_62/ap_clk] \
  [get_bd_pins hbm_bandwidth_63/ap_clk] \
  [get_bd_pins hbm_bandwidth_64/ap_clk] \
  [get_bd_pins hbm_bandwidth_65/ap_clk] \
  [get_bd_pins hbm_bandwidth_66/ap_clk] \
  [get_bd_pins hbm_bandwidth_67/ap_clk] \
  [get_bd_pins hbm_bandwidth_68/ap_clk] \
  [get_bd_pins hbm_bandwidth_69/ap_clk] \
  [get_bd_pins hbm_bandwidth_6/ap_clk] \
  [get_bd_pins hbm_bandwidth_70/ap_clk] \
  [get_bd_pins hbm_bandwidth_71/ap_clk] \
  [get_bd_pins hbm_bandwidth_7/ap_clk] \
  [get_bd_pins hbm_bandwidth_8/ap_clk] \
  [get_bd_pins hbm_bandwidth_9/ap_clk] \
  [get_bd_pins traffic_virt_0/ap_clk] \
  [get_bd_pins traffic_virt_1/ap_clk] \
  [get_bd_pins traffic_virt_2/ap_clk] \
  [get_bd_pins traffic_virt_3/ap_clk] \
  [get_bd_pins traffic_virt_4/ap_clk] \
  [get_bd_pins smartconnect_1/aclk1] \
  [get_bd_pins smartconnect_0/aclk] \
  [get_bd_pins smartconnect_1/aclk] \
  [get_bd_pins smartconnect_2/aclk] \
  [get_bd_pins smartconnect_3/aclk] \
  [get_bd_pins smartconnect_4/aclk] \
  [get_bd_pins smartconnect_5/aclk] \
  [get_bd_pins hbm_sc_00/aclk] \
  [get_bd_pins hbm_sc_01/aclk] \
  [get_bd_pins hbm_sc_02/aclk] \
  [get_bd_pins hbm_sc_03/aclk] \
  [get_bd_pins hbm_sc_04/aclk] \
  [get_bd_pins hbm_sc_05/aclk] \
  [get_bd_pins hbm_sc_06/aclk] \
  [get_bd_pins hbm_sc_07/aclk] \
  [get_bd_pins hbm_sc_08/aclk] \
  [get_bd_pins hbm_sc_09/aclk] \
  [get_bd_pins hbm_sc_10/aclk] \
  [get_bd_pins hbm_sc_11/aclk] \
  [get_bd_pins hbm_sc_12/aclk] \
  [get_bd_pins hbm_sc_13/aclk] \
  [get_bd_pins hbm_sc_14/aclk] \
  [get_bd_pins hbm_sc_15/aclk] \
  [get_bd_pins hbm_sc_16/aclk] \
  [get_bd_pins hbm_sc_17/aclk] \
  [get_bd_pins hbm_sc_18/aclk] \
  [get_bd_pins hbm_sc_19/aclk] \
  [get_bd_pins hbm_sc_20/aclk] \
  [get_bd_pins hbm_sc_21/aclk] \
  [get_bd_pins hbm_sc_22/aclk] \
  [get_bd_pins hbm_sc_23/aclk] \
  [get_bd_pins hbm_sc_24/aclk] \
  [get_bd_pins hbm_sc_25/aclk] \
  [get_bd_pins hbm_sc_26/aclk] \
  [get_bd_pins hbm_sc_27/aclk] \
  [get_bd_pins hbm_sc_28/aclk] \
  [get_bd_pins hbm_sc_29/aclk] \
  [get_bd_pins hbm_sc_30/aclk] \
  [get_bd_pins hbm_sc_31/aclk] \
  [get_bd_pins hbm_sc_32/aclk] \
  [get_bd_pins hbm_sc_33/aclk] \
  [get_bd_pins hbm_sc_34/aclk] \
  [get_bd_pins hbm_sc_35/aclk] \
  [get_bd_pins hbm_sc_36/aclk] \
  [get_bd_pins hbm_sc_37/aclk] \
  [get_bd_pins hbm_sc_38/aclk] \
  [get_bd_pins hbm_sc_39/aclk] \
  [get_bd_pins hbm_sc_40/aclk] \
  [get_bd_pins hbm_sc_41/aclk] \
  [get_bd_pins hbm_sc_42/aclk] \
  [get_bd_pins hbm_sc_43/aclk] \
  [get_bd_pins hbm_sc_44/aclk] \
  [get_bd_pins hbm_sc_45/aclk] \
  [get_bd_pins hbm_sc_46/aclk] \
  [get_bd_pins hbm_sc_47/aclk] \
  [get_bd_pins hbm_sc_48/aclk] \
  [get_bd_pins hbm_sc_49/aclk] \
  [get_bd_pins hbm_sc_50/aclk] \
  [get_bd_pins hbm_sc_51/aclk] \
  [get_bd_pins hbm_sc_52/aclk] \
  [get_bd_pins hbm_sc_53/aclk] \
  [get_bd_pins hbm_sc_54/aclk] \
  [get_bd_pins hbm_sc_55/aclk] \
  [get_bd_pins hbm_sc_56/aclk] \
  [get_bd_pins hbm_sc_57/aclk] \
  [get_bd_pins hbm_sc_58/aclk] \
  [get_bd_pins hbm_sc_59/aclk] \
  [get_bd_pins hbm_sc_60/aclk] \
  [get_bd_pins hbm_sc_61/aclk] \
  [get_bd_pins hbm_sc_62/aclk] \
  [get_bd_pins hbm_sc_63/aclk] \
  [get_bd_pins noc_virt_00/aclk0] \
  [get_bd_pins qdma_slave_bridge_noc/aclk0] \
  [get_bd_pins noc_virt_02/aclk0] \
  [get_bd_pins noc_virt_03/aclk0] \
  [get_bd_pins axi_noc_1/aclk0] \
  [get_bd_pins axi_noc_0/aclk0] \
  [get_bd_pins c_shift_ram_0/CLK]
  connect_bd_net -net proc_sys_reset_0_interconnect_aresetn  [get_bd_pins ilreduced_logic_0/Res] \
  [get_bd_pins smartconnect_0/aresetn] \
  [get_bd_pins smartconnect_1/aresetn] \
  [get_bd_pins smartconnect_2/aresetn] \
  [get_bd_pins smartconnect_3/aresetn] \
  [get_bd_pins smartconnect_4/aresetn] \
  [get_bd_pins smartconnect_5/aresetn] \
  [get_bd_pins traffic_producer_0/ap_rst_n] \
  [get_bd_pins traffic_producer_1/ap_rst_n] \
  [get_bd_pins traffic_producer_2/ap_rst_n] \
  [get_bd_pins traffic_producer_3/ap_rst_n] \
  [get_bd_pins traffic_producer_4/ap_rst_n] \
  [get_bd_pins traffic_producer_5/ap_rst_n] \
  [get_bd_pins traffic_producer_6/ap_rst_n] \
  [get_bd_pins traffic_producer_7/ap_rst_n] \
  [get_bd_pins ddr_bandwidth_64/ap_rst_n] \
  [get_bd_pins ddr_bandwidth_65/ap_rst_n] \
  [get_bd_pins ddr_bandwidth_66/ap_rst_n] \
  [get_bd_pins ddr_bandwidth_67/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_0/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_10/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_11/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_12/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_13/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_14/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_15/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_16/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_17/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_18/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_19/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_1/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_20/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_21/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_22/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_23/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_24/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_25/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_26/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_27/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_28/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_29/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_2/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_30/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_31/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_32/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_33/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_34/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_35/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_36/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_37/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_38/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_39/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_3/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_40/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_41/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_42/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_43/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_44/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_45/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_46/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_47/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_48/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_49/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_4/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_50/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_51/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_52/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_53/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_54/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_55/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_56/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_57/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_58/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_59/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_5/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_60/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_61/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_62/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_63/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_64/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_65/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_66/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_67/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_68/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_69/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_6/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_70/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_71/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_7/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_8/ap_rst_n] \
  [get_bd_pins hbm_bandwidth_9/ap_rst_n] \
  [get_bd_pins traffic_virt_0/ap_rst_n] \
  [get_bd_pins traffic_virt_1/ap_rst_n] \
  [get_bd_pins traffic_virt_2/ap_rst_n] \
  [get_bd_pins traffic_virt_3/ap_rst_n] \
  [get_bd_pins traffic_virt_4/ap_rst_n] \
  [get_bd_pins hbm_sc_00/aresetn] \
  [get_bd_pins hbm_sc_01/aresetn] \
  [get_bd_pins hbm_sc_02/aresetn] \
  [get_bd_pins hbm_sc_03/aresetn] \
  [get_bd_pins hbm_sc_04/aresetn] \
  [get_bd_pins hbm_sc_05/aresetn] \
  [get_bd_pins hbm_sc_06/aresetn] \
  [get_bd_pins hbm_sc_07/aresetn] \
  [get_bd_pins hbm_sc_08/aresetn] \
  [get_bd_pins hbm_sc_09/aresetn] \
  [get_bd_pins hbm_sc_10/aresetn] \
  [get_bd_pins hbm_sc_11/aresetn] \
  [get_bd_pins hbm_sc_12/aresetn] \
  [get_bd_pins hbm_sc_13/aresetn] \
  [get_bd_pins hbm_sc_14/aresetn] \
  [get_bd_pins hbm_sc_15/aresetn] \
  [get_bd_pins hbm_sc_16/aresetn] \
  [get_bd_pins hbm_sc_17/aresetn] \
  [get_bd_pins hbm_sc_18/aresetn] \
  [get_bd_pins hbm_sc_19/aresetn] \
  [get_bd_pins hbm_sc_20/aresetn] \
  [get_bd_pins hbm_sc_21/aresetn] \
  [get_bd_pins hbm_sc_22/aresetn] \
  [get_bd_pins hbm_sc_23/aresetn] \
  [get_bd_pins hbm_sc_24/aresetn] \
  [get_bd_pins hbm_sc_25/aresetn] \
  [get_bd_pins hbm_sc_26/aresetn] \
  [get_bd_pins hbm_sc_27/aresetn] \
  [get_bd_pins hbm_sc_28/aresetn] \
  [get_bd_pins hbm_sc_29/aresetn] \
  [get_bd_pins hbm_sc_30/aresetn] \
  [get_bd_pins hbm_sc_31/aresetn] \
  [get_bd_pins hbm_sc_32/aresetn] \
  [get_bd_pins hbm_sc_33/aresetn] \
  [get_bd_pins hbm_sc_34/aresetn] \
  [get_bd_pins hbm_sc_35/aresetn] \
  [get_bd_pins hbm_sc_36/aresetn] \
  [get_bd_pins hbm_sc_37/aresetn] \
  [get_bd_pins hbm_sc_38/aresetn] \
  [get_bd_pins hbm_sc_39/aresetn] \
  [get_bd_pins hbm_sc_40/aresetn] \
  [get_bd_pins hbm_sc_41/aresetn] \
  [get_bd_pins hbm_sc_42/aresetn] \
  [get_bd_pins hbm_sc_43/aresetn] \
  [get_bd_pins hbm_sc_44/aresetn] \
  [get_bd_pins hbm_sc_45/aresetn] \
  [get_bd_pins hbm_sc_46/aresetn] \
  [get_bd_pins hbm_sc_47/aresetn] \
  [get_bd_pins hbm_sc_48/aresetn] \
  [get_bd_pins hbm_sc_49/aresetn] \
  [get_bd_pins hbm_sc_50/aresetn] \
  [get_bd_pins hbm_sc_51/aresetn] \
  [get_bd_pins hbm_sc_52/aresetn] \
  [get_bd_pins hbm_sc_53/aresetn] \
  [get_bd_pins hbm_sc_54/aresetn] \
  [get_bd_pins hbm_sc_55/aresetn] \
  [get_bd_pins hbm_sc_56/aresetn] \
  [get_bd_pins hbm_sc_57/aresetn] \
  [get_bd_pins hbm_sc_58/aresetn] \
  [get_bd_pins hbm_sc_59/aresetn] \
  [get_bd_pins hbm_sc_60/aresetn] \
  [get_bd_pins hbm_sc_61/aresetn] \
  [get_bd_pins hbm_sc_62/aresetn] \
  [get_bd_pins hbm_sc_63/aresetn]
  connect_bd_net -net static_region_clk_1  [get_bd_ports static_region_clk] \
  [get_bd_pins hbm_sc_00/aclk1] \
  [get_bd_pins hbm_sc_01/aclk1] \
  [get_bd_pins hbm_sc_02/aclk1] \
  [get_bd_pins hbm_sc_03/aclk1] \
  [get_bd_pins hbm_sc_04/aclk1] \
  [get_bd_pins hbm_sc_05/aclk1] \
  [get_bd_pins hbm_sc_06/aclk1] \
  [get_bd_pins hbm_sc_07/aclk1] \
  [get_bd_pins hbm_sc_08/aclk1] \
  [get_bd_pins hbm_sc_09/aclk1] \
  [get_bd_pins hbm_sc_10/aclk1] \
  [get_bd_pins hbm_sc_11/aclk1] \
  [get_bd_pins hbm_sc_12/aclk1] \
  [get_bd_pins hbm_sc_13/aclk1] \
  [get_bd_pins hbm_sc_14/aclk1] \
  [get_bd_pins hbm_sc_15/aclk1] \
  [get_bd_pins hbm_sc_16/aclk1] \
  [get_bd_pins hbm_sc_17/aclk1] \
  [get_bd_pins hbm_sc_18/aclk1] \
  [get_bd_pins hbm_sc_19/aclk1] \
  [get_bd_pins hbm_sc_20/aclk1] \
  [get_bd_pins hbm_sc_21/aclk1] \
  [get_bd_pins hbm_sc_22/aclk1] \
  [get_bd_pins hbm_sc_23/aclk1] \
  [get_bd_pins hbm_sc_24/aclk1] \
  [get_bd_pins hbm_sc_25/aclk1] \
  [get_bd_pins hbm_sc_26/aclk1] \
  [get_bd_pins hbm_sc_27/aclk1] \
  [get_bd_pins hbm_sc_28/aclk1] \
  [get_bd_pins hbm_sc_29/aclk1] \
  [get_bd_pins hbm_sc_30/aclk1] \
  [get_bd_pins hbm_sc_31/aclk1] \
  [get_bd_pins hbm_sc_32/aclk1] \
  [get_bd_pins hbm_sc_33/aclk1] \
  [get_bd_pins hbm_sc_34/aclk1] \
  [get_bd_pins hbm_sc_35/aclk1] \
  [get_bd_pins hbm_sc_36/aclk1] \
  [get_bd_pins hbm_sc_37/aclk1] \
  [get_bd_pins hbm_sc_38/aclk1] \
  [get_bd_pins hbm_sc_39/aclk1] \
  [get_bd_pins hbm_sc_40/aclk1] \
  [get_bd_pins hbm_sc_41/aclk1] \
  [get_bd_pins hbm_sc_42/aclk1] \
  [get_bd_pins hbm_sc_43/aclk1] \
  [get_bd_pins hbm_sc_44/aclk1] \
  [get_bd_pins hbm_sc_45/aclk1] \
  [get_bd_pins hbm_sc_46/aclk1] \
  [get_bd_pins hbm_sc_47/aclk1] \
  [get_bd_pins hbm_sc_48/aclk1] \
  [get_bd_pins hbm_sc_49/aclk1] \
  [get_bd_pins hbm_sc_50/aclk1] \
  [get_bd_pins hbm_sc_51/aclk1] \
  [get_bd_pins hbm_sc_52/aclk1] \
  [get_bd_pins hbm_sc_53/aclk1] \
  [get_bd_pins hbm_sc_54/aclk1] \
  [get_bd_pins hbm_sc_55/aclk1] \
  [get_bd_pins hbm_sc_56/aclk1] \
  [get_bd_pins hbm_sc_57/aclk1] \
  [get_bd_pins hbm_sc_58/aclk1] \
  [get_bd_pins hbm_sc_59/aclk1] \
  [get_bd_pins hbm_sc_60/aclk1] \
  [get_bd_pins hbm_sc_61/aclk1] \
  [get_bd_pins hbm_sc_62/aclk1] \
  [get_bd_pins hbm_sc_63/aclk1]
  connect_bd_net -net util_ds_buf_0_BUFG_FABRIC_O  [get_bd_pins util_ds_buf_0/BUFG_FABRIC_O] \
  [get_bd_pins ilreduced_logic_0/Op1]
  connect_bd_net -net xlconstant_0_dout  [get_bd_pins xlconstant_0/dout] \
  [get_bd_pins dcmac_axis_noc_s_0/M00_AXIS_tready] \
  [get_bd_pins dcmac_axis_noc_s_1/M00_AXIS_tready] \
  [get_bd_pins dcmac_axis_noc_s_2/M00_AXIS_tready] \
  [get_bd_pins dcmac_axis_noc_s_3/M00_AXIS_tready] \
  [get_bd_pins dcmac_axis_noc_s_4/M00_AXIS_tready] \
  [get_bd_pins dcmac_axis_noc_s_5/M00_AXIS_tready] \
  [get_bd_pins dcmac_axis_noc_s_6/M00_AXIS_tready] \
  [get_bd_pins dcmac_axis_noc_s_7/M00_AXIS_tready]

  # Create address segments
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces ddr_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs M00_INI/Reg] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces ddr_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs M01_INI/Reg] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces ddr_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs M02_INI/Reg] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces ddr_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs M03_INI/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM0_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_0/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_00/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM0_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_1/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_01/Reg] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM2_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_10/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_10/Reg] -force
  assign_bd_address -offset 0x004140000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM2_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_11/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_11/Reg] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM3_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_12/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_12/Reg] -force
  assign_bd_address -offset 0x004180000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM3_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_13/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_13/Reg] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM3_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_14/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_14/Reg] -force
  assign_bd_address -offset 0x0041C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM3_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_15/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_15/Reg] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM4_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_16/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_16/Reg] -force
  assign_bd_address -offset 0x004200000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM4_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_17/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_17/Reg] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM4_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_18/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_18/Reg] -force
  assign_bd_address -offset 0x004240000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM4_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_19/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_19/Reg] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM0_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_2/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_02/Reg] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM5_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_20/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_20/Reg] -force
  assign_bd_address -offset 0x004280000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM5_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_21/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_21/Reg] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM5_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_22/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_22/Reg] -force
  assign_bd_address -offset 0x0042C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM5_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_23/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_23/Reg] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM6_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_24/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_24/Reg] -force
  assign_bd_address -offset 0x004300000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM6_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_25/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_25/Reg] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM6_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_26/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_26/Reg] -force
  assign_bd_address -offset 0x004340000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM6_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_27/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_27/Reg] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM7_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_28/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_28/Reg] -force
  assign_bd_address -offset 0x004380000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM7_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_29/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_29/Reg] -force
  assign_bd_address -offset 0x004040000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM0_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_3/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_03/Reg] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM7_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_30/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_30/Reg] -force
  assign_bd_address -offset 0x0043C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM7_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_31/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_31/Reg] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM8_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_32/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_32/Reg] -force
  assign_bd_address -offset 0x004400000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM8_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_33/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_33/Reg] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM8_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_34/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_34/Reg] -force
  assign_bd_address -offset 0x004440000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM8_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_35/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_35/Reg] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM9_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_36/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_36/Reg] -force
  assign_bd_address -offset 0x004480000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM9_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_37/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_37/Reg] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM9_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_38/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_38/Reg] -force
  assign_bd_address -offset 0x0044C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM9_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_39/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_39/Reg] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM1_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_4/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_04/Reg] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM10_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_40/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_40/Reg] -force
  assign_bd_address -offset 0x004500000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM10_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_41/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_41/Reg] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM10_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_42/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_42/Reg] -force
  assign_bd_address -offset 0x004540000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM10_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_43/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_43/Reg] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM11_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_44/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_44/Reg] -force
  assign_bd_address -offset 0x004580000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM11_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_45/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_45/Reg] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM11_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_46/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_46/Reg] -force
  assign_bd_address -offset 0x0045C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM11_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_47/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_47/Reg] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM12_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_48/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_48/Reg] -force
  assign_bd_address -offset 0x004600000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM12_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_49/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_49/Reg] -force
  assign_bd_address -offset 0x004080000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM1_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_5/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_05/Reg] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM12_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_50/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_50/Reg] -force
  assign_bd_address -offset 0x004640000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM12_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_51/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_51/Reg] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM13_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_52/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_52/Reg] -force
  assign_bd_address -offset 0x004680000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM13_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_53/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_53/Reg] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM13_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_54/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_54/Reg] -force
  assign_bd_address -offset 0x0046C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM13_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_55/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_55/Reg] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM14_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_56/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_56/Reg] -force
  assign_bd_address -offset 0x004700000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM14_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_57/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_57/Reg] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM14_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_58/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_58/Reg] -force
  assign_bd_address -offset 0x004740000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM14_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_59/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_59/Reg] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM1_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_6/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_06/Reg] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM15_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_60/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_60/Reg] -force
  assign_bd_address -offset 0x004780000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM15_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_61/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_61/Reg] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM15_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_62/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_62/Reg] -force
  assign_bd_address -offset 0x0047C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM15_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_63/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_63/Reg] -force
  assign_bd_address -offset 0x0040C0000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM1_PC1 -target_address_space [get_bd_addr_spaces hbm_bandwidth_7/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_07/Reg] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM2_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_8/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_08/Reg] -force
  assign_bd_address -offset 0x004100000000 -range 0x40000000 -with_name SEG_axi_noc_cips_HBM2_PC0 -target_address_space [get_bd_addr_spaces hbm_bandwidth_9/Data_m_axi_gmem0] [get_bd_addr_segs HBM_AXI_09/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces hbm_bandwidth_64/Data_m_axi_gmem0] [get_bd_addr_segs HBM_VNOC_INI_00/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces hbm_bandwidth_65/Data_m_axi_gmem0] [get_bd_addr_segs HBM_VNOC_INI_01/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces hbm_bandwidth_66/Data_m_axi_gmem0] [get_bd_addr_segs HBM_VNOC_INI_02/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces hbm_bandwidth_67/Data_m_axi_gmem0] [get_bd_addr_segs HBM_VNOC_INI_03/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces hbm_bandwidth_68/Data_m_axi_gmem0] [get_bd_addr_segs HBM_VNOC_INI_04/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces hbm_bandwidth_69/Data_m_axi_gmem0] [get_bd_addr_segs HBM_VNOC_INI_05/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces hbm_bandwidth_70/Data_m_axi_gmem0] [get_bd_addr_segs HBM_VNOC_INI_06/Reg] -force
  assign_bd_address -offset 0x004000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces hbm_bandwidth_71/Data_m_axi_gmem0] [get_bd_addr_segs HBM_VNOC_INI_07/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -with_name SEG_SL_VIRT_0_Reg -target_address_space [get_bd_addr_spaces traffic_virt_0/Data_m_axi_gmem0] [get_bd_addr_segs SL_VIRT_00/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -with_name SEG_SL_VIRT_1_Reg -target_address_space [get_bd_addr_spaces traffic_virt_1/Data_m_axi_gmem0] [get_bd_addr_segs SL_VIRT_01/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -with_name SEG_SL_VIRT_2_Reg -target_address_space [get_bd_addr_spaces traffic_virt_2/Data_m_axi_gmem0] [get_bd_addr_segs SL_VIRT_02/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -with_name SEG_SL_VIRT_3_Reg -target_address_space [get_bd_addr_spaces traffic_virt_3/Data_m_axi_gmem0] [get_bd_addr_segs SL_VIRT_03/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -with_name SEG_QDMA_SLAVE_BRIDGE_Reg -target_address_space [get_bd_addr_spaces traffic_virt_4/Data_m_axi_gmem0] [get_bd_addr_segs QDMA_SLAVE_BRIDGE_0/Reg] -force
  assign_bd_address -offset 0x020200480000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs ddr_bandwidth_64/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200490000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs ddr_bandwidth_65/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs ddr_bandwidth_66/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs ddr_bandwidth_67/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_10/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_11/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_12/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_13/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_14/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202000F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_15/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200100000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_16/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200110000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_17/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200120000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_18/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200130000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_19/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200140000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_20/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200150000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_21/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200160000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_22/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200170000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_23/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200180000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_24/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200190000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_25/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_26/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_27/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_28/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_29/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200020000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_30/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202001F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_31/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200200000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_32/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200210000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_33/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200220000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_34/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200230000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_35/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200240000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_36/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200250000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_37/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200260000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_38/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200270000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_39/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200030000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200280000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_40/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200290000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_41/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_42/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_43/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_44/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_45/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_46/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202002F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_47/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200300000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_48/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200310000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_49/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200040000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200320000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_50/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200330000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_51/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200340000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_52/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200350000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_53/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200360000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_54/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200370000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_55/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200380000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_56/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200390000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_57/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_58/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_59/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200050000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_60/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_61/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_62/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202003F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_63/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200400000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_64/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200410000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_65/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200420000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_66/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200430000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_67/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200440000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_68/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200450000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_69/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200060000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200460000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_70/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200470000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_71/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200070000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200080000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_8/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200090000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs hbm_bandwidth_9/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004C0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0202004F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200500000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200510000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200520000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200530000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200540000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_virt_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200550000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_virt_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200560000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_virt_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200570000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_virt_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020200580000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_virt_4/s_axi_control/Reg] -force

  set_property USAGE memory [get_bd_addr_segs HBM_AXI_00/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_01/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_10/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_11/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_12/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_13/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_14/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_15/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_16/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_17/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_18/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_19/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_02/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_20/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_21/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_22/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_23/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_24/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_25/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_26/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_27/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_28/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_29/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_03/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_30/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_31/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_32/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_33/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_34/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_35/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_36/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_37/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_38/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_39/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_04/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_40/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_41/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_42/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_43/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_44/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_45/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_46/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_47/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_48/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_49/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_05/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_50/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_51/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_52/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_53/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_54/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_55/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_56/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_57/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_58/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_59/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_06/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_60/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_61/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_62/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_63/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_07/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_08/Reg]
  set_property USAGE memory [get_bd_addr_segs HBM_AXI_09/Reg]


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


