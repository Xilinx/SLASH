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
set_property APERTURES {{0xE000_0000 256M}} [get_bd_intf_ports M_QDMA_SLV_BRIDGE]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports M_VIRT_0]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports M_VIRT_1]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports M_VIRT_2]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports M_VIRT_3]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports SL2NOC_0]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports SL2NOC_1]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports SL2NOC_2]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports SL2NOC_3]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports SL2NOC_4]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports SL2NOC_5]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports SL2NOC_6]
set_property APERTURES {{0x600_0000_0000 32G}} [get_bd_intf_ports SL2NOC_7]
set_property APERTURES {{0x203_0000_0000 128M}} [get_bd_intf_ports S_AXILITE_INI]
set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_ports S_QDMA_SLV_BRIDGE]
set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_ports S_VIRT_00]
set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_ports S_VIRT_01]
set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_ports S_VIRT_02]
set_property APERTURES {{0x208_0000_0000 32G}} [get_bd_intf_ports S_VIRT_03]
{% endraw %}
    set axi_noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc axi_noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_0

{% raw %}
  set_property -dict [ list \
   CONFIG.APERTURES {{0x203_0000_0000 128M}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_0/M00_AXI]
  
  set_property -dict [ list \
   CONFIG.INI_STRATEGY {load} \
   CONFIG.CONNECTIONS {M00_AXI {read_bw {5} write_bw {5} read_avg_burst {4} write_avg_burst {4}}} \
 ] [get_bd_intf_pins /axi_noc_0/S00_INI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {M00_AXI:S_QDMA_SLV_BRIDGE:M_QDMA_SLV_BRIDGE:S_VIRT_3:S_VIRT_2:S_VIRT_1:S_VIRT_0} \
 ] [get_bd_pins /axi_noc_0/aclk0]
  set_property APERTURES {{0x203_0000_0000 128M}} [get_bd_intf_ports S_AXILITE_INI]
  {% endraw %}

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

# Create instance: sl2noc_0, and set properties
  set sl2noc_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc sl2noc_0 ]
  set_property -dict [list \
    CONFIG.NUM_MI {0} \
    CONFIG.NUM_NMI {1} \
  ] $sl2noc_0


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
   CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}} \
   CONFIG.DEST_IDS {} \
   CONFIG.NOC_PARAMS {} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /sl2noc_7/S00_AXI]

  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {S00_AXI} \
 ] [get_bd_pins /sl2noc_7/aclk0]


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
  
    # Create instance: axi_noc_1, and set properties
  set axi_noc_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc axi_noc_1 ]
  set_property -dict [list \
    CONFIG.MI_SIDEBAND_PINS {} \
    CONFIG.NUM_NSI {1} \
    CONFIG.NUM_SI {0} \
  ] $axi_noc_1

{% raw %}
  set_property -dict [ list \
   CONFIG.APERTURES {{0x208_0000_0000 32G}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_1/M00_AXI]
{% endraw %}
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

{% raw %}
  set_property -dict [ list \
   CONFIG.APERTURES {{0x208_0000_0000 32G}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_2/M00_AXI]
{% endraw %}
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

{% raw %}
  set_property -dict [ list \
   CONFIG.APERTURES {{0x208_0000_0000 32G}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_3/M00_AXI]
{% endraw %}
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

{% raw %}
  set_property -dict [ list \
   CONFIG.APERTURES {{0x208_0000_0000 32G}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_4/M00_AXI]
{% endraw %}
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

{% raw %}
  set_property -dict [ list \
   CONFIG.APERTURES {{0x208_0000_0000 32G}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_5/M00_AXI]
{% endraw %}
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


  set_property -dict [list CONFIG.INI_STRATEGY {driver}] [get_bd_intf_pins /sl2noc_0/M00_INI]
  set_property -dict [list CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}}] [get_bd_intf_pins /sl2noc_0/S00_AXI]

  set_property -dict [list CONFIG.INI_STRATEGY {driver}] [get_bd_intf_pins /sl2noc_1/M00_INI]
  set_property -dict [list CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}}] [get_bd_intf_pins /sl2noc_1/S00_AXI]
  set_property -dict [list CONFIG.INI_STRATEGY {driver}] [get_bd_intf_pins /sl2noc_2/M00_INI]
  set_property -dict [list CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}}] [get_bd_intf_pins /sl2noc_2/S00_AXI]
  set_property -dict [list CONFIG.INI_STRATEGY {driver}] [get_bd_intf_pins /sl2noc_3/M00_INI]
  set_property -dict [list CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}}] [get_bd_intf_pins /sl2noc_3/S00_AXI]
  set_property -dict [list CONFIG.INI_STRATEGY {driver}] [get_bd_intf_pins /sl2noc_4/M00_INI]
  set_property -dict [list CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}}] [get_bd_intf_pins /sl2noc_4/S00_AXI]
  set_property -dict [list CONFIG.INI_STRATEGY {driver}] [get_bd_intf_pins /sl2noc_5/M00_INI]
  set_property -dict [list CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}}] [get_bd_intf_pins /sl2noc_5/S00_AXI]
  set_property -dict [list CONFIG.INI_STRATEGY {driver}] [get_bd_intf_pins /sl2noc_6/M00_INI]
  set_property -dict [list CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}}] [get_bd_intf_pins /sl2noc_6/S00_AXI]
  set_property -dict [list CONFIG.INI_STRATEGY {driver}] [get_bd_intf_pins /sl2noc_7/M00_INI]
  set_property -dict [list CONFIG.CONNECTIONS {M00_INI {read_bw {500} write_bw {500}}}] [get_bd_intf_pins /sl2noc_7/S00_AXI]

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

  # Slave bridge connections
  connect_bd_intf_net -intf_net S_QDMA_SLV_BRIDGE_1 [get_bd_intf_ports S_QDMA_SLV_BRIDGE] [get_bd_intf_pins axi_noc_5/S00_INI]
  connect_bd_intf_net -intf_net axi_noc_5_M00_AXI [get_bd_intf_pins axi_register_slice_8/S_AXI] [get_bd_intf_pins axi_noc_5/M00_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_2_M_AXI3 [get_bd_intf_pins axi_register_slice_8/M_AXI] [get_bd_intf_pins axi4_full_passthrough_4/s_axi]
  connect_bd_intf_net -intf_net axi4_full_passthrough_1_m_axi3 [get_bd_intf_pins axi4_full_passthrough_4/m_axi] [get_bd_intf_pins axi_register_slice_9/S_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_9_M_AXI [get_bd_intf_pins axi_register_slice_9/M_AXI] [get_bd_intf_pins noc_virt_4/S00_AXI]
  connect_bd_intf_net -intf_net noc_virt_5_M00_INI [get_bd_intf_ports M_QDMA_SLV_BRIDGE] [get_bd_intf_pins noc_virt_4/M00_INI]

  # Virtual NOC connections
  connect_bd_intf_net -intf_net S_VIRT_00_1 [get_bd_intf_ports S_VIRT_00] [get_bd_intf_pins axi_noc_1/S00_INI]
  connect_bd_intf_net -intf_net axi_noc_1_M00_AXI [get_bd_intf_pins axi_register_slice_0/S_AXI] [get_bd_intf_pins axi_noc_1/M00_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_0_M_AXI [get_bd_intf_pins axi_register_slice_0/M_AXI] [get_bd_intf_pins axi4_full_passthrough_0/s_axi]
  connect_bd_intf_net -intf_net axi4_full_passthrough_0_m_axi [get_bd_intf_pins axi_register_slice_1/S_AXI] [get_bd_intf_pins axi4_full_passthrough_0/m_axi]
  connect_bd_intf_net -intf_net axi_register_slice_1_M_AXI [get_bd_intf_pins axi_register_slice_1/M_AXI] [get_bd_intf_pins noc_virt_0/S00_AXI]
  connect_bd_intf_net -intf_net axi_noc_1_M00_INI [get_bd_intf_ports M_VIRT_0] [get_bd_intf_pins noc_virt_0/M00_INI]

  connect_bd_intf_net -intf_net S_VIRT_01_1 [get_bd_intf_ports S_VIRT_01] [get_bd_intf_pins axi_noc_2/S00_INI]
  connect_bd_intf_net -intf_net axi_noc_2_M00_AXI [get_bd_intf_pins axi_register_slice_2/S_AXI] [get_bd_intf_pins axi_noc_2/M00_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_2_M_AXI [get_bd_intf_pins axi_register_slice_2/M_AXI] [get_bd_intf_pins axi4_full_passthrough_1/s_axi]
  connect_bd_intf_net -intf_net axi4_full_passthrough_1_m_axi [get_bd_intf_pins axi4_full_passthrough_1/m_axi] [get_bd_intf_pins axi_register_slice_3/S_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_3_M_AXI [get_bd_intf_pins noc_virt_1/S00_AXI] [get_bd_intf_pins axi_register_slice_3/M_AXI]
  connect_bd_intf_net -intf_net axi_noc_2_M00_INI [get_bd_intf_ports M_VIRT_1] [get_bd_intf_pins noc_virt_1/M00_INI]

  connect_bd_intf_net -intf_net S_VIRT_02_1 [get_bd_intf_ports S_VIRT_02] [get_bd_intf_pins axi_noc_3/S00_INI]
  connect_bd_intf_net -intf_net axi_noc_3_M00_AXI [get_bd_intf_pins axi_register_slice_4/S_AXI] [get_bd_intf_pins axi_noc_3/M00_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_2_M_AXI1 [get_bd_intf_pins axi_register_slice_4/M_AXI] [get_bd_intf_pins axi4_full_passthrough_2/s_axi]
  connect_bd_intf_net -intf_net axi4_full_passthrough_1_m_axi1 [get_bd_intf_pins axi4_full_passthrough_2/m_axi] [get_bd_intf_pins axi_register_slice_5/S_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_5_M_AXI [get_bd_intf_pins axi_register_slice_5/M_AXI] [get_bd_intf_pins noc_virt_2/S00_AXI]
  connect_bd_intf_net -intf_net axi_noc_3_M00_INI [get_bd_intf_ports M_VIRT_2] [get_bd_intf_pins noc_virt_2/M00_INI]


  connect_bd_intf_net -intf_net S_VIRT_03_1 [get_bd_intf_ports S_VIRT_03] [get_bd_intf_pins axi_noc_4/S00_INI]
  connect_bd_intf_net -intf_net axi_noc_4_M00_AXI [get_bd_intf_pins axi_register_slice_6/S_AXI] [get_bd_intf_pins axi_noc_4/M00_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_2_M_AXI2 [get_bd_intf_pins axi_register_slice_6/M_AXI] [get_bd_intf_pins axi4_full_passthrough_3/s_axi]
  connect_bd_intf_net -intf_net axi4_full_passthrough_1_m_axi2 [get_bd_intf_pins axi4_full_passthrough_3/m_axi] [get_bd_intf_pins axi_register_slice_7/S_AXI]
  connect_bd_intf_net -intf_net axi_register_slice_7_M_AXI [get_bd_intf_pins axi_register_slice_7/M_AXI] [get_bd_intf_pins noc_virt_3/S00_AXI]
  connect_bd_intf_net -intf_net axi_noc_4_M00_INI [get_bd_intf_ports M_VIRT_3] [get_bd_intf_pins noc_virt_3/M00_INI]




  connect_bd_intf_net -intf_net sl2noc_0_M00_INI [get_bd_intf_ports SL2NOC_0] [get_bd_intf_pins sl2noc_0/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_1_M00_INI [get_bd_intf_ports SL2NOC_1] [get_bd_intf_pins sl2noc_1/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_2_M00_INI [get_bd_intf_ports SL2NOC_2] [get_bd_intf_pins sl2noc_2/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_3_M00_INI [get_bd_intf_ports SL2NOC_3] [get_bd_intf_pins sl2noc_3/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_4_M00_INI [get_bd_intf_ports SL2NOC_4] [get_bd_intf_pins sl2noc_4/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_5_M00_INI [get_bd_intf_ports SL2NOC_5] [get_bd_intf_pins sl2noc_5/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_6_M00_INI [get_bd_intf_ports SL2NOC_6] [get_bd_intf_pins sl2noc_6/M00_INI]
  connect_bd_intf_net -intf_net sl2noc_7_M00_INI [get_bd_intf_ports SL2NOC_7] [get_bd_intf_pins sl2noc_7/M00_INI]


  connect_bd_net -net service_clk_1  [get_bd_pins service_clk] \
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
  [get_bd_pins sl2noc_0/aclk0] \
  [get_bd_pins sl2noc_1/aclk0] \
  [get_bd_pins sl2noc_2/aclk0] \
  [get_bd_pins sl2noc_3/aclk0] \
  [get_bd_pins sl2noc_4/aclk0] \
  [get_bd_pins sl2noc_5/aclk0] \
  [get_bd_pins sl2noc_6/aclk0] \
  [get_bd_pins sl2noc_7/aclk0] \
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
  [get_bd_pins axi_register_slice_8/aresetn]

  connect_bd_net -net xlconstant_0_dout  [get_bd_pins xlconstant_0/dout] \
  [get_bd_pins dummy_noc_1/M00_AXIS_tready] \
  [get_bd_pins dummy_noc_2/M00_AXIS_tready] \
  [get_bd_pins dummy_noc_3/M00_AXIS_tready] \
  [get_bd_pins dummy_noc_5/M00_AXIS_tready] \
  [get_bd_pins dummy_noc_6/M00_AXIS_tready] \
  [get_bd_pins dummy_noc_7/M00_AXIS_tready]


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
# ===== Service Layer (generated) =====
# create_service_layer ""

# Absolute paths (normalized)
set ::slash_dcmac_tcl  [file normalize "{{ dcmac_tcl }}"]
set ::slash_dcmac_hdl  [file normalize "{{ dcmac_hdl_dir }}"]

# Source the DCMAC Tcl helpers
source $::slash_dcmac_tcl

{% for vf in dcmac_hdl_files %}
import_files -fileset sources_1 -norecurse [file normalize "{{ vf }}"]
{% endfor %}

# --- Drive DCMAC creation based on config ---
{% if needs_dcmac %}
  set DCMAC0_ENABLED {{ dc_enable_0 }}
  set DCMAC1_ENABLED {{ dc_enable_1 }}
  set DUAL_QSFP_DCMAC0 {{ dual_qsfp_0 }}
  set DUAL_QSFP_DCMAC1 {{ dual_qsfp_1 }}

  # Calls proc add_dcmac_inst which expects the above variables
  add_dcmac_inst
{% else %}
  set DCMAC0_ENABLED 0
  set DCMAC1_ENABLED 0
  set DUAL_QSFP_DCMAC0 0
  set DUAL_QSFP_DCMAC1 0
  # Ethernet disabled; no DCMAC hierarchy created.
{% endif %}

# === AXI-Lite SmartConnect for service_layer control ===
# === AXI-Lite SmartConnect for service_layer control ===
{% if sl_have_xbar %}
  current_bd_design service_layer_user

  # Create SmartConnect inside the service_layer BD with a local name
  create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_0
  set_property -dict [list \
    CONFIG.NUM_CLKS {{ "{" ~ sl_num_clks ~ "}" }} \
    CONFIG.NUM_MI   {{ "{" ~ sl_num_mi  ~ "}" }} \
    CONFIG.NUM_SI   {{ "{" ~ sl_num_si  ~ "}" }} \
  ] [get_bd_cells smartconnect_0]
  # Rename for convenience
  set_property name {{ sl_smartconnect_name }} [get_bd_cells smartconnect_0]

  # Clocks & reset (design pins to SC pins)
  connect_bd_net [get_bd_pins {{ sl_clk0 }}] [get_bd_pins {{ sl_smartconnect_name }}/aclk]
  connect_bd_net [get_bd_pins {{ sl_rstn }}] [get_bd_pins {{ sl_smartconnect_name }}/aresetn]

  # SI: service_layer design's S_AXILITE -> SmartConnect S00_AXI
  connect_bd_intf_net \
    [get_bd_intf_pins {{ sl_si_src_if }}] \
    [get_bd_intf_pins {{ sl_smartconnect_name }}/S00_AXI]

  # MI: fan-out to DCMAC hierarchies' s_axi
  {% for tgt in sl_mi_targets %}
    {% set idx = "%02d"|format(loop.index0) %}
    connect_bd_intf_net \
      [get_bd_intf_pins {{ sl_smartconnect_name }}/M{{ idx }}_AXI] \
      [get_bd_intf_pins {{ tgt }}]
  {% endfor %}

  # Tie QSFP block clocks/resets
  {% for q in sl_qsfp_blocks %}
    connect_bd_net [get_bd_pins {{ sl_clk0 }}] [get_bd_pins {{ q }}/ap_clk]
    connect_bd_net [get_bd_pins {{ sl_rstn }}] [get_bd_pins {{ q }}/ap_rst_n]
  {% endfor %}

{% else %}
  # No AXI-Lite users: create a dummy SmartConnect with clocks+reset,
  # connect S_AXILITE -> S00_AXI, but leave the single M00_AXI unconnected.
  # current_bd_design service_layer_user

  # create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_0
  # create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 hbm_bandwidth_0
  # set_property -dict [list \
  #   CONFIG.NUM_CLKS {1} \
  #   CONFIG.NUM_MI   {1} \
  #   CONFIG.NUM_SI   {1} \
  # ] [get_bd_cells smartconnect_0]
  # set_property name {{ sl_smartconnect_name }} [get_bd_cells smartconnect_0]

  # # Clocks & reset
  # connect_bd_net [get_bd_pins service_clk]     [get_bd_pins {{ sl_smartconnect_name }}/aclk]
  # connect_bd_net [get_bd_pins arstn]  [get_bd_pins {{ sl_smartconnect_name }}/aresetn]

  # # SI: service_layer design's S_AXILITE -> SmartConnect S00_AXI
  # connect_bd_intf_net \
  #   [get_bd_intf_pins axi_noc_0/M00_AXI] \
  #   [get_bd_intf_pins {{ sl_smartconnect_name }}/S00_AXI]

  # # M00_AXI connected to dummy HBM bandwidth core
  # connect_bd_intf_net [get_bd_intf_pins hbm_bandwidth_0/s_axi_control] [get_bd_intf_pins smartconnect_0/M00_AXI]
  # connect_bd_net [get_bd_ports service_clk] [get_bd_pins hbm_bandwidth_0/ap_clk]
  # connect_bd_net [get_bd_ports arstn] [get_bd_pins hbm_bandwidth_0/ap_rst_n]
  # connect_bd_intf_net [get_bd_intf_pins hbm_bandwidth_0/m_axi_gmem0] [get_bd_intf_pins sl2noc_0/S00_AXI]

{% endif %}


# === QSFP <-> NoC AXIS links (inside service_layer) ===
{% if sl_axis_noc_links %}
  # Enter service_layer hierarchy
  set __oldCurInst [current_bd_instance .]
  current_bd_instance [get_bd_cells /service_layer]

  {% for L in sl_axis_noc_links %}
  # Link: {{ L.src_pin }} -> {{ L.dst_pin }}
  set __src [get_bd_intf_pins {{ L.src_pin }}]
  set __dst [get_bd_intf_pins {{ L.dst_pin }}]
  if { $__src eq "" } {
    error "AXIS source pin '{{ L.src_pin }}' not found in service_layer."
  }
  if { $__dst eq "" } {
    error "AXIS dest pin '{{ L.dst_pin }}' not found in service_layer."
  }
  connect_bd_intf_net $__src $__dst
  {% endfor %}

  # Restore previous instance
  current_bd_instance $__oldCurInst
  assign_bd_address -offset 0x020302040400 -range 0x00000100 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_0_n_1/control_intf/axi_gpio_datapath/S_AXI/Reg] -force
  assign_bd_address -offset 0x020303040400 -range 0x00000100 -with_name SEG_axi_gpio_datapath_Reg_1 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_2_n_3/control_intf/axi_gpio_datapath/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302040000 -range 0x00000100 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_0_n_1/control_intf/axi_gpio_gt_control/S_AXI/Reg] -force
  assign_bd_address -offset 0x020303040000 -range 0x00000100 -with_name SEG_axi_gpio_gt_control_Reg_1 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_2_n_3/control_intf/axi_gpio_gt_control/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302040200 -range 0x00000100 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_0_n_1/control_intf/axi_gpio_monitor/S_AXI/Reg] -force
  assign_bd_address -offset 0x020303040200 -range 0x00000100 -with_name SEG_axi_gpio_monitor_Reg_1 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_2_n_3/control_intf/axi_gpio_monitor/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302040600 -range 0x00000100 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_0_n_1/control_intf/axi_gpio_reset_txrx/S_AXI/Reg] -force
  assign_bd_address -offset 0x020303040600 -range 0x00000100 -with_name SEG_axi_gpio_reset_txrx_Reg_1 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_2_n_3/control_intf/axi_gpio_reset_txrx/S_AXI/Reg] -force
  assign_bd_address -offset 0x020302000000 -range 0x00040000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_0_n_1/DCMAC_subsys/dcmac_0_core/s_axi/Reg] -force
  assign_bd_address -offset 0x020303000000 -range 0x00040000 -target_address_space [get_bd_addr_spaces S_AXILITE_INI] [get_bd_addr_segs qsfp_2_n_3/DCMAC_subsys/dcmac_1_core/s_axi/Reg] -force

{% else %}
  # No QSFP <-> NoC links required
{% endif %}

# @TODO: change this when virtualization core is available.
# === Temporary VIRT wiring: S_VIRT_x -> sl2noc_virt_x/S00_AXI ===
# current_bd_design service_layer_user
# for {set i 0} {$i < 4} {incr i} {
#   set sv     [get_bd_intf_pins S_VIRT_$i]
#   set noc_in [get_bd_intf_pins sl2noc_virt_$i/S00_AXI]

#   if { $sv eq "" } {
#     puts "Info: service_layer: S_VIRT_$i not present; skipping."
#     continue
#   }
#   if { $noc_in eq "" } {
#     puts "Info: service_layer: sl2noc_virt_$i/S00_AXI not present; skipping."
#     continue
#   }
#   connect_bd_intf_net $sv $noc_in
# }

assign_bd_address -offset 0xE0000000 -range 0x00010000 -target_address_space [get_bd_addr_spaces S_QDMA_SLV_BRIDGE] [get_bd_addr_segs M_QDMA_SLV_BRIDGE/Reg] -force
assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces S_VIRT_00] [get_bd_addr_segs M_VIRT_0/Reg] -force
assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces S_VIRT_01] [get_bd_addr_segs M_VIRT_1/Reg] -force
assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces S_VIRT_02] [get_bd_addr_segs M_VIRT_2/Reg] -force
assign_bd_address -offset 0x00000000 -range 0x00010000000000000000 -target_address_space [get_bd_addr_spaces S_VIRT_03] [get_bd_addr_segs M_VIRT_3/Reg] -force
assign_bd_address
validate_bd_design
save_bd_design
# current_bd_design [get_bd_designs top]
# validate_bd_design
# save_bd_design

# ===== End Service Layer =====
