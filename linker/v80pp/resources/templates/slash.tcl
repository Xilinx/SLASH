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

delete_bd_objs [get_bd_cells ]
delete_bd_objs [get_bd_intf_nets]
delete_bd_objs [get_bd_nets]
update_compile_order -fileset sources_1
  {% raw %}
  set_property APERTURES {{0x40_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_00]
  set_property APERTURES {{0x40_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_01]
  set_property APERTURES {{0x40_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_02]
  set_property APERTURES {{0x40_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_03]
  set_property APERTURES {{0x40_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_04]
  set_property APERTURES {{0x40_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_05]
  set_property APERTURES {{0x40_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_06]
  set_property APERTURES {{0x40_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_07]
  set_property APERTURES {{0x41_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_08]
  set_property APERTURES {{0x41_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_09]
  set_property APERTURES {{0x41_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_10]
  set_property APERTURES {{0x41_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_11]
  set_property APERTURES {{0x41_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_12]
  set_property APERTURES {{0x41_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_13]
  set_property APERTURES {{0x41_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_14]
  set_property APERTURES {{0x41_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_15]
  set_property APERTURES {{0x42_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_16]
  set_property APERTURES {{0x42_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_17]
  set_property APERTURES {{0x42_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_18]
  set_property APERTURES {{0x42_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_19]
  set_property APERTURES {{0x42_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_20]
  set_property APERTURES {{0x42_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_21]
  set_property APERTURES {{0x42_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_22]
  set_property APERTURES {{0x42_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_23]
  set_property APERTURES {{0x43_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_24]
  set_property APERTURES {{0x43_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_25]
  set_property APERTURES {{0x43_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_26]
  set_property APERTURES {{0x43_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_27]
  set_property APERTURES {{0x43_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_28]
  set_property APERTURES {{0x43_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_29]
  set_property APERTURES {{0x43_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_30]
  set_property APERTURES {{0x43_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_31]
  set_property APERTURES {{0x44_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_32]
  set_property APERTURES {{0x44_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_33]
  set_property APERTURES {{0x44_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_34]
  set_property APERTURES {{0x44_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_35]
  set_property APERTURES {{0x44_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_36]
  set_property APERTURES {{0x44_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_37]
  set_property APERTURES {{0x44_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_38]
  set_property APERTURES {{0x44_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_39]
  set_property APERTURES {{0x45_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_40]
  set_property APERTURES {{0x45_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_41]
  set_property APERTURES {{0x45_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_42]
  set_property APERTURES {{0x45_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_43]
  set_property APERTURES {{0x45_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_44]
  set_property APERTURES {{0x45_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_45]
  set_property APERTURES {{0x45_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_46]
  set_property APERTURES {{0x45_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_47]
  set_property APERTURES {{0x46_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_48]
  set_property APERTURES {{0x46_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_49]
  set_property APERTURES {{0x46_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_50]
  set_property APERTURES {{0x46_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_51]
  set_property APERTURES {{0x46_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_52]
  set_property APERTURES {{0x46_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_53]
  set_property APERTURES {{0x46_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_54]
  set_property APERTURES {{0x46_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_55]
  set_property APERTURES {{0x47_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_56]
  set_property APERTURES {{0x47_0000_0000 1G}} [get_bd_intf_ports HBM_AXI_57]
  set_property APERTURES {{0x47_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_58]
  set_property APERTURES {{0x47_4000_0000 1G}} [get_bd_intf_ports HBM_AXI_59]
  set_property APERTURES {{0x47_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_60]
  set_property APERTURES {{0x47_8000_0000 1G}} [get_bd_intf_ports HBM_AXI_61]
  set_property APERTURES {{0x47_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_62]
  set_property APERTURES {{0x47_C000_0000 1G}} [get_bd_intf_ports HBM_AXI_63]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports HBM_VNOC_INI_00]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports HBM_VNOC_INI_01]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports HBM_VNOC_INI_02]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports HBM_VNOC_INI_03]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports HBM_VNOC_INI_04]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports HBM_VNOC_INI_05]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports HBM_VNOC_INI_06]
  set_property APERTURES {{0x0 2G} {0x40_0000_0000 1G} {0x40_4000_0000 1G} {0x40_8000_0000 1G} {0x40_C000_0000 1G} {0x41_0000_0000 1G} {0x41_4000_0000 1G} {0x41_8000_0000 1G} {0x41_C000_0000 1G} {0x42_0000_0000 1G} {0x42_4000_0000 1G} {0x42_8000_0000 1G} {0x42_C000_0000 1G} {0x43_0000_0000 1G} {0x43_4000_0000 1G} {0x43_8000_0000 1G} {0x43_C000_0000 1G} {0x44_0000_0000 1G} {0x44_4000_0000 1G} {0x44_8000_0000 1G} {0x44_C000_0000 1G} {0x45_0000_0000 1G} {0x45_4000_0000 1G} {0x45_8000_0000 1G} {0x45_C000_0000 1G} {0x46_0000_0000 1G} {0x46_4000_0000 1G} {0x46_8000_0000 1G} {0x46_C000_0000 1G} {0x47_0000_0000 1G} {0x47_4000_0000 1G} {0x47_8000_0000 1G} {0x47_C000_0000 1G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports HBM_VNOC_INI_07]
  set_property APERTURES {{0x0 2G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports M00_INI]
  set_property APERTURES {{0x0 2G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports M01_INI]
  set_property APERTURES {{0x0 2G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports M02_INI]
  set_property APERTURES {{0x0 2G} {0x500_8000_0000 2G} {0x600_0000_0000 32G}} [get_bd_intf_ports M03_INI]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_ports QDMA_SLAVE_BRIDGE_0]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_ports SL_VIRT_00]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_ports SL_VIRT_01]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_ports SL_VIRT_02]
  set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_ports SL_VIRT_03]
  set_property APERTURES {{0x202_0000_0000 128M}} [get_bd_intf_ports S_AXILITE_INI]
  {% endraw %}
 # Create instance: ddr_noc_0, and set properties
  set ddr_noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc ddr_noc_0 ]
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
  set ddr_noc_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc ddr_noc_1 ]
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
  set ddr_noc_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc ddr_noc_2 ]
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
  set ddr_noc_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc ddr_noc_3 ]
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

  # Create instance: hbm_vnoc_00, and set properties
  set hbm_vnoc_00 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc hbm_vnoc_00 ]
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
  set hbm_vnoc_01 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc hbm_vnoc_01 ]
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
  set hbm_vnoc_02 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc hbm_vnoc_02 ]
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
  set hbm_vnoc_03 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc hbm_vnoc_03 ]
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
  set hbm_vnoc_04 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc hbm_vnoc_04 ]
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
  set hbm_vnoc_05 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc hbm_vnoc_05 ]
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
  set hbm_vnoc_06 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc hbm_vnoc_06 ]
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
  set hbm_vnoc_07 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc hbm_vnoc_07 ]
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

  # Create instance: xlconstant_0, and set properties
  set xlconstant_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_0 ]

  # Create instance: axi_noc_0, and set properties
  set axi_noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc axi_noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_0

{% raw %}
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
  {% endraw %}


  # Create instance: noc_virt_00, and set properties
  set noc_virt_00 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc noc_virt_00 ]
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

  # Create instance: noc_virt_01, and set properties
  set noc_virt_01 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc noc_virt_01 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
    CONFIG.NUM_NSI {0} \
  ] $noc_virt_01


  set_property -dict [ list \
   CONFIG.INI_STRATEGY {driver} \
 ] [get_bd_intf_pins /noc_virt_01/M00_INI]

  set_property -dict [ list \
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /noc_virt_01/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /noc_virt_01/aclk0]

  # Create instance: noc_virt_02, and set properties
  set noc_virt_02 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc noc_virt_02 ]
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
  set noc_virt_03 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc noc_virt_03 ]
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

  # Create instance: qdma_slave_bridge_noc, and set properties
  set qdma_slave_bridge_noc [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc qdma_slave_bridge_noc ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
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

  connect_bd_net -net util_ds_buf_0_BUFG_FABRIC_O  [get_bd_pins util_ds_buf_0/BUFG_FABRIC_O] \
  [get_bd_pins ilreduced_logic_0/Op1]

  connect_bd_net -net arstn_1  [get_bd_ports arstn] \
  [get_bd_pins c_shift_ram_0/D]
  
  connect_bd_net -net c_shift_ram_0_Q  [get_bd_pins c_shift_ram_0/Q] \
  [get_bd_pins util_ds_buf_0/BUFG_FABRIC_I]


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
  connect_bd_intf_net -intf_net axi_noc_1_M00_INI [get_bd_intf_ports QDMA_SLAVE_BRIDGE_0] [get_bd_intf_pins qdma_slave_bridge_noc/M00_INI]
  connect_bd_intf_net -intf_net dcmac_axis_noc_0_M00_INIS [get_bd_intf_ports M_DCMAC_INIS0] [get_bd_intf_pins dcmac_axis_noc_0/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_1_M00_INIS [get_bd_intf_ports M_DCMAC_INIS1] [get_bd_intf_pins dcmac_axis_noc_1/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_2_M00_INIS [get_bd_intf_ports M_DCMAC_INIS2] [get_bd_intf_pins dcmac_axis_noc_2/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_3_M00_INIS [get_bd_intf_ports M_DCMAC_INIS3] [get_bd_intf_pins dcmac_axis_noc_3/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_4_M00_INIS [get_bd_intf_ports M_DCMAC_INIS4] [get_bd_intf_pins dcmac_axis_noc_4/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_5_M00_INIS [get_bd_intf_ports M_DCMAC_INIS5] [get_bd_intf_pins dcmac_axis_noc_5/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_6_M00_INIS [get_bd_intf_ports M_DCMAC_INIS6] [get_bd_intf_pins dcmac_axis_noc_6/M00_INIS]
  connect_bd_intf_net -intf_net dcmac_axis_noc_7_M00_INIS [get_bd_intf_ports M_DCMAC_INIS7] [get_bd_intf_pins dcmac_axis_noc_7/M00_INIS]
  connect_bd_intf_net -intf_net ddr_noc_0_M00_INI [get_bd_intf_ports M00_INI] [get_bd_intf_pins ddr_noc_0/M00_INI]
  connect_bd_intf_net -intf_net ddr_noc_1_M00_INI [get_bd_intf_ports M01_INI] [get_bd_intf_pins ddr_noc_1/M00_INI]
  connect_bd_intf_net -intf_net ddr_noc_2_M00_INI [get_bd_intf_ports M02_INI] [get_bd_intf_pins ddr_noc_2/M00_INI]
  connect_bd_intf_net -intf_net ddr_noc_3_M00_INI [get_bd_intf_ports M03_INI] [get_bd_intf_pins ddr_noc_3/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_00_M00_INI [get_bd_intf_ports HBM_VNOC_INI_00] [get_bd_intf_pins hbm_vnoc_00/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_01_M00_INI [get_bd_intf_ports HBM_VNOC_INI_01] [get_bd_intf_pins hbm_vnoc_01/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_02_M00_INI [get_bd_intf_ports HBM_VNOC_INI_02] [get_bd_intf_pins hbm_vnoc_02/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_03_M00_INI [get_bd_intf_ports HBM_VNOC_INI_03] [get_bd_intf_pins hbm_vnoc_03/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_04_M00_INI [get_bd_intf_ports HBM_VNOC_INI_04] [get_bd_intf_pins hbm_vnoc_04/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_05_M00_INI [get_bd_intf_ports HBM_VNOC_INI_05] [get_bd_intf_pins hbm_vnoc_05/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_06_M00_INI [get_bd_intf_ports HBM_VNOC_INI_06] [get_bd_intf_pins hbm_vnoc_06/M00_INI]
  connect_bd_intf_net -intf_net hbm_vnoc_07_M00_INI [get_bd_intf_ports HBM_VNOC_INI_07] [get_bd_intf_pins hbm_vnoc_07/M00_INI]
  connect_bd_intf_net -intf_net noc_virt_00_M00_INI [get_bd_intf_ports SL_VIRT_00] [get_bd_intf_pins noc_virt_00/M00_INI]
  connect_bd_intf_net -intf_net noc_virt_01_M00_INI [get_bd_intf_ports SL_VIRT_01] [get_bd_intf_pins noc_virt_01/M00_INI]
  connect_bd_intf_net -intf_net noc_virt_02_M00_INI [get_bd_intf_ports SL_VIRT_02] [get_bd_intf_pins noc_virt_02/M00_INI]
  connect_bd_intf_net -intf_net noc_virt_03_M00_INI [get_bd_intf_ports SL_VIRT_03] [get_bd_intf_pins noc_virt_03/M00_INI]

  # Create port connections
  connect_bd_net -net user_clk_net  [get_bd_pins user_clk] \
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
  [get_bd_pins noc_virt_00/aclk0] \
  [get_bd_pins noc_virt_01/aclk0] \
  [get_bd_pins noc_virt_02/aclk0] \
  [get_bd_pins noc_virt_03/aclk0] \
  [get_bd_pins qdma_slave_bridge_noc/aclk0] \
  [get_bd_pins axi_noc_0/aclk0] \
  [get_bd_pins c_shift_ram_0/CLK]

  # Tie-off RX tready only for unused DCMAC RX NoC slots.
  {% if dcmac_rx_tready_tie_pins|default([]) %}
  connect_bd_net -net xlconstant_0_dout  [get_bd_pins xlconstant_0/dout] \
  {% for p in dcmac_rx_tready_tie_pins %}
  [get_bd_pins {{ p }}]{{ " \\" if not loop.last else "" }}
  {% endfor %}
  {% endif %}

# === Instantiate kernel IPs ===
{% for name, inst in instances.items() %}
set {{ name }} [ create_bd_cell -type ip -vlnv {{ inst.kernel.vlnv }} {{ name }} ]
{% endfor %}

# === Per-kernel AXI-MM data width tweaks for HBM/VIRT ===
{% for p in data_width_params %}
#set_property {{ p.param }} {{ "{" ~ p.value ~ "}" }} [get_bd_cells {{ p.inst }}]
{% endfor %}


# === Connect kernel clocks to aclk1 ===
{% for c in clocks %}
connect_bd_net [get_bd_pins {{ c.src_pin }}] [get_bd_pins user_clk]
{% endfor %}

# === Connect kernel resets to ap_rst_n ===
{% for r in resets %}
connect_bd_net [get_bd_pins {{ r.src_pin }}] [get_bd_pins ilreduced_logic_0/Res]
{% endfor %}

# === SmartConnects for AXI-Lite control ===
{% for sc in smartconnects %}
# Create {{ sc.name }}
set {{ sc.name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 {{ sc.name }} ]
set_property -dict [list \
  CONFIG.NUM_CLKS {1} \
  CONFIG.NUM_SI {1} \
  CONFIG.NUM_MI {{ '{' ~ sc.num_mi ~ '}' }} \
] ${{ sc.name }}

# Clocks/Reset
connect_bd_net [get_bd_pins {{ sc.name }}/aclk]    [get_bd_pins user_clk]
connect_bd_net [get_bd_pins {{ sc.name }}/aresetn] [get_bd_pins ilreduced_logic_0/Res]

# SI (slave) connection
{% if sc.si_from.type == 'bd_port' %}
connect_bd_intf_net [get_bd_intf_pins {{ sc.si_from.name }}] [get_bd_intf_pins {{ sc.name }}/S00_AXI]
{% else %}
connect_bd_intf_net [get_bd_intf_pins {{ sc.si_from.prev }}/M{{ "%02d"|format(sc.chain_slot) }}_AXI] [get_bd_intf_pins {{ sc.name }}/S00_AXI]
{% endif %}

# MI (master) connections to kernel AXI-Lite pins
{% for m in sc.mi %}
connect_bd_intf_net [get_bd_intf_pins {{ sc.name }}/M{{ "%02d"|format(m.slot) }}_AXI] [get_bd_intf_pins {{ m.dst_pin }}]
{% endfor %}

{% endfor %}

# === HBM AXI-MM connections ===

# === HBM reduction nodes (internal fan-in) ===
{% for n in hbm_reduce_nodes|default([]) %}
# {{ n.name }} (NUM_SI={{ n.num_si }}, NUM_MI=1)
set {{ n.name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 {{ n.name }} ]
set_property -dict [list \
  CONFIG.NUM_CLKS {1} \
  CONFIG.NUM_MI   {1} \
  CONFIG.NUM_SI   {{ "{" ~ n.num_si ~ "}" }} \
] ${{ n.name }}
connect_bd_net [get_bd_pins {{ n.name }}/aclk]    [get_bd_pins {{ n.clk }}]
connect_bd_net [get_bd_pins {{ n.name }}/aresetn] [get_bd_pins {{ n.rst }}]
{% for si in n.si %}
connect_bd_intf_net \
  [get_bd_intf_pins {{ si.src }}] \
  [get_bd_intf_pins {{ n.name }}/S{{ "%02d"|format(si.slot) }}_AXI]
{% endfor %}
{% endfor %}

# === HBM root SmartConnects (instantiate only for channels with writers) ===
{% for r in hbm_root_create|default([]) %}
# {{ r.name }} drives HBM{{ "%02d"|format(r.idx) }}
set {{ r.name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 {{ r.name }} ]
set_property -dict [list \
  CONFIG.NUM_CLKS {2} \
  CONFIG.NUM_MI   {1} \
  CONFIG.NUM_SI   {1} \
] ${{ r.name }}
# Clocks / reset
connect_bd_net [get_bd_pins {{ r.name }}/aclk]   [get_bd_pins {{ r.clk0 }}]
connect_bd_net [get_bd_pins {{ r.name }}/aclk1]  {{ r.clk1 }}
connect_bd_net [get_bd_pins {{ r.name }}/aresetn] [get_bd_pins {{ r.rst }}]
{% endfor %}

# === Wire into the root (either single source or last reduction MI) ===
{% for w in hbm_root_in|default([]) %}
connect_bd_intf_net \
  [get_bd_intf_pins {{ w.src_pin }}] \
  [get_bd_intf_pins {{ w.dst_pin }}]
{% endfor %}

# === Root MI -> real HBM port ===
{% for o in hbm_root_out|default([]) %}
connect_bd_intf_net \
  [get_bd_intf_pins {{ o.src_pin }}] \
  [get_bd_intf_ports {{ o.dst_port }}]
{% endfor %}


# === DDR AXI-MM connections (via Versal NoC) ===

# Direct connects: single writer to a DDRx NoC slave
{% for c in ddr_direct|default([]) %}
connect_bd_intf_net [get_bd_intf_pins {{ c.src_pin }}] [get_bd_intf_pins {{ c.dst_pin }}]
{% endfor %}

# SmartConnect reduction nodes (NUM_CLKS=1, aclk→aclk1, aresetn→ap_rst_n)
{% for n in ddr_smart_nodes|default([]) %}
# {{ n.name }} (NUM_SI={{ n.num_si }}, NUM_MI=1)
set {{ n.name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 {{ n.name }} ]
set_property -dict [list \
  CONFIG.NUM_CLKS {1} \
  CONFIG.NUM_MI {1} \
  CONFIG.NUM_SI {{ '{' ~ n.num_si ~ '}' }} \
] ${{ n.name }}

# Clocks/Reset
connect_bd_net [get_bd_pins {{ n.name }}/aclk]    [get_bd_pins user_clk]
connect_bd_net [get_bd_pins {{ n.name }}/aresetn] [get_bd_pins ilreduced_logic_0/Res]

# SIs into this SmartConnect
{% for si in n.si %}
connect_bd_intf_net [get_bd_intf_pins {{ si.src }}] [get_bd_intf_pins {{ n.name }}/S{{ "%02d"|format(si.slot) }}_AXI]
{% endfor %}

{% endfor %}

# Root outputs to DDR NoC slaves
{% for r in ddr_smart_roots|default([]) %}
connect_bd_intf_net [get_bd_intf_pins {{ r.sc_name }}/M00_AXI] [get_bd_intf_pins {{ r.dst_pin }}]
{% endfor %}

# === MEM AXI-MM connections (via VNOC) ===

# Direct connects: single writer to a MEMx VNOC slave
{% for c in mem_direct|default([]) %}
connect_bd_intf_net [get_bd_intf_pins {{ c.src_pin }}] [get_bd_intf_pins {{ c.dst_pin }}]
{% endfor %}

# SmartConnect reduction nodes for MEMx (NUM_CLKS=1, aclk→aclk1, aresetn→ap_rst_n)
{% for n in mem_smart_nodes|default([]) %}
# {{ n.name }} (NUM_SI={{ n.num_si }}, NUM_MI=1)
set {{ n.name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 {{ n.name }} ]
set_property -dict [list \
  CONFIG.NUM_CLKS {1} \
  CONFIG.NUM_MI {1} \
  CONFIG.NUM_SI {{ '{' ~ n.num_si ~ '}' }} \
] ${{ n.name }}

# Clocks/Reset
connect_bd_net [get_bd_pins {{ n.name }}/aclk]    [get_bd_pins user_clk]
connect_bd_net [get_bd_pins {{ n.name }}/aresetn] [get_bd_pins ilreduced_logic_0/Res]

# SIs into this SmartConnect
{% for si in n.si %}
connect_bd_intf_net [get_bd_intf_pins {{ si.src }}] [get_bd_intf_pins {{ n.name }}/S{{ "%02d"|format(si.slot) }}_AXI]
{% endfor %}

{% endfor %}

# Root outputs to VNOC slaves
{% for r in mem_smart_roots|default([]) %}
connect_bd_intf_net [get_bd_intf_pins {{ r.sc_name }}/M00_AXI] [get_bd_intf_pins {{ r.dst_pin }}]
{% endfor %}

# === VIRT AXI-MM connections (connects to NoC) ===
{% for c in virt_direct|default([]) %}
connect_bd_intf_net [get_bd_intf_pins {{ c.src_pin }}] [get_bd_intf_pins {{ c.dst_pin }}]
{% endfor %}

# VIRT SmartConnect nodes (NUM_CLKS=1; aclk=aclk1, aresetn=ap_rst_n)
{% for n in virt_smart_nodes|default([]) %}
# {{ n.name }} (NUM_SI={{ n.num_si }}, NUM_MI=1)
set {{ n.name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 {{ n.name }} ]
set_property -dict [list \
  CONFIG.NUM_CLKS {1} \
  CONFIG.NUM_MI   {1} \
  CONFIG.NUM_SI   {{ "{" ~ n.num_si ~ "}" }} \
] ${{ n.name }}

# Clock / Reset
connect_bd_net [get_bd_pins {{ n.name }}/aclk]    [get_bd_pins user_clk]
connect_bd_net [get_bd_pins {{ n.name }}/aresetn] [get_bd_pins ilreduced_logic_0/Res]

# SI fan-in
{% for si in n.si %}
connect_bd_intf_net \
  [get_bd_intf_pins {{ si.src }}] \
  [get_bd_intf_pins {{ n.name }}/S{{ "%02d"|format(si.slot) }}_AXI]
{% endfor %}
{% endfor %}

# VIRT SmartConnect roots → NoC pin
{% for r in virt_smart_roots|default([]) %}
connect_bd_intf_net \
  [get_bd_intf_pins {{ r.sc_name }}/M00_AXI] \
  [get_bd_intf_pins {{ r.dst_pin }}]
{% endfor %}


# === AXIS stream connections from config ===
{% for e in axis_streams|default([]) %}
connect_bd_intf_net [get_bd_intf_pins {{ e.src_pin }}] [get_bd_intf_pins {{ e.dst_pin }}]
{% endfor %}

# === AXIS network: instance -> fabric TX ===
{% for e in axis_to_fabric|default([]) %}
connect_bd_intf_net [get_bd_intf_pins {{ e.src_pin }}] [get_bd_intf_pins {{ e.dst_pin }}]
{% endfor %}

# === AXIS network: fabric RX -> instance ===
{% for e in axis_from_fabric|default([]) %}
connect_bd_intf_net [get_bd_intf_pins {{ e.src_pin }}] [get_bd_intf_pins {{ e.dst_pin }}]
{% endfor %}

# === AXI Register Slice terminators for UNUSED memory endpoints ===
{% for t in axi_terminators|default([]) %}
# {{ t.name }} -> {{ t.dst }}
set {{ t.name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 {{ t.name }} ]

# Clock / Reset (defaults to user_clk and arstn if not provided)
connect_bd_net [get_bd_pins {{ t.name }}/aclk]    [get_bd_pins {{ t.clk|default('user_clk') }}]
connect_bd_net [get_bd_pins {{ t.name }}/aresetn] [get_bd_pins {{ t.rst|default('ilreduced_logic_0/Res') }}]

# Leave S_AXI unconnected on purpose

# Connect M_AXI to the free destination pin
connect_bd_intf_net [get_bd_intf_pins {{ t.name }}/M_AXI] [get_bd_intf_pins {{ t.dst }}]

{% endfor %}


# === HOST aggregation: SmartConnect tree -> QDMA_SLAVE_BRIDGE ===
{% for c in host_direct|default([]) %}
connect_bd_intf_net [get_bd_intf_pins {{ c.src_pin }}] [get_bd_intf_pins {{ c.dst_pin }}]
{% endfor %}

# HOST SmartConnect nodes (if any)
{% for n in host_smart_nodes|default([]) %}
# {{ n.name }} (NUM_SI={{ n.num_si }}, NUM_MI=1)
set {{ n.name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 {{ n.name }} ]
set_property -dict [list \
  CONFIG.NUM_CLKS {1} \
  CONFIG.NUM_MI   {1} \
  CONFIG.NUM_SI   {{ "{" ~ n.num_si ~ "}" }} \
] ${{ n.name }}
connect_bd_net [get_bd_pins {{ n.name }}/aclk]    [get_bd_pins user_clk]
connect_bd_net [get_bd_pins {{ n.name }}/aresetn] [get_bd_pins ilreduced_logic_0/Res]
{% for si in n.si %}
connect_bd_intf_net \
  [get_bd_intf_pins {{ si.src }}] \
  [get_bd_intf_pins {{ n.name }}/S{{ "%02d"|format(si.slot) }}_AXI]
{% endfor %}
{% endfor %}

# HOST SmartConnect root to NoC sink
{% for r in host_smart_roots|default([]) %}
connect_bd_intf_net \
  [get_bd_intf_pins {{ r.sc_name }}/M00_AXI] \
  [get_bd_intf_pins {{ r.dst_pin }}]
{% endfor %}


# === Optional AXIS ILA debug ===
{% if debug_axis_ila_enabled|default(false) %}
set {{ debug_axis_ila_name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_ila:1.3 {{ debug_axis_ila_name }} ]
set_property -dict [list \
  CONFIG.C_MON_TYPE {Interface_Monitor} \
  CONFIG.C_NUM_MONITOR_SLOTS {{ "{" ~ debug_axis_ila_num_slots ~ "}" }} \
] [get_bd_cells {{ debug_axis_ila_name }}]
{% for s in debug_axis_ila_slots %}
set_property CONFIG.C_SLOT_{{ s.idx }}_INTF_TYPE {{ "{" ~ s.intf_type ~ "}" }} [get_bd_cells {{ debug_axis_ila_name }}]
{% endfor %}
connect_bd_net [get_bd_pins {{ debug_axis_ila_name }}/clk] [get_bd_pins user_clk]
connect_bd_net [get_bd_pins {{ debug_axis_ila_name }}/resetn] [get_bd_pins ilreduced_logic_0/Res]
{% for s in debug_axis_ila_slots %}
connect_bd_intf_net [get_bd_intf_pins {{ debug_axis_ila_name }}/{{ s.slot_pin }}] [get_bd_intf_pins {{ s.src_pin }}]
{% endfor %}
{% endif %}

# === AXI-Lite address map ===
{% for a in axilite_addr %}
assign_bd_address -offset {{ "0x%012X"|format(a.offset) }} -range {{ "0x%08X"|format(a.range)  }} -target_address_space [get_bd_addr_spaces {{ a.addr_space }}] [get_bd_addr_segs {{ a.inst }}/{{ a.busif }}/{{ a.segment }}] -force
{% endfor %}

# === Assign all other addresses ===
assign_bd_address
validate_bd_design
save_bd_design

# current_bd_design [get_bd_designs top]
# validate_bd_design
# save_bd_design
