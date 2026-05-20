# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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
# source service_layer_script.tcl


# The design that will be created by this Tcl script contains the following 
# module references:
# dcmac_syncer_reset, dcmac_syncer_reset, clock_to_clock_bus, clock_to_serdes, clock_to_clock_bus, clock_to_serdes, clock_to_serdes, dcmac200g_ctl_port, clock_to_clock_bus, clock_to_serdes, axis_seg_to_unseg_converter, axis_unseg_to_seg_converter, dcmac_syncer_reset, dcmac_syncer_reset, clock_to_clock_bus, clock_to_serdes, clock_to_clock_bus, clock_to_serdes, clock_to_serdes, dcmac200g_ctl_port, clock_to_clock_bus, clock_to_serdes, axis_seg_to_unseg_converter, axis_unseg_to_seg_converter

# Please add the sources of those modules before sourcing this Tcl script.

# If there is no project opened, this script will create a
# project, but make sure you do not have an existing project
# <./myproj/project_1.xpr> in the current working folder.

set list_projs [get_projects -quiet]
if { $list_projs eq "" } {
   create_project project_1 myproj -part xcv80-lsva4737-2MHP-e-S
}


# CHANGE DESIGN NAME HERE
variable design_name
set design_name service_layer

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
xilinx.com:ip:axis_noc:1.0\
xilinx.com:hls:hbm_bandwidth:1.0\
xilinx.com:ip:axi_noc:*\
xilinx.com:ip:smartconnect:1.0\
xilinx.com:hls:traffic_producer:1.0\
xilinx.com:ip:xlconstant:1.1\
xilinx.com:ip:clk_wizard:1.0\
xilinx.com:ip:proc_sys_reset:5.0\
xilinx.com:ip:axis_dwidth_converter:1.1\
xilinx.com:ip:axis_data_fifo:2.0\
xilinx.com:ip:util_vector_logic:2.0\
xilinx.com:ip:xlslice:1.0\
xilinx.com:ip:xlconcat:2.1\
xilinx.com:inline_hdl:ilreduced_logic:1.0\
xilinx.com:ip:dcmac:3.0\
xilinx.com:ip:axi_gpio:2.0\
xilinx.com:ip:util_ds_buf:2.2\
xilinx.com:ip:gt_quad_base:1.1\
xilinx.com:ip:bufg_gt:1.0\
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
# CHECK Modules
##################################################################
set bCheckModules 0
if { $bCheckModules == 1 } {
   set list_check_mods "\ 
dcmac_syncer_reset\
dcmac_syncer_reset\
clock_to_clock_bus\
clock_to_serdes\
clock_to_clock_bus\
clock_to_serdes\
clock_to_serdes\
dcmac200g_ctl_port\
clock_to_clock_bus\
clock_to_serdes\
axis_seg_to_unseg_converter\
axis_unseg_to_seg_converter\
dcmac_syncer_reset\
dcmac_syncer_reset\
clock_to_clock_bus\
clock_to_serdes\
clock_to_clock_bus\
clock_to_serdes\
clock_to_serdes\
dcmac200g_ctl_port\
clock_to_clock_bus\
clock_to_serdes\
axis_seg_to_unseg_converter\
axis_unseg_to_seg_converter\
"

   set list_mods_missing ""
   common::send_gid_msg -ssname BD::TCL -id 2020 -severity "INFO" "Checking if the following modules exist in the project's sources: $list_check_mods ."

   foreach mod_vlnv $list_check_mods {
      if { [can_resolve_reference $mod_vlnv] == 0 } {
         lappend list_mods_missing $mod_vlnv
      }
   }

   if { $list_mods_missing ne "" } {
      catch {common::send_gid_msg -ssname BD::TCL -id 2021 -severity "ERROR" "The following module(s) are not found in the project: $list_mods_missing" }
      common::send_gid_msg -ssname BD::TCL -id 2022 -severity "INFO" "Please add source files for the missing module(s) above."
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

proc add_dcmac_inst {} {

  set DCMAC0_ENABLED 1
  set DCMAC1_ENABLED 1

  ## Each DCMAC can support 2 QSFP56 interfaces
  ## select how many QSFP56 you want for each DCMAC, provided they are enabled

  ## Setup number of QSFP56 interfaces for DCMAC0
  set DUAL_QSFP_DCMAC0 0

  ## Setup number of QSFP56 interfaces for DCMAC1
  set DUAL_QSFP_DCMAC1 0

    # Create network hierarchy
    if { ${DCMAC0_ENABLED} == "1" } {
        create_qsfp_hierarchy 0 ${DUAL_QSFP_DCMAC0}
    }
    if { ${DCMAC1_ENABLED} == "1" } {
        create_qsfp_hierarchy 1 ${DUAL_QSFP_DCMAC1}
    }
}
set current_file [file normalize [info script]]
set current_dir [file normalize ${current_file}]
set dcmac_base [file normalize [file join $current_dir .. .. .. dcmac]]

# Absolute paths (normalized)
set ::slash_dcmac_tcl  [file join $dcmac_base tcl dcmac.tcl]
set ::slash_dcmac_hdl  [file join $dcmac_base hdl]

# Source the DCMAC Tcl helpers
source $::slash_dcmac_tcl

# Import DCMAC source files
import_files -fileset sources_1 -norecurse [file join $::slash_dcmac_hdl axis_seg_to_unseg_converter.v]
import_files -fileset sources_1 -norecurse [file join $::slash_dcmac_hdl clock_to_clock_bus.v]
import_files -fileset sources_1 -norecurse [file join $::slash_dcmac_hdl dcmac200g_ctl_port.v]
import_files -fileset sources_1 -norecurse [file join $::slash_dcmac_hdl serdes_clock.v]
import_files -fileset sources_1 -norecurse [file join $::slash_dcmac_hdl syncer_reset.v]

# --- DCMAC creation variables ---
set DCMAC0_ENABLED 1
set DCMAC1_ENABLED 1
set DUAL_QSFP_DCMAC0 0
set DUAL_QSFP_DCMAC1 0


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
  set M_DCMAC_INIS0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS0 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS0

  set M_DCMAC_INIS1 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS1 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS1

  set M_DCMAC_INIS2 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS2 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS2

  set M_DCMAC_INIS3 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS3 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS3

  set M_DCMAC_INIS4 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS4 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS4

  set M_DCMAC_INIS5 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS5 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS5

  set M_DCMAC_INIS6 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS6 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS6

  set M_DCMAC_INIS7 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inis_rtl:1.0 M_DCMAC_INIS7 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_DCMAC_INIS7

  set M_VIRT_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M_VIRT_0 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_VIRT_0

  set M_VIRT_1 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M_VIRT_1 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_VIRT_1

  set M_VIRT_2 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M_VIRT_2 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_VIRT_2

  set M_VIRT_3 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M_VIRT_3 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_VIRT_3

  set S_DCMAC_INIS0 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS0 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS0

  set S_DCMAC_INIS1 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS1 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS1

  set S_DCMAC_INIS2 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS2 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS2

  set S_DCMAC_INIS3 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS3 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS3

  set S_DCMAC_INIS4 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS4 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS4

  set S_DCMAC_INIS5 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS5 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS5

  set S_DCMAC_INIS6 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS6 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS6

  set S_DCMAC_INIS7 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inis_rtl:1.0 S_DCMAC_INIS7 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_DCMAC_INIS7

  set SL2NOC_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL2NOC_0 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL2NOC_0

  set SL2NOC_1 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL2NOC_1 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL2NOC_1

  set SL2NOC_2 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL2NOC_2 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL2NOC_2

  set SL2NOC_3 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL2NOC_3 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL2NOC_3

  set SL2NOC_4 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL2NOC_4 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL2NOC_4

  set SL2NOC_5 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL2NOC_5 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL2NOC_5

  set SL2NOC_6 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL2NOC_6 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL2NOC_6

  set SL2NOC_7 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 SL2NOC_7 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $SL2NOC_7

  set qsfp1_4x [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp1_4x ]

  set qsfp3_4x [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp3_4x ]

  set S_AXILITE_INI [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S_AXILITE_INI ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_AXILITE_INI

  set S_VIRT_00 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S_VIRT_00 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_VIRT_00

  set S_VIRT_01 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S_VIRT_01 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_VIRT_01

  set S_VIRT_02 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S_VIRT_02 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_VIRT_02

  set S_VIRT_03 [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S_VIRT_03 ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_VIRT_03

  set S_QDMA_SLV_BRIDGE [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 S_QDMA_SLV_BRIDGE ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {load} \
   CONFIG.INI_STRATEGY {load} \
   ] $S_QDMA_SLV_BRIDGE

  set M_QDMA_SLV_BRIDGE [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 M_QDMA_SLV_BRIDGE ]
  set_property -dict [ list \
   CONFIG.COMPUTED_STRATEGY {driver} \
   CONFIG.INI_STRATEGY {driver} \
   ] $M_QDMA_SLV_BRIDGE

  set qsfp0_4x [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp0_4x ]

  set qsfp0_322mhz [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 qsfp0_322mhz ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ {322265625} \
   ] $qsfp0_322mhz

  set qsfp2_4x [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp2_4x ]

  set qsfp2_322mhz [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 qsfp2_322mhz ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ {322265625} \
   ] $qsfp2_322mhz


  # Create ports
  set service_clk [ create_bd_port -dir I -type clk -freq_hz 300000000 service_clk ]
  set_property -dict [ list \
   CONFIG.ASSOCIATED_RESET {arstn} \
   CONFIG.CLK_DOMAIN {bd_4885_pspmc_0_0_pl0_ref_clk} \
 ] $service_clk
  set arstn [ create_bd_port -dir I -type rst arstn ]

  # Create instance: dummy_noc_0, and set properties
  set dummy_noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dummy_noc_0


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_0/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dummy_noc_0/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dummy_noc_0/aclk0]

  # Create instance: dummy_noc_1, and set properties
  set dummy_noc_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_1 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dummy_noc_1


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_1/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dummy_noc_1/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dummy_noc_1/aclk0]

  # Create instance: dummy_noc_2, and set properties
  set dummy_noc_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_2 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dummy_noc_2


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_2/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dummy_noc_2/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dummy_noc_2/aclk0]

  # Create instance: dummy_noc_3, and set properties
  set dummy_noc_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_3 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dummy_noc_3


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_3/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dummy_noc_3/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dummy_noc_3/aclk0]

  # Create instance: dummy_noc_4, and set properties
  set dummy_noc_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_4 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dummy_noc_4


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_4/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dummy_noc_4/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dummy_noc_4/aclk0]

  # Create instance: dummy_noc_5, and set properties
  set dummy_noc_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_5 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dummy_noc_5


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_5/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dummy_noc_5/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dummy_noc_5/aclk0]

  # Create instance: dummy_noc_6, and set properties
  set dummy_noc_6 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_6 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dummy_noc_6


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_6/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dummy_noc_6/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dummy_noc_6/aclk0]

  # Create instance: dummy_noc_7, and set properties
  set dummy_noc_7 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_7 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $dummy_noc_7


  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_7/M00_AXIS]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXIS { write_bw {500} write_avg_burst {4}}} \
   CONFIG.DEST_IDS {} \
 ] [get_bd_intf_pins /dummy_noc_7/S00_INIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXIS} \
 ] [get_bd_pins /dummy_noc_7/aclk0]

  # Create instance: dummy_noc_m_0, and set properties
  set dummy_noc_m_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_m_0 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dummy_noc_m_0


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dummy_noc_m_0/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_m_0/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dummy_noc_m_0/aclk0]

  # Create instance: dummy_noc_m_1, and set properties
  set dummy_noc_m_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_m_1 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dummy_noc_m_1


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dummy_noc_m_1/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_m_1/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dummy_noc_m_1/aclk0]

  # Create instance: dummy_noc_m_2, and set properties
  set dummy_noc_m_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_m_2 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dummy_noc_m_2


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dummy_noc_m_2/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_m_2/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dummy_noc_m_2/aclk0]

  # Create instance: dummy_noc_m_3, and set properties
  set dummy_noc_m_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_m_3 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dummy_noc_m_3


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dummy_noc_m_3/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_m_3/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dummy_noc_m_3/aclk0]

  # Create instance: dummy_noc_m_4, and set properties
  set dummy_noc_m_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_m_4 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dummy_noc_m_4


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dummy_noc_m_4/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_m_4/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dummy_noc_m_4/aclk0]

  # Create instance: dummy_noc_m_5, and set properties
  set dummy_noc_m_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_m_5 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dummy_noc_m_5


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dummy_noc_m_5/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_m_5/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dummy_noc_m_5/aclk0]

  # Create instance: dummy_noc_m_6, and set properties
  set dummy_noc_m_6 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_m_6 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dummy_noc_m_6


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dummy_noc_m_6/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_m_6/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dummy_noc_m_6/aclk0]

  # Create instance: dummy_noc_m_7, and set properties
  set dummy_noc_m_7 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_noc:1.0 dummy_noc_m_7 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $dummy_noc_m_7


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /dummy_noc_m_7/M00_INIS]

  set_property -dict [ list \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.CONNECTIONS {M00_INIS { write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /dummy_noc_m_7/S00_AXIS]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXIS} \
 ] [get_bd_pins /dummy_noc_m_7/aclk0]

  # Create instance: eth_0, and set properties
  set eth_0 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 eth_0 ]

  # Create instance: eth_1, and set properties
  set eth_1 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 eth_1 ]

  # Create instance: eth_2, and set properties
  set eth_2 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 eth_2 ]

  # Create instance: eth_3, and set properties
  set eth_3 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 eth_3 ]

  # Create instance: eth_4, and set properties
  set eth_4 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 eth_4 ]

  # Create instance: eth_5, and set properties
  set eth_5 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 eth_5 ]

  # Create instance: eth_6, and set properties
  set eth_6 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 eth_6 ]

  # Create instance: eth_7, and set properties
  set eth_7 [ create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 eth_7 ]

  # Create instance: sl2noc_0, and set properties
  set sl2noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc sl2noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $sl2noc_0


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /sl2noc_0/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /sl2noc_0/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /sl2noc_0/aclk0]

  # Create instance: sl2noc_1, and set properties
  set sl2noc_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc sl2noc_1 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $sl2noc_1


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /sl2noc_1/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /sl2noc_1/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /sl2noc_1/aclk0]

  # Create instance: sl2noc_2, and set properties
  set sl2noc_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc sl2noc_2 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $sl2noc_2


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /sl2noc_2/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /sl2noc_2/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /sl2noc_2/aclk0]

  # Create instance: sl2noc_3, and set properties
  set sl2noc_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc sl2noc_3 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $sl2noc_3


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /sl2noc_3/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /sl2noc_3/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /sl2noc_3/aclk0]

  # Create instance: sl2noc_4, and set properties
  set sl2noc_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc sl2noc_4 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $sl2noc_4


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /sl2noc_4/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /sl2noc_4/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /sl2noc_4/aclk0]

  # Create instance: sl2noc_5, and set properties
  set sl2noc_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc sl2noc_5 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $sl2noc_5


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /sl2noc_5/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /sl2noc_5/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /sl2noc_5/aclk0]

  # Create instance: sl2noc_6, and set properties
  set sl2noc_6 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc sl2noc_6 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $sl2noc_6


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /sl2noc_6/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /sl2noc_6/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /sl2noc_6/aclk0]

  # Create instance: sl2noc_7, and set properties
  set sl2noc_7 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc sl2noc_7 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $sl2noc_7


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /sl2noc_7/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /sl2noc_7/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /sl2noc_7/aclk0]

  # Create instance: smartconnect_0, and set properties
  set smartconnect_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_0 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MI {14} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect_0


  # Create instance: traffic_producer_1, and set properties
  set traffic_producer_1 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_1 ]

  # Create instance: traffic_producer_2, and set properties
  set traffic_producer_2 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_2 ]

  # Create instance: traffic_producer_3, and set properties
  set traffic_producer_3 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_3 ]

  # Create instance: traffic_producer_5, and set properties
  set traffic_producer_5 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_5 ]

  # Create instance: traffic_producer_6, and set properties
  set traffic_producer_6 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_6 ]

  # Create instance: traffic_producer_7, and set properties
  set traffic_producer_7 [ create_bd_cell -type ip -vlnv xilinx.com:hls:traffic_producer:1.0 traffic_producer_7 ]

  # Create instance: xlconstant_0, and set properties
  set xlconstant_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_0 ]

  #### DCMAC entry points ####
  add_dcmac_inst

   # Create instance: smartconnect_1, and set properties
  set smartconnect_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_1 ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {1} \
    CONFIG.NUM_MI {3} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect_1


  # Create instance: axi_noc_0, and set properties
  set axi_noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc axi_noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_0


  set_property -dict [ list \
   CONFIG.APERTURES {{0x203_0000_0000 128M}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_0/M00_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXI {read_bw {5} write_bw {5} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /axi_noc_0/S00_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI} \
 ] [get_bd_pins /axi_noc_0/aclk0]

  # Create instance: noc_virt_0, and set properties
  set noc_virt_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc noc_virt_0 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
    CONFIG.NUM_SI {1} \
  ] $noc_virt_0


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /noc_virt_0/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /noc_virt_0/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /noc_virt_0/aclk0]

  # Create instance: noc_virt_1, and set properties
  set noc_virt_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc noc_virt_1 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
    CONFIG.NUM_SI {1} \
  ] $noc_virt_1


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /noc_virt_1/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /noc_virt_1/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /noc_virt_1/aclk0]

  # Create instance: noc_virt_2, and set properties
  set noc_virt_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc noc_virt_2 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
    CONFIG.NUM_SI {1} \
  ] $noc_virt_2


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /noc_virt_2/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /noc_virt_2/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /noc_virt_2/aclk0]

  # Create instance: noc_virt_3, and set properties
  set noc_virt_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc noc_virt_3 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
    CONFIG.NUM_SI {1} \
  ] $noc_virt_3


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /noc_virt_3/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /noc_virt_3/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /noc_virt_3/aclk0]

  # Create instance: noc_virt_4, and set properties
  set noc_virt_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc noc_virt_4 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
    CONFIG.NUM_SI {1} \
  ] $noc_virt_4


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /noc_virt_4/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /noc_virt_4/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /noc_virt_4/aclk0]

  # Create instance: axi_noc_1, and set properties
  set axi_noc_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc axi_noc_1 ]
  set_property -dict [list \
    CONFIG.MI_SIDEBAND_PINS {} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_1


  set_property -dict [ list \
   CONFIG.APERTURES {{0x208_0000_0000 32G}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_1/M00_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXI {read_bw {500} write_bw {500} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /axi_noc_1/S00_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI} \
 ] [get_bd_pins /axi_noc_1/aclk0]

  # Create instance: axi4_full_passthrough_0, and set properties
  set axi4_full_passthrough_0 [ create_bd_cell -type ip -vlnv user.org:user:axi4_full_passthrough:1.0 axi4_full_passthrough_0 ]
  set_property CONFIG.AXI_DATA_WIDTH {128} $axi4_full_passthrough_0


  # Create instance: axi_register_slice_0, and set properties
  set axi_register_slice_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 axi_register_slice_0 ]

  # Create instance: axi_register_slice_1, and set properties
  set axi_register_slice_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 axi_register_slice_1 ]

  # Create instance: axi_noc_2, and set properties
  set axi_noc_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc axi_noc_2 ]
  set_property -dict [list \
    CONFIG.MI_SIDEBAND_PINS {} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_2


  set_property -dict [ list \
   CONFIG.APERTURES {{0x208_0000_0000 32G}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_2/M00_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXI {read_bw {500} write_bw {500} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /axi_noc_2/S00_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI} \
 ] [get_bd_pins /axi_noc_2/aclk0]

  # Create instance: axi_noc_3, and set properties
  set axi_noc_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc axi_noc_3 ]
  set_property -dict [list \
    CONFIG.MI_SIDEBAND_PINS {} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_3


  set_property -dict [ list \
   CONFIG.APERTURES {{0x208_0000_0000 32G}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_3/M00_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXI {read_bw {500} write_bw {500} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /axi_noc_3/S00_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI} \
 ] [get_bd_pins /axi_noc_3/aclk0]

  # Create instance: axi_noc_4, and set properties
  set axi_noc_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc axi_noc_4 ]
  set_property -dict [list \
    CONFIG.MI_SIDEBAND_PINS {} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_4


  set_property -dict [ list \
   CONFIG.APERTURES {{0x208_0000_0000 32G}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_4/M00_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXI {read_bw {500} write_bw {500} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /axi_noc_4/S00_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI} \
 ] [get_bd_pins /axi_noc_4/aclk0]

  # Create instance: axi_noc_5, and set properties
  set axi_noc_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc axi_noc_5 ]
  set_property -dict [list \
    CONFIG.MI_SIDEBAND_PINS {} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_5


  set_property -dict [ list \
   CONFIG.APERTURES {{0x208_0000_0000 32G}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_5/M00_AXI]

  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXI {read_bw {500} write_bw {500} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /axi_noc_5/S00_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI} \
 ] [get_bd_pins /axi_noc_5/aclk0]

  # Create instance: axi4_full_passthrough_1, and set properties
  set axi4_full_passthrough_1 [ create_bd_cell -type ip -vlnv user.org:user:axi4_full_passthrough:1.0 axi4_full_passthrough_1 ]
  set_property CONFIG.AXI_DATA_WIDTH {128} $axi4_full_passthrough_1


  # Create instance: axi_register_slice_2, and set properties
  set axi_register_slice_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 axi_register_slice_2 ]

  # Create instance: axi_register_slice_3, and set properties
  set axi_register_slice_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 axi_register_slice_3 ]

  # Create instance: axi4_full_passthrough_2, and set properties
  set axi4_full_passthrough_2 [ create_bd_cell -type ip -vlnv user.org:user:axi4_full_passthrough:1.0 axi4_full_passthrough_2 ]
  set_property CONFIG.AXI_DATA_WIDTH {128} $axi4_full_passthrough_2


  # Create instance: axi_register_slice_4, and set properties
  set axi_register_slice_4 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 axi_register_slice_4 ]

  # Create instance: axi_register_slice_5, and set properties
  set axi_register_slice_5 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 axi_register_slice_5 ]

  # Create instance: axi4_full_passthrough_3, and set properties
  set axi4_full_passthrough_3 [ create_bd_cell -type ip -vlnv user.org:user:axi4_full_passthrough:1.0 axi4_full_passthrough_3 ]
  set_property CONFIG.AXI_DATA_WIDTH {128} $axi4_full_passthrough_3


  # Create instance: axi_register_slice_6, and set properties
  set axi_register_slice_6 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 axi_register_slice_6 ]

  # Create instance: axi_register_slice_7, and set properties
  set axi_register_slice_7 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 axi_register_slice_7 ]

  # Create instance: axi4_full_passthrough_4, and set properties
  set axi4_full_passthrough_4 [ create_bd_cell -type ip -vlnv user.org:user:axi4_full_passthrough:1.0 axi4_full_passthrough_4 ]
  set_property CONFIG.AXI_DATA_WIDTH {128} $axi4_full_passthrough_4


  # Create instance: axi_register_slice_8, and set properties
  set axi_register_slice_8 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 axi_register_slice_8 ]

  # Create instance: axi_register_slice_9, and set properties
  set axi_register_slice_9 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 axi_register_slice_9 ]

  # Create instance: ilreduced_logic_0, and set properties
  set ilreduced_logic_0 [ create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilreduced_logic:1.0 ilreduced_logic_0 ]
  set_property -dict [list \
    CONFIG.C_OPERATION {or} \
    CONFIG.C_SIZE {1} \
  ] $ilreduced_logic_0


  # Create instance: util_ds_buf_0, and set properties
  set util_ds_buf_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_ds_buf:2.2 util_ds_buf_0 ]
  set_property CONFIG.C_BUF_TYPE {BUFG_FABRIC} $util_ds_buf_0


  # Create instance: c_shift_ram_0, and set properties
  set c_shift_ram_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:c_shift_ram:12.0 c_shift_ram_0 ]
  set_property -dict [list \
    CONFIG.Depth {1} \
    CONFIG.Width {1} \
  ] $c_shift_ram_0

  # Create interface connections
  connect_bd_intf_net -intf_net Conn1 [get_bd_intf_pins dummy_noc_m_0/M00_INIS] [get_bd_intf_ports M_DCMAC_INIS0]
  connect_bd_intf_net -intf_net Conn2 [get_bd_intf_pins dummy_noc_m_1/M00_INIS] [get_bd_intf_ports M_DCMAC_INIS1]
  connect_bd_intf_net -intf_net Conn3 [get_bd_intf_pins dummy_noc_m_2/M00_INIS] [get_bd_intf_ports M_DCMAC_INIS2]
  connect_bd_intf_net -intf_net Conn4 [get_bd_intf_pins dummy_noc_m_3/M00_INIS] [get_bd_intf_ports M_DCMAC_INIS3]
  connect_bd_intf_net -intf_net Conn5 [get_bd_intf_pins dummy_noc_m_4/M00_INIS] [get_bd_intf_ports M_DCMAC_INIS4]
  connect_bd_intf_net -intf_net Conn6 [get_bd_intf_pins dummy_noc_m_5/M00_INIS] [get_bd_intf_ports M_DCMAC_INIS5]
  connect_bd_intf_net -intf_net Conn7 [get_bd_intf_pins dummy_noc_m_6/M00_INIS] [get_bd_intf_ports M_DCMAC_INIS6]
  connect_bd_intf_net -intf_net Conn8 [get_bd_intf_pins dummy_noc_m_7/M00_INIS] [get_bd_intf_ports M_DCMAC_INIS7]
  connect_bd_intf_net -intf_net Conn9 [get_bd_intf_pins dummy_noc_0/S00_INIS] [get_bd_intf_ports S_DCMAC_INIS0]
  connect_bd_intf_net -intf_net Conn10 [get_bd_intf_pins dummy_noc_1/S00_INIS] [get_bd_intf_ports S_DCMAC_INIS1]
  connect_bd_intf_net -intf_net Conn11 [get_bd_intf_pins dummy_noc_2/S00_INIS] [get_bd_intf_ports S_DCMAC_INIS2]
  connect_bd_intf_net -intf_net Conn12 [get_bd_intf_pins dummy_noc_3/S00_INIS] [get_bd_intf_ports S_DCMAC_INIS3]
  connect_bd_intf_net -intf_net Conn13 [get_bd_intf_pins dummy_noc_4/S00_INIS] [get_bd_intf_ports S_DCMAC_INIS4]
  connect_bd_intf_net -intf_net Conn14 [get_bd_intf_pins dummy_noc_5/S00_INIS] [get_bd_intf_ports S_DCMAC_INIS5]
  connect_bd_intf_net -intf_net Conn15 [get_bd_intf_pins dummy_noc_6/S00_INIS] [get_bd_intf_ports S_DCMAC_INIS6]
  connect_bd_intf_net -intf_net Conn16 [get_bd_intf_pins dummy_noc_7/S00_INIS] [get_bd_intf_ports S_DCMAC_INIS7]
  connect_bd_intf_net -intf_net S_AXILITE_INI_1 [get_bd_intf_ports S_AXILITE_INI] [get_bd_intf_pins axi_noc_0/S00_INI]
  connect_bd_intf_net -intf_net S_AXIS_0_1 [get_bd_intf_pins qsfp_0_n_1/S_AXIS_0] [get_bd_intf_pins dummy_noc_0/M00_AXIS]
  connect_bd_intf_net -intf_net S_QDMA_SLV_BRIDGE_1 [get_bd_intf_ports S_QDMA_SLV_BRIDGE] [get_bd_intf_pins axi_noc_5/S00_INI]
  connect_bd_intf_net -intf_net S_VIRT_00_1 [get_bd_intf_ports S_VIRT_00] [get_bd_intf_pins axi_noc_1/S00_INI]
  connect_bd_intf_net -intf_net S_VIRT_01_1 [get_bd_intf_ports S_VIRT_01] [get_bd_intf_pins axi_noc_2/S00_INI]
  connect_bd_intf_net -intf_net S_VIRT_02_1 [get_bd_intf_ports S_VIRT_02] [get_bd_intf_pins axi_noc_3/S00_INI]
  connect_bd_intf_net -intf_net S_VIRT_03_1 [get_bd_intf_ports S_VIRT_03] [get_bd_intf_pins axi_noc_4/S00_INI]
  connect_bd_intf_net -intf_net axi4_full_passthrough_0_m_axi [get_bd_intf_pins axi_register_slice_1/S_AXI] [get_bd_intf_pins axi4_full_passthrough_0/m_axi]
  connect_bd_intf_net -intf_net axi4_full_passthrough_1_m_axi [get_bd_intf_pins axi4_full_passthrough_1/m_axi] [get_bd_intf_pins axi_register_slice_3/S_AXI]
  connect_bd_intf_net -intf_net axi4_full_passthrough_1_m_axi1 [get_bd_intf_pins axi4_full_passthrough_2/m_axi] [get_bd_intf_pins axi_register_slice_5/S_AXI]
  connect_bd_intf_net -intf_net axi4_full_passthrough_1_m_axi2 [get_bd_intf_pins axi4_full_passthrough_3/m_axi] [get_bd_intf_pins axi_register_slice_7/S_AXI]
  connect_bd_intf_net -intf_net axi4_full_passthrough_1_m_axi3 [get_bd_intf_pins axi4_full_passthrough_4/m_axi] [get_bd_intf_pins axi_register_slice_9/S_AXI]
  connect_bd_intf_net -intf_net axi_noc_0_M00_AXI [get_bd_intf_pins axi_noc_0/M00_AXI] [get_bd_intf_pins smartconnect_0/S00_AXI]
  connect_bd_intf_net -intf_net axi_noc_1_M00_AXI [get_bd_intf_pins axi_register_slice_0/S_AXI] [get_bd_intf_pins axi_noc_1/M00_AXI]
  connect_bd_intf_net -intf_net axi_noc_1_M00_INI [get_bd_intf_ports M_VIRT_0] [get_bd_intf_pins noc_virt_0/M00_INI]
  connect_bd_intf_net -intf_net axi_noc_2_M00_AXI [get_bd_intf_pins axi_register_slice_2/S_AXI] [get_bd_intf_pins axi_noc_2/M00_AXI]
  connect_bd_intf_net -intf_net axi_noc_2_M00_INI [get_bd_intf_ports M_VIRT_1] [get_bd_intf_pins noc_virt_1/M00_INI]
  connect_bd_intf_net -intf_net axi_noc_3_M00_AXI [get_bd_intf_pins axi_register_slice_4/S_AXI] [get_bd_intf_pins axi_noc_3/M00_AXI]
  connect_bd_intf_net -intf_net axi_noc_3_M00_INI [get_bd_intf_ports M_VIRT_2] [get_bd_intf_pins noc_virt_2/M00_INI]
  connect_bd_intf_net -intf_net axi_noc_4_M00_AXI [get_bd_intf_pins axi_register_slice_6/S_AXI] [get_bd_intf_pins axi_noc_4/M00_AXI]
  connect_bd_intf_net -intf_net axi_noc_4_M00_INI [get_bd_intf_ports M_VIRT_3] [get_bd_intf_pins noc_virt_3/M00_INI]
  connect_bd_intf_net -intf_net axi_noc_5_M00_AXI [get_bd_intf_pins axi_register_slice_8/S_AXI] [get_bd_intf_pins axi_noc_5/M00_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_0_M_AXI [get_bd_intf_pins axi_register_slice_0/M_AXI] [get_bd_intf_pins axi4_full_passthrough_0/s_axi]
  connect_bd_intf_net -intf_net axi_register_slice_1_M_AXI [get_bd_intf_pins axi_register_slice_1/M_AXI] [get_bd_intf_pins noc_virt_0/S00_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_2_M_AXI [get_bd_intf_pins axi_register_slice_2/M_AXI] [get_bd_intf_pins axi4_full_passthrough_1/s_axi]
  connect_bd_intf_net -intf_net axi_register_slice_2_M_AXI1 [get_bd_intf_pins axi_register_slice_4/M_AXI] [get_bd_intf_pins axi4_full_passthrough_2/s_axi]
  connect_bd_intf_net -intf_net axi_register_slice_2_M_AXI2 [get_bd_intf_pins axi_register_slice_6/M_AXI] [get_bd_intf_pins axi4_full_passthrough_3/s_axi]
  connect_bd_intf_net -intf_net axi_register_slice_2_M_AXI3 [get_bd_intf_pins axi_register_slice_8/M_AXI] [get_bd_intf_pins axi4_full_passthrough_4/s_axi]
  connect_bd_intf_net -intf_net axi_register_slice_3_M_AXI [get_bd_intf_pins noc_virt_1/S00_AXI] [get_bd_intf_pins axi_register_slice_3/M_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_5_M_AXI [get_bd_intf_pins axi_register_slice_5/M_AXI] [get_bd_intf_pins noc_virt_2/S00_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_7_M_AXI [get_bd_intf_pins axi_register_slice_7/M_AXI] [get_bd_intf_pins noc_virt_3/S00_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_9_M_AXI [get_bd_intf_pins axi_register_slice_9/M_AXI] [get_bd_intf_pins noc_virt_4/S00_AXI]
  connect_bd_intf_net -intf_net dummy_noc_4_M00_AXIS [get_bd_intf_pins dummy_noc_4/M00_AXIS] [get_bd_intf_pins qsfp_2_n_3/S_AXIS_0]
  connect_bd_intf_net -intf_net eth_0_m_axi_gmem0 [get_bd_intf_pins eth_0/m_axi_gmem0] [get_bd_intf_pins sl2noc_0/S00_AXI]
  connect_bd_intf_net -intf_net eth_1_m_axi_gmem0 [get_bd_intf_pins eth_1/m_axi_gmem0] [get_bd_intf_pins sl2noc_1/S00_AXI]
  connect_bd_intf_net -intf_net eth_2_m_axi_gmem0 [get_bd_intf_pins eth_2/m_axi_gmem0] [get_bd_intf_pins sl2noc_2/S00_AXI]
  connect_bd_intf_net -intf_net eth_3_m_axi_gmem0 [get_bd_intf_pins eth_3/m_axi_gmem0] [get_bd_intf_pins sl2noc_3/S00_AXI]
  connect_bd_intf_net -intf_net eth_4_m_axi_gmem0 [get_bd_intf_pins eth_4/m_axi_gmem0] [get_bd_intf_pins sl2noc_4/S00_AXI]
  connect_bd_intf_net -intf_net eth_5_m_axi_gmem0 [get_bd_intf_pins eth_5/m_axi_gmem0] [get_bd_intf_pins sl2noc_5/S00_AXI]
  connect_bd_intf_net -intf_net eth_6_m_axi_gmem0 [get_bd_intf_pins eth_6/m_axi_gmem0] [get_bd_intf_pins sl2noc_6/S00_AXI]
  connect_bd_intf_net -intf_net eth_7_m_axi_gmem0 [get_bd_intf_pins eth_7/m_axi_gmem0] [get_bd_intf_pins sl2noc_7/S00_AXI]
  connect_bd_intf_net -intf_net noc_virt_5_M00_INI [get_bd_intf_ports M_QDMA_SLV_BRIDGE] [get_bd_intf_pins noc_virt_4/M00_INI]
  connect_bd_intf_net -intf_net qsfp0_322mhz_1 [get_bd_intf_ports qsfp0_322mhz] [get_bd_intf_pins qsfp_0_n_1/qsfp_clk_322mhz]
  connect_bd_intf_net -intf_net qsfp2_322mhz_1 [get_bd_intf_ports qsfp2_322mhz] [get_bd_intf_pins qsfp_2_n_3/qsfp_clk_322mhz]
  connect_bd_intf_net -intf_net qsfp_0_n_1_M_AXIS_0 [get_bd_intf_pins dummy_noc_m_0/S00_AXIS] [get_bd_intf_pins qsfp_0_n_1/M_AXIS_0]
  connect_bd_intf_net -intf_net qsfp_0_n_1_qsfp_gt0 [get_bd_intf_ports qsfp0_4x] [get_bd_intf_pins qsfp_0_n_1/qsfp_gt0]
  connect_bd_intf_net -intf_net qsfp_2_n_3_M_AXIS_0 [get_bd_intf_pins dummy_noc_m_4/S00_AXIS] [get_bd_intf_pins qsfp_2_n_3/M_AXIS_0]
  connect_bd_intf_net -intf_net qsfp_2_n_3_qsfp_gt0 [get_bd_intf_ports qsfp2_4x] [get_bd_intf_pins qsfp_2_n_3/qsfp_gt0]
  connect_bd_intf_net -intf_net sl2noc_0_M00_INI [get_bd_intf_ports SL2NOC_0] [get_bd_intf_pins sl2noc_0/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_1_M00_INI [get_bd_intf_ports SL2NOC_1] [get_bd_intf_pins sl2noc_1/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_2_M00_INI [get_bd_intf_ports SL2NOC_2] [get_bd_intf_pins sl2noc_2/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_3_M00_INI [get_bd_intf_ports SL2NOC_3] [get_bd_intf_pins sl2noc_3/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_4_M00_INI [get_bd_intf_ports SL2NOC_4] [get_bd_intf_pins sl2noc_4/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_5_M00_INI [get_bd_intf_ports SL2NOC_5] [get_bd_intf_pins sl2noc_5/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_6_M00_INI [get_bd_intf_ports SL2NOC_6] [get_bd_intf_pins sl2noc_6/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_7_M00_INI [get_bd_intf_ports SL2NOC_7] [get_bd_intf_pins sl2noc_7/M00_INI]
  connect_bd_intf_net -intf_net smartconnect_0_M00_AXI [get_bd_intf_pins eth_5/s_axi_control] [get_bd_intf_pins smartconnect_0/M00_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M01_AXI [get_bd_intf_pins smartconnect_0/M01_AXI] [get_bd_intf_pins traffic_producer_1/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_0_M02_AXI [get_bd_intf_pins smartconnect_0/M02_AXI] [get_bd_intf_pins traffic_producer_2/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_0_M03_AXI [get_bd_intf_pins smartconnect_0/M03_AXI] [get_bd_intf_pins traffic_producer_3/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_0_M04_AXI [get_bd_intf_pins eth_6/s_axi_control] [get_bd_intf_pins smartconnect_0/M04_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M05_AXI [get_bd_intf_pins smartconnect_0/M05_AXI] [get_bd_intf_pins traffic_producer_5/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_0_M06_AXI [get_bd_intf_pins smartconnect_0/M06_AXI] [get_bd_intf_pins traffic_producer_6/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_0_M07_AXI [get_bd_intf_pins smartconnect_0/M07_AXI] [get_bd_intf_pins traffic_producer_7/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_0_M08_AXI [get_bd_intf_pins eth_0/s_axi_control] [get_bd_intf_pins smartconnect_0/M08_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M09_AXI [get_bd_intf_pins eth_1/s_axi_control] [get_bd_intf_pins smartconnect_0/M09_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M10_AXI [get_bd_intf_pins eth_2/s_axi_control] [get_bd_intf_pins smartconnect_0/M10_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M11_AXI [get_bd_intf_pins eth_3/s_axi_control] [get_bd_intf_pins smartconnect_0/M11_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M12_AXI [get_bd_intf_pins eth_4/s_axi_control] [get_bd_intf_pins smartconnect_0/M12_AXI]
  connect_bd_intf_net -intf_net smartconnect_0_M13_AXI [get_bd_intf_pins smartconnect_1/S00_AXI] [get_bd_intf_pins smartconnect_0/M13_AXI]
  connect_bd_intf_net -intf_net smartconnect_1_M00_AXI [get_bd_intf_pins smartconnect_1/M00_AXI] [get_bd_intf_pins eth_7/s_axi_control]
  connect_bd_intf_net -intf_net smartconnect_1_M01_AXI [get_bd_intf_pins smartconnect_1/M01_AXI] [get_bd_intf_pins qsfp_0_n_1/s_axi]
  connect_bd_intf_net -intf_net smartconnect_1_M02_AXI [get_bd_intf_pins smartconnect_1/M02_AXI] [get_bd_intf_pins qsfp_2_n_3/s_axi]
  connect_bd_intf_net -intf_net traffic_producer_1_axis_out [get_bd_intf_pins traffic_producer_1/axis_out] [get_bd_intf_pins dummy_noc_m_1/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_2_axis_out [get_bd_intf_pins traffic_producer_2/axis_out] [get_bd_intf_pins dummy_noc_m_2/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_3_axis_out [get_bd_intf_pins traffic_producer_3/axis_out] [get_bd_intf_pins dummy_noc_m_3/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_5_axis_out [get_bd_intf_pins traffic_producer_5/axis_out] [get_bd_intf_pins dummy_noc_m_5/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_6_axis_out [get_bd_intf_pins traffic_producer_6/axis_out] [get_bd_intf_pins dummy_noc_m_6/S00_AXIS]
  connect_bd_intf_net -intf_net traffic_producer_7_axis_out [get_bd_intf_pins traffic_producer_7/axis_out] [get_bd_intf_pins dummy_noc_m_7/S00_AXIS]

  # Create port connections
  connect_bd_net -net arstn_1  [get_bd_ports arstn] \
  [get_bd_pins c_shift_ram_0/D]
  connect_bd_net -net c_shift_ram_0_Q  [get_bd_pins c_shift_ram_0/Q] \
  [get_bd_pins util_ds_buf_0/BUFG_FABRIC_I]

  connect_bd_net -net clk_wizard_0_clk_out1  [get_bd_ports service_clk] \
  [get_bd_pins dummy_noc_0/aclk0] \
  [get_bd_pins dummy_noc_1/aclk0] \
  [get_bd_pins dummy_noc_2/aclk0] \
  [get_bd_pins dummy_noc_3/aclk0] \
  [get_bd_pins dummy_noc_4/aclk0] \
  [get_bd_pins dummy_noc_5/aclk0] \
  [get_bd_pins dummy_noc_6/aclk0] \
  [get_bd_pins dummy_noc_7/aclk0] \
  [get_bd_pins dummy_noc_m_0/aclk0] \
  [get_bd_pins dummy_noc_m_1/aclk0] \
  [get_bd_pins dummy_noc_m_2/aclk0] \
  [get_bd_pins dummy_noc_m_3/aclk0] \
  [get_bd_pins dummy_noc_m_4/aclk0] \
  [get_bd_pins dummy_noc_m_5/aclk0] \
  [get_bd_pins dummy_noc_m_6/aclk0] \
  [get_bd_pins dummy_noc_m_7/aclk0] \
  [get_bd_pins traffic_producer_1/ap_clk] \
  [get_bd_pins traffic_producer_2/ap_clk] \
  [get_bd_pins traffic_producer_3/ap_clk] \
  [get_bd_pins traffic_producer_5/ap_clk] \
  [get_bd_pins traffic_producer_6/ap_clk] \
  [get_bd_pins traffic_producer_7/ap_clk] \
  [get_bd_pins smartconnect_0/aclk] \
  [get_bd_pins eth_0/ap_clk] \
  [get_bd_pins eth_1/ap_clk] \
  [get_bd_pins eth_2/ap_clk] \
  [get_bd_pins eth_3/ap_clk] \
  [get_bd_pins eth_4/ap_clk] \
  [get_bd_pins eth_5/ap_clk] \
  [get_bd_pins eth_6/ap_clk] \
  [get_bd_pins eth_7/ap_clk] \
  [get_bd_pins sl2noc_0/aclk0] \
  [get_bd_pins sl2noc_1/aclk0] \
  [get_bd_pins sl2noc_2/aclk0] \
  [get_bd_pins sl2noc_3/aclk0] \
  [get_bd_pins sl2noc_4/aclk0] \
  [get_bd_pins sl2noc_5/aclk0] \
  [get_bd_pins sl2noc_6/aclk0] \
  [get_bd_pins sl2noc_7/aclk0] \
  [get_bd_pins qsfp_0_n_1/ap_clk] \
  [get_bd_pins qsfp_2_n_3/ap_clk] \
  [get_bd_pins smartconnect_1/aclk] \
  [get_bd_pins axi_noc_0/aclk0] \
  [get_bd_pins noc_virt_0/aclk0] \
  [get_bd_pins axi_noc_1/aclk0] \
  [get_bd_pins axi4_full_passthrough_0/aclk] \
  [get_bd_pins axi_register_slice_0/aclk] \
  [get_bd_pins axi_register_slice_1/aclk] \
  [get_bd_pins axi_noc_5/aclk0] \
  [get_bd_pins axi_noc_4/aclk0] \
  [get_bd_pins axi_noc_3/aclk0] \
  [get_bd_pins axi_noc_2/aclk0] \
  [get_bd_pins axi_register_slice_3/aclk] \
  [get_bd_pins axi4_full_passthrough_1/aclk] \
  [get_bd_pins axi_register_slice_2/aclk] \
  [get_bd_pins noc_virt_1/aclk0] \
  [get_bd_pins axi4_full_passthrough_2/aclk] \
  [get_bd_pins axi_register_slice_5/aclk] \
  [get_bd_pins axi_register_slice_4/aclk] \
  [get_bd_pins axi4_full_passthrough_3/aclk] \
  [get_bd_pins axi_register_slice_7/aclk] \
  [get_bd_pins axi_register_slice_6/aclk] \
  [get_bd_pins axi4_full_passthrough_4/aclk] \
  [get_bd_pins axi_register_slice_9/aclk] \
  [get_bd_pins axi_register_slice_8/aclk] \
  [get_bd_pins noc_virt_3/aclk0] \
  [get_bd_pins noc_virt_2/aclk0] \
  [get_bd_pins noc_virt_4/aclk0] \
  [get_bd_pins c_shift_ram_0/CLK]
  connect_bd_net -net proc_sys_reset_0_peripheral_aresetn  [get_bd_pins ilreduced_logic_0/Res] \
  [get_bd_pins traffic_producer_1/ap_rst_n] \
  [get_bd_pins traffic_producer_2/ap_rst_n] \
  [get_bd_pins traffic_producer_3/ap_rst_n] \
  [get_bd_pins traffic_producer_5/ap_rst_n] \
  [get_bd_pins traffic_producer_6/ap_rst_n] \
  [get_bd_pins traffic_producer_7/ap_rst_n] \
  [get_bd_pins eth_0/ap_rst_n] \
  [get_bd_pins eth_1/ap_rst_n] \
  [get_bd_pins eth_2/ap_rst_n] \
  [get_bd_pins eth_3/ap_rst_n] \
  [get_bd_pins eth_4/ap_rst_n] \
  [get_bd_pins eth_5/ap_rst_n] \
  [get_bd_pins eth_6/ap_rst_n] \
  [get_bd_pins eth_7/ap_rst_n] \
  [get_bd_pins qsfp_0_n_1/ap_rst_n] \
  [get_bd_pins qsfp_2_n_3/ap_rst_n] \
  [get_bd_pins axi4_full_passthrough_0/aresetn] \
  [get_bd_pins axi_register_slice_0/aresetn] \
  [get_bd_pins axi_register_slice_1/aresetn] \
  [get_bd_pins axi_register_slice_3/aresetn] \
  [get_bd_pins axi4_full_passthrough_1/aresetn] \
  [get_bd_pins axi_register_slice_2/aresetn] \
  [get_bd_pins axi_register_slice_5/aresetn] \
  [get_bd_pins axi4_full_passthrough_2/aresetn] \
  [get_bd_pins axi_register_slice_4/aresetn] \
  [get_bd_pins axi_register_slice_7/aresetn] \
  [get_bd_pins axi4_full_passthrough_3/aresetn] \
  [get_bd_pins axi_register_slice_6/aresetn] \
  [get_bd_pins axi_register_slice_9/aresetn] \
  [get_bd_pins axi4_full_passthrough_4/aresetn] \
  [get_bd_pins axi_register_slice_8/aresetn] \
  [get_bd_pins smartconnect_1/aresetn] \
  [get_bd_pins smartconnect_0/aresetn]

  connect_bd_net -net util_ds_buf_0_BUFG_FABRIC_O  [get_bd_pins util_ds_buf_0/BUFG_FABRIC_O] \
  [get_bd_pins ilreduced_logic_0/Op1]

  connect_bd_net -net xlconstant_0_dout  [get_bd_pins xlconstant_0/dout] \
  [get_bd_pins dummy_noc_1/M00_AXIS_tready] \
  [get_bd_pins dummy_noc_2/M00_AXIS_tready] \
  [get_bd_pins dummy_noc_3/M00_AXIS_tready] \
  [get_bd_pins dummy_noc_5/M00_AXIS_tready] \
  [get_bd_pins dummy_noc_6/M00_AXIS_tready] \
  [get_bd_pins dummy_noc_7/M00_AXIS_tready]

  # Create address segments
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces eth_0/Data_m_axi_gmem0] [get_bd_addr_segs SL2NOC_0/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces eth_1/Data_m_axi_gmem0] [get_bd_addr_segs SL2NOC_1/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces eth_2/Data_m_axi_gmem0] [get_bd_addr_segs SL2NOC_2/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces eth_3/Data_m_axi_gmem0] [get_bd_addr_segs SL2NOC_3/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces eth_4/Data_m_axi_gmem0] [get_bd_addr_segs SL2NOC_4/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces eth_5/Data_m_axi_gmem0] [get_bd_addr_segs SL2NOC_5/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces eth_6/Data_m_axi_gmem0] [get_bd_addr_segs SL2NOC_6/Reg] -force
  assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces eth_7/Data_m_axi_gmem0] [get_bd_addr_segs SL2NOC_7/Reg] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces axi4_full_passthrough_0/m_axi] [get_bd_addr_segs M_VIRT_0/Reg] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces axi4_full_passthrough_1/m_axi] [get_bd_addr_segs M_VIRT_1/Reg] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces axi4_full_passthrough_2/m_axi] [get_bd_addr_segs M_VIRT_2/Reg] -force
  assign_bd_address -offset 0x060000000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces axi4_full_passthrough_3/m_axi] [get_bd_addr_segs M_VIRT_3/Reg] -force
  assign_bd_address -offset 0xE0000000 -range 0x10000000 -target_address_space [get_bd_addr_spaces axi4_full_passthrough_4/m_axi] [get_bd_addr_segs M_QDMA_SLV_BRIDGE/Reg] -force
  # assign_bd_address -offset 0x020302040400 -range 0x00000100 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_0_n_1/control_intf/axi_gpio_datapath/S_AXI/Reg] -force
  # assign_bd_address -offset 0x020303040400 -range 0x00000100 -with_name SEG_axi_gpio_datapath_Reg_1 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_2_n_3/control_intf/axi_gpio_datapath/S_AXI/Reg] -force
  # assign_bd_address -offset 0x020302040000 -range 0x00000100 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_0_n_1/control_intf/axi_gpio_gt_control/S_AXI/Reg] -force
  # assign_bd_address -offset 0x020303040000 -range 0x00000100 -with_name SEG_axi_gpio_gt_control_Reg_1 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_2_n_3/control_intf/axi_gpio_gt_control/S_AXI/Reg] -force
  # assign_bd_address -offset 0x020302040200 -range 0x00000100 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_0_n_1/control_intf/axi_gpio_monitor/S_AXI/Reg] -force
  # assign_bd_address -offset 0x020303040200 -range 0x00000100 -with_name SEG_axi_gpio_monitor_Reg_1 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_2_n_3/control_intf/axi_gpio_monitor/S_AXI/Reg] -force
  # assign_bd_address -offset 0x020302040600 -range 0x00000100 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_0_n_1/control_intf/axi_gpio_reset_txrx/S_AXI/Reg] -force
  # assign_bd_address -offset 0x020303040600 -range 0x00000100 -with_name SEG_axi_gpio_reset_txrx_Reg_1 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_2_n_3/control_intf/axi_gpio_reset_txrx/S_AXI/Reg] -force
  # assign_bd_address -offset 0x020302000000 -range 0x00040000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_0_n_1/DCMAC_subsys/dcmac_0_core/s_axi/Reg] -force
  # assign_bd_address -offset 0x020303000000 -range 0x00040000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_2_n_3/DCMAC_subsys/dcmac_1_core/s_axi/Reg] -force
  assign_bd_address -offset 0x020300000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs eth_0/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300010000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs eth_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300020000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs eth_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300030000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs eth_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300040000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs eth_4/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300050000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs eth_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300060000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs eth_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300070000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs eth_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020300090000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_1/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000A0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_2/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000B0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_3/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000D0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_5/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000E0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_6/s_axi_control/Reg] -force
  assign_bd_address -offset 0x0203000F0000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs traffic_producer_7/s_axi_control/Reg] -force
  assign_bd_address -offset 0x020800000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces S_QDMA_SLV_BRIDGE] [get_bd_addr_segs axi4_full_passthrough_4/s_axi/reg0] -force
  assign_bd_address -offset 0x020800000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces S_VIRT_00] [get_bd_addr_segs axi4_full_passthrough_0/s_axi/reg0] -force
  assign_bd_address -offset 0x020800000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces S_VIRT_01] [get_bd_addr_segs axi4_full_passthrough_1/s_axi/reg0] -force
  assign_bd_address -offset 0x020800000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces S_VIRT_02] [get_bd_addr_segs axi4_full_passthrough_2/s_axi/reg0] -force
  assign_bd_address -offset 0x020800000000 -range 0x000800000000 -target_address_space [get_bd_addr_spaces S_VIRT_03] [get_bd_addr_segs axi4_full_passthrough_3/s_axi/reg0] -force

  set_property USAGE memory [get_bd_addr_segs M_VIRT_0/Reg]
  set_property USAGE memory [get_bd_addr_segs M_VIRT_1/Reg]
  set_property USAGE memory [get_bd_addr_segs M_VIRT_2/Reg]
  set_property USAGE memory [get_bd_addr_segs M_VIRT_3/Reg]
  set_property USAGE memory [get_bd_addr_segs SL2NOC_0/Reg]
  set_property USAGE memory [get_bd_addr_segs SL2NOC_1/Reg]
  set_property USAGE memory [get_bd_addr_segs SL2NOC_2/Reg]
  set_property USAGE memory [get_bd_addr_segs SL2NOC_3/Reg]
  set_property USAGE memory [get_bd_addr_segs SL2NOC_4/Reg]
  set_property USAGE memory [get_bd_addr_segs SL2NOC_5/Reg]
  set_property USAGE memory [get_bd_addr_segs SL2NOC_6/Reg]
  set_property USAGE memory [get_bd_addr_segs SL2NOC_7/Reg]


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


