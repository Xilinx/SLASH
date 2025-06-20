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

# hdl files from resources will be available in this script when running at "$src_dir/dcmac/hdl/..."


# Hierarchical cell: dcmac_gt_wrapper
proc create_hier_cell_dcmac_gt_wrapper { parentCell nameHier dcmac_index dual_dcmac } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_dcmac_gt_wrapper() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 qsfp_clk_322mhz

  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_tx_interface_rtl:1.0 TX0_GT0_IP_Interface
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_tx_interface_rtl:1.0 TX1_GT0_IP_Interface
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_tx_interface_rtl:1.0 TX2_GT0_IP_Interface
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_tx_interface_rtl:1.0 TX3_GT0_IP_Interface
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_rx_interface_rtl:1.0 RX0_GT0_IP_Interface
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_rx_interface_rtl:1.0 RX1_GT0_IP_Interface
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_rx_interface_rtl:1.0 RX2_GT0_IP_Interface
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_rx_interface_rtl:1.0 RX3_GT0_IP_Interface
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 GT0_Serial

  if { ${dual_dcmac} == "1" } {
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_tx_interface_rtl:1.0 TX0_GT1_IP_Interface
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_tx_interface_rtl:1.0 TX1_GT1_IP_Interface
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_tx_interface_rtl:1.0 TX2_GT1_IP_Interface
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_tx_interface_rtl:1.0 TX3_GT1_IP_Interface
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_rx_interface_rtl:1.0 RX0_GT1_IP_Interface
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_rx_interface_rtl:1.0 RX1_GT1_IP_Interface
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_rx_interface_rtl:1.0 RX2_GT1_IP_Interface
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:gt_rx_interface_rtl:1.0 RX3_GT1_IP_Interface
    create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 GT1_Serial
  }

  # Create pins
  create_bd_pin -dir I -from 0 -to 0 MBUFG_GT_CLR
  create_bd_pin -dir I -from 0 -to 0 MBUFG_GT_CLRB_LEAF
  create_bd_pin -dir O -from 0 -to 0 -type gt_usrclk qsfp0_rx_usr_clk_664mhz
  create_bd_pin -dir O -from 0 -to 0 -type gt_usrclk qsfp0_rx_usr_clk_332mhz
  create_bd_pin -dir I -from 0 -to 0 MBUFG_GT_CLR1
  create_bd_pin -dir I -from 0 -to 0 MBUFG_GT_CLRB_LEAF1
  create_bd_pin -dir O -from 0 -to 0 -type gt_usrclk qsfp0_tx_usr_clk_664mhz
  create_bd_pin -dir O -from 0 -to 0 -type gt_usrclk qsfp0_tx_usr_clk_332mhz
  create_bd_pin -dir I -type rst hsclk_pllreset0
  create_bd_pin -dir O hsclk_plllock0
  create_bd_pin -dir O gtpowergood_0
  create_bd_pin -dir I -type rst gt0_ch0_iloreset
  create_bd_pin -dir I -type rst gt0_ch1_iloreset
  create_bd_pin -dir I -type rst gt0_ch2_iloreset
  create_bd_pin -dir I -type rst gt0_ch3_iloreset
  create_bd_pin -dir O gt0_ch0_iloresetdone
  create_bd_pin -dir O gt0_ch1_iloresetdone
  create_bd_pin -dir O gt0_ch2_iloresetdone
  create_bd_pin -dir O gt0_ch3_iloresetdone
  create_bd_pin -dir I -type clk apb3clk_quad
  create_bd_pin -dir I -type rst s_axi_aresetn
  create_bd_pin -dir O -type gt_usrclk GT0_ref_clk
  create_bd_pin -dir I -from 31 -to 0 gt_control_pins

  if { ${dual_dcmac} == "1" } {
    create_bd_pin -dir I -type rst hsclk_pllreset1
    create_bd_pin -dir O hsclk_plllock1
    create_bd_pin -dir I -type rst gt1_ch0_iloreset
    create_bd_pin -dir I -type rst gt1_ch1_iloreset
    create_bd_pin -dir I -type rst gt1_ch2_iloreset
    create_bd_pin -dir I -type rst gt1_ch3_iloreset
    create_bd_pin -dir O gt1_ch0_iloresetdone
    create_bd_pin -dir O gt1_ch1_iloresetdone
    create_bd_pin -dir O gt1_ch2_iloresetdone
    create_bd_pin -dir O gt1_ch3_iloresetdone
    create_bd_pin -dir O gtpowergood_1
  }

  # Create instance: util_ds_buf_0, and set properties
  set util_ds_buf_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_ds_buf util_ds_buf_0 ]
  set_property CONFIG.C_BUF_TYPE {IBUFDS_GTME5} $util_ds_buf_0

  set top_dcmac_name "top_dcmac_${dcmac_index}_core_0"
  set dcmac_name "dcmac_${dcmac_index}_core"

  set list_quad_index { 0 }
  if { ${dual_dcmac} == "1" } {
    lappend list_quad_index 1
  }

  foreach idx ${list_quad_index} {
    # Create instance: gt0_quad, and set properties
    set quad_name "gt${idx}_quad"
    set gt_quad [ create_bd_cell -type ip -vlnv xilinx.com:ip:gt_quad_base ${quad_name} ]

    set channel_ordering {}
    foreach dirid {TX RX} {
        for {set lane 0} {$lane < 4} {incr lane} {
          set new_idx ${lane}
          if { ${idx} == "1" } {
            set new_idx "[expr {$lane + 4}]"
          }

          set ord " ${oldCurInst}/${quad_name}/${dirid}${lane}_GT_IP_Interface top_dcmac_${idx}_core_0.${oldCurInst}/dcmac_${idx}_core/gtm_tx_serdes_interface_${new_idx}.${new_idx}"
          append channel_ordering ${ord}
        }
    }

    set quad_usage {}
    foreach dirid {TX_QUAD_CH RX_QUAD_CH} {
      set list_quad_index_ii { 0 }
      if { ${dual_dcmac} == "1" } {
        lappend list_quad_index_ii 1
      }
      append quad_usage " ${dirid} {"
        foreach idxii ${list_quad_index_ii} {
          set quad_name_ii "gt${idxii}_quad"
          set q0 [expr {${idx} == ${idxii}}]
          #set q1 [expr {!$q0}]
          if { ${idxii} == "0" } {
            set conf " ${oldCurInst}/dcmac_gt${idxii}_wrapper/${quad_name_ii} {${oldCurInst}/dcmac_gt${dcmac_index}_wrapper/${quad_name_ii}\
            top_dcmac_${dcmac_index}_core_0.IP_CH0,top_dcmac_${dcmac_index}_core_0.IP_CH1,top_dcmac_${dcmac_index}_core_0.IP_CH2,top_dcmac_${dcmac_index}_core_0.IP_CH3 MSTRCLK 1,0,0,0 IS_CURRENT_QUAD ${q0}}"
          } else {
            set conf " ${oldCurInst}/dcmac_gt${idxii}_wrapper/${quad_name_ii} {${oldCurInst}/dcmac_gt${dcmac_index}_wrapper/${quad_name_ii}\
            top_dcmac_${dcmac_index}_core_0.IP_CH4,top_dcmac_${dcmac_index}_core_0.IP_CH5,top_dcmac_${dcmac_index}_core_0.IP_CH6,top_dcmac_${dcmac_index}_core_0.IP_CH7 MSTRCLK 1,0,0,0 IS_CURRENT_QUAD ${q0}}"
          }
          append quad_usage "${conf}"
        }
        append quad_usage "}"
    }

    set_property -dict [list \
    CONFIG.APB3_CLK_FREQUENCY {100.0} \
    CONFIG.CHANNEL_ORDERING {${channel_ordering}} \
    CONFIG.GT_TYPE {GTM} \
    CONFIG.PORTS_INFO_DICT {LANE_SEL_DICT {PROT0 {RX0 RX1 RX2 RX3 TX0 TX1 TX2 TX3}} GT_TYPE GTM REG_CONF_INTF APB3_INTF BOARD_PARAMETER { }} \
    CONFIG.PROT0_ENABLE {true} \
    CONFIG.PROT0_GT_DIRECTION {DUPLEX} \
    CONFIG.PROT0_LR0_SETTINGS {GT_DIRECTION DUPLEX TX_PAM_SEL PAM4 TX_HD_EN 0 TX_GRAY_BYP false TX_GRAY_LITTLEENDIAN false TX_PRECODE_BYP true TX_PRECODE_LITTLEENDIAN false TX_LINE_RATE 53.125 TX_PLL_TYPE\
LCPLL TX_REFCLK_FREQUENCY 322.265625 TX_ACTUAL_REFCLK_FREQUENCY 322.265625183611 TX_FRACN_ENABLED true TX_FRACN_OVRD false TX_FRACN_NUMERATOR 0 TX_REFCLK_SOURCE R0 TX_DATA_ENCODING RAW TX_USER_DATA_WIDTH\
160 TX_INT_DATA_WIDTH 128 TX_BUFFER_MODE 1 TX_BUFFER_BYPASS_MODE Fast_Sync TX_PIPM_ENABLE false TX_OUTCLK_SOURCE TXPROGDIVCLK TXPROGDIV_FREQ_ENABLE true TXPROGDIV_FREQ_SOURCE LCPLL TXPROGDIV_FREQ_VAL 664.062\
TX_DIFF_SWING_EMPH_MODE CUSTOM TX_64B66B_SCRAMBLER false TX_64B66B_ENCODER false TX_64B66B_CRC false TX_RATE_GROUP A TX_LANE_DESKEW_HDMI_ENABLE false TX_BUFFER_RESET_ON_RATE_CHANGE ENABLE PRESET GTM-PAM4_Ethernet_53G\
RX_PAM_SEL PAM4 RX_HD_EN 0 RX_GRAY_BYP false RX_GRAY_LITTLEENDIAN false RX_PRECODE_BYP true RX_PRECODE_LITTLEENDIAN false INTERNAL_PRESET PAM4_Ethernet_53G RX_LINE_RATE 53.125 RX_PLL_TYPE LCPLL RX_REFCLK_FREQUENCY\
322.265625 RX_ACTUAL_REFCLK_FREQUENCY 322.265625183611 RX_FRACN_ENABLED true RX_FRACN_OVRD false RX_FRACN_NUMERATOR 0 RX_REFCLK_SOURCE R0 RX_DATA_DECODING RAW RX_USER_DATA_WIDTH 160 RX_INT_DATA_WIDTH 128\
RX_BUFFER_MODE 1 RX_OUTCLK_SOURCE RXPROGDIVCLK RXPROGDIV_FREQ_ENABLE true RXPROGDIV_FREQ_SOURCE LCPLL RXPROGDIV_FREQ_VAL 664.062 RXRECCLK_FREQ_ENABLE false RXRECCLK_FREQ_VAL 0 INS_LOSS_NYQ 20 RX_EQ_MODE\
AUTO RX_COUPLING AC RX_TERMINATION VCOM_VREF RX_RATE_GROUP A RX_TERMINATION_PROG_VALUE 800 RX_PPM_OFFSET 200 RX_64B66B_DESCRAMBLER false RX_64B66B_DECODER false RX_64B66B_CRC false OOB_ENABLE false RX_COMMA_ALIGN_WORD\
1 RX_COMMA_SHOW_REALIGN_ENABLE true PCIE_ENABLE false RX_COMMA_P_ENABLE false RX_COMMA_M_ENABLE false RX_COMMA_DOUBLE_ENABLE false RX_COMMA_P_VAL 0101111100 RX_COMMA_M_VAL 1010000011 RX_COMMA_MASK 0000000000\
RX_SLIDE_MODE OFF RX_SSC_PPM 0 RX_CB_NUM_SEQ 0 RX_CB_LEN_SEQ 1 RX_CB_MAX_SKEW 1 RX_CB_MAX_LEVEL 1 RX_CB_MASK 00000000 RX_CB_VAL 00000000000000000000000000000000000000000000000000000000000000000000000000000000\
RX_CB_K 00000000 RX_CB_DISP 00000000 RX_CB_MASK_0_0 false RX_CB_VAL_0_0 0000000000 RX_CB_K_0_0 false RX_CB_DISP_0_0 false RX_CB_MASK_0_1 false RX_CB_VAL_0_1 0000000000 RX_CB_K_0_1 false RX_CB_DISP_0_1\
false RX_CB_MASK_0_2 false RX_CB_VAL_0_2 0000000000 RX_CB_K_0_2 false RX_CB_DISP_0_2 false RX_CB_MASK_0_3 false RX_CB_VAL_0_3 0000000000 RX_CB_K_0_3 false RX_CB_DISP_0_3 false RX_CB_MASK_1_0 false RX_CB_VAL_1_0\
0000000000 RX_CB_K_1_0 false RX_CB_DISP_1_0 false RX_CB_MASK_1_1 false RX_CB_VAL_1_1 0000000000 RX_CB_K_1_1 false RX_CB_DISP_1_1 false RX_CB_MASK_1_2 false RX_CB_VAL_1_2 0000000000 RX_CB_K_1_2 false RX_CB_DISP_1_2\
false RX_CB_MASK_1_3 false RX_CB_VAL_1_3 0000000000 RX_CB_K_1_3 false RX_CB_DISP_1_3 false RX_CC_NUM_SEQ 0 RX_CC_LEN_SEQ 1 RX_CC_PERIODICITY 5000 RX_CC_KEEP_IDLE DISABLE RX_CC_PRECEDENCE ENABLE RX_CC_REPEAT_WAIT\
0 RX_CC_MASK 00000000 RX_CC_VAL 00000000000000000000000000000000000000000000000000000000000000000000000000000000 RX_CC_K 00000000 RX_CC_DISP 00000000 RX_CC_MASK_0_0 false RX_CC_VAL_0_0 0000000000 RX_CC_K_0_0\
false RX_CC_DISP_0_0 false RX_CC_MASK_0_1 false RX_CC_VAL_0_1 0000000000 RX_CC_K_0_1 false RX_CC_DISP_0_1 false RX_CC_MASK_0_2 false RX_CC_VAL_0_2 0000000000 RX_CC_K_0_2 false RX_CC_DISP_0_2 false RX_CC_MASK_0_3\
false RX_CC_VAL_0_3 0000000000 RX_CC_K_0_3 false RX_CC_DISP_0_3 false RX_CC_MASK_1_0 false RX_CC_VAL_1_0 0000000000 RX_CC_K_1_0 false RX_CC_DISP_1_0 false RX_CC_MASK_1_1 false RX_CC_VAL_1_1 0000000000\
RX_CC_K_1_1 false RX_CC_DISP_1_1 false RX_CC_MASK_1_2 false RX_CC_VAL_1_2 0000000000 RX_CC_K_1_2 false RX_CC_DISP_1_2 false RX_CC_MASK_1_3 false RX_CC_VAL_1_3 0000000000 RX_CC_K_1_3 false RX_CC_DISP_1_3\
false PCIE_USERCLK2_FREQ 250 PCIE_USERCLK_FREQ 250 RX_JTOL_FC 10 RX_JTOL_LF_SLOPE -20 RX_BUFFER_BYPASS_MODE Fast_Sync RX_BUFFER_BYPASS_MODE_LANE MULTI RX_BUFFER_RESET_ON_CB_CHANGE ENABLE RX_BUFFER_RESET_ON_COMMAALIGN\
DISABLE RX_BUFFER_RESET_ON_RATE_CHANGE ENABLE RESET_SEQUENCE_INTERVAL 0 RX_COMMA_PRESET NONE RX_COMMA_VALID_ONLY 0 GT_TYPE GTM} \
    CONFIG.PROT0_LR10_SETTINGS {NA NA} \
    CONFIG.PROT0_LR11_SETTINGS {NA NA} \
    CONFIG.PROT0_LR12_SETTINGS {NA NA} \
    CONFIG.PROT0_LR13_SETTINGS {NA NA} \
    CONFIG.PROT0_LR14_SETTINGS {NA NA} \
    CONFIG.PROT0_LR15_SETTINGS {NA NA} \
    CONFIG.PROT0_LR1_SETTINGS {NA NA} \
    CONFIG.PROT0_LR2_SETTINGS {NA NA} \
    CONFIG.PROT0_LR3_SETTINGS {NA NA} \
    CONFIG.PROT0_LR4_SETTINGS {NA NA} \
    CONFIG.PROT0_LR5_SETTINGS {NA NA} \
    CONFIG.PROT0_LR6_SETTINGS {NA NA} \
    CONFIG.PROT0_LR7_SETTINGS {NA NA} \
    CONFIG.PROT0_LR8_SETTINGS {NA NA} \
    CONFIG.PROT0_LR9_SETTINGS {NA NA} \
    CONFIG.PROT0_NO_OF_LANES {4} \
    CONFIG.PROT0_RX_MASTERCLK_SRC {RX0} \
    CONFIG.PROT0_TX_MASTERCLK_SRC {TX0} \
    CONFIG.REFCLK_LIST {{/qsfp0_322mhz_clk_p[0]}} \
    CONFIG.REFCLK_STRING {HSCLK0_LCPLLGTREFCLK0 refclk_PROT0_R0_322.265625183611_MHz_unique1} \
    CONFIG.RX0_LANE_SEL {PROT0} \
    CONFIG.RX1_LANE_SEL {PROT0} \
    CONFIG.RX2_LANE_SEL {PROT0} \
    CONFIG.RX3_LANE_SEL {PROT0} \
    CONFIG.TX0_LANE_SEL {PROT0} \
    CONFIG.TX1_LANE_SEL {PROT0} \
    CONFIG.TX2_LANE_SEL {PROT0} \
    CONFIG.TX3_LANE_SEL {PROT0} \
    ] $gt_quad

    #CONFIG.QUAD_USAGE {${quad_usage}} \

    set_property -dict [list \
      CONFIG.APB3_CLK_FREQUENCY.VALUE_MODE {auto} \
      CONFIG.CHANNEL_ORDERING.VALUE_MODE {auto} \
      CONFIG.GT_TYPE.VALUE_MODE {auto} \
      CONFIG.PROT0_ENABLE.VALUE_MODE {auto} \
      CONFIG.PROT0_GT_DIRECTION.VALUE_MODE {auto} \
      CONFIG.PROT0_LR0_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR10_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR11_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR12_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR13_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR14_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR15_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR1_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR2_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR3_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR4_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR5_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR6_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR7_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR8_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_LR9_SETTINGS.VALUE_MODE {auto} \
      CONFIG.PROT0_NO_OF_LANES.VALUE_MODE {auto} \
      CONFIG.PROT0_RX_MASTERCLK_SRC.VALUE_MODE {auto} \
      CONFIG.PROT0_TX_MASTERCLK_SRC.VALUE_MODE {auto} \
      CONFIG.QUAD_USAGE.VALUE_MODE {auto} \
      CONFIG.REFCLK_LIST.VALUE_MODE {auto} \
      CONFIG.RX0_LANE_SEL.VALUE_MODE {auto} \
      CONFIG.RX1_LANE_SEL.VALUE_MODE {auto} \
      CONFIG.RX2_LANE_SEL.VALUE_MODE {auto} \
      CONFIG.RX3_LANE_SEL.VALUE_MODE {auto} \
      CONFIG.TX0_LANE_SEL.VALUE_MODE {auto} \
      CONFIG.TX1_LANE_SEL.VALUE_MODE {auto} \
      CONFIG.TX2_LANE_SEL.VALUE_MODE {auto} \
      CONFIG.TX3_LANE_SEL.VALUE_MODE {auto} \
    ] $gt_quad

  }

  # Create instance: xlconstant_0, and set properties
  set xlconstant_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant xlconstant_0 ]
  set_property -dict [list \
    CONFIG.CONST_VAL {1} \
    CONFIG.CONST_WIDTH {1} \
  ] $xlconstant_0

  # Create instance: bufg_gt_odiv2, and set properties
  set bufg_gt_odiv2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:bufg_gt bufg_gt_odiv2 ]

  # Create instance: util_ds_buf_mbufg_rx_0, and set properties
  set util_ds_buf_mbufg_rx_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_ds_buf util_ds_buf_mbufg_rx_0 ]
  set_property -dict [list \
    CONFIG.C_BUFG_GT_SYNC {true} \
    CONFIG.C_BUF_TYPE {MBUFG_GT} \
  ] $util_ds_buf_mbufg_rx_0


  # Create instance: util_ds_buf_mbufg_tx_0, and set properties
  set util_ds_buf_mbufg_tx_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_ds_buf util_ds_buf_mbufg_tx_0 ]
  set_property -dict [list \
    CONFIG.C_BUFGCE_DIV {1} \
    CONFIG.C_BUFG_GT_SYNC {true} \
    CONFIG.C_BUF_TYPE {MBUFG_GT} \
  ] $util_ds_buf_mbufg_tx_0


  # Create instance: xlslice_gt_txpostcursor, and set properties
  set xlslice_gt_txpostcursor [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_gt_txpostcursor ]
  set_property -dict [list \
    CONFIG.DIN_FROM {23} \
    CONFIG.DIN_TO {18} \
  ] $xlslice_gt_txpostcursor


  # Create instance: xlslice_gt_txprecursor, and set properties
  set xlslice_gt_txprecursor [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_gt_txprecursor ]
  set_property -dict [list \
    CONFIG.DIN_FROM {17} \
    CONFIG.DIN_TO {12} \
  ] $xlslice_gt_txprecursor


  # Create instance: xlslice_gt_txmaincursor, and set properties
  set xlslice_gt_txmaincursor [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_gt_txmaincursor ]
  set_property -dict [list \
    CONFIG.DIN_FROM {30} \
    CONFIG.DIN_TO {24} \
  ] $xlslice_gt_txmaincursor


  # Create instance: xlslice_gt_line_rate, and set properties
  set xlslice_gt_line_rate [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_gt_line_rate ]
  set_property -dict [list \
    CONFIG.DIN_FROM {8} \
    CONFIG.DIN_TO {1} \
  ] $xlslice_gt_line_rate


  # Create instance: xlslice_gt_rxcdrhold, and set properties
  set xlslice_gt_rxcdrhold [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice: xlslice_gt_rxcdrhold ]
  set_property -dict [list \
    CONFIG.DIN_FROM {31} \
    CONFIG.DIN_TO {31} \
  ] $xlslice_gt_rxcdrhold


  # Create instance: xlslice_gt_loopback, and set properties
  set xlslice_gt_loopback [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_gt_loopback ]
  set_property -dict [list \
    CONFIG.DIN_FROM {11} \
    CONFIG.DIN_TO {9} \
  ] $xlslice_gt_loopback


  # Create interface connections
  connect_bd_intf_net [get_bd_intf_pins qsfp_clk_322mhz] [get_bd_intf_pins util_ds_buf_0/CLK_IN_D1]

  # Create port connections
  connect_bd_net [get_bd_pins gt_control_pins] [get_bd_pins xlslice_gt_txpostcursor/Din] [get_bd_pins xlslice_gt_txprecursor/Din] [get_bd_pins xlslice_gt_txmaincursor/Din] [get_bd_pins xlslice_gt_line_rate/Din] [get_bd_pins xlslice_gt_rxcdrhold/Din] [get_bd_pins xlslice_gt_loopback/Din]
  connect_bd_net [get_bd_pins bufg_gt_odiv2/usrclk] [get_bd_pins GT0_ref_clk]
  connect_bd_net [get_bd_pins MBUFG_GT_CLR] [get_bd_pins util_ds_buf_mbufg_rx_0/MBUFG_GT_CLR]
  connect_bd_net [get_bd_pins MBUFG_GT_CLRB_LEAF] [get_bd_pins util_ds_buf_mbufg_rx_0/MBUFG_GT_CLRB_LEAF]
  connect_bd_net [get_bd_pins MBUFG_GT_CLR1] [get_bd_pins util_ds_buf_mbufg_tx_0/MBUFG_GT_CLR]
  connect_bd_net [get_bd_pins MBUFG_GT_CLRB_LEAF1] [get_bd_pins util_ds_buf_mbufg_tx_0/MBUFG_GT_CLRB_LEAF]
  connect_bd_net [get_bd_pins gt0_quad/ch0_rxoutclk] [get_bd_pins util_ds_buf_mbufg_rx_0/MBUFG_GT_I]
  connect_bd_net [get_bd_pins gt0_quad/ch0_txoutclk] [get_bd_pins util_ds_buf_mbufg_tx_0/MBUFG_GT_I]
  connect_bd_net [get_bd_pins util_ds_buf_0/IBUFDS_GTME5_ODIV2] [get_bd_pins bufg_gt_odiv2/outclk]
  connect_bd_net [get_bd_pins util_ds_buf_mbufg_rx_0/MBUFG_GT_O1] [get_bd_pins qsfp0_rx_usr_clk_664mhz]
  connect_bd_net [get_bd_pins util_ds_buf_mbufg_rx_0/MBUFG_GT_O2] [get_bd_pins qsfp0_rx_usr_clk_332mhz]
  connect_bd_net [get_bd_pins util_ds_buf_mbufg_tx_0/MBUFG_GT_O1] [get_bd_pins qsfp0_tx_usr_clk_664mhz]
  connect_bd_net [get_bd_pins util_ds_buf_mbufg_tx_0/MBUFG_GT_O2] [get_bd_pins qsfp0_tx_usr_clk_332mhz]
  connect_bd_net [get_bd_pins xlconstant_0/dout] [get_bd_pins util_ds_buf_mbufg_tx_0/MBUFG_GT_CE] [get_bd_pins util_ds_buf_mbufg_rx_0/MBUFG_GT_CE]

  foreach idx ${list_quad_index} {
    connect_bd_net [get_bd_pins apb3clk_quad] [get_bd_pins gt${idx}_quad/apb3clk]
    connect_bd_net [get_bd_pins gt${idx}_ch0_iloreset] [get_bd_pins gt${idx}_quad/ch0_iloreset]
    connect_bd_net [get_bd_pins gt${idx}_ch1_iloreset] [get_bd_pins gt${idx}_quad/ch1_iloreset]
    connect_bd_net [get_bd_pins gt${idx}_ch2_iloreset] [get_bd_pins gt${idx}_quad/ch2_iloreset]
    connect_bd_net [get_bd_pins gt${idx}_ch3_iloreset] [get_bd_pins gt${idx}_quad/ch3_iloreset]
    connect_bd_net [get_bd_pins hsclk_pllreset${idx}] [get_bd_pins gt${idx}_quad/hsclk1_lcpllreset] [get_bd_pins gt${idx}_quad/hsclk0_rpllreset] [get_bd_pins gt${idx}_quad/hsclk1_rpllreset] [get_bd_pins gt${idx}_quad/hsclk0_lcpllreset]

    connect_bd_net [get_bd_pins gt${idx}_quad/ch0_iloresetdone] [get_bd_pins gt${idx}_ch0_iloresetdone]
    connect_bd_net [get_bd_pins gt${idx}_quad/ch1_iloresetdone] [get_bd_pins gt${idx}_ch1_iloresetdone]
    connect_bd_net [get_bd_pins gt${idx}_quad/ch2_iloresetdone] [get_bd_pins gt${idx}_ch2_iloresetdone]
    connect_bd_net [get_bd_pins gt${idx}_quad/ch3_iloresetdone] [get_bd_pins gt${idx}_ch3_iloresetdone]
    connect_bd_net [get_bd_pins gt${idx}_quad/gtpowergood] [get_bd_pins gtpowergood_${idx}]
    connect_bd_net [get_bd_pins gt${idx}_quad/hsclk0_lcplllock] [get_bd_pins hsclk_plllock${idx}]
    connect_bd_net [get_bd_pins xlslice_gt_rxcdrhold/Dout] [get_bd_pins gt${idx}_quad/ch1_rxcdrhold] [get_bd_pins gt${idx}_quad/ch2_rxcdrhold] [get_bd_pins gt${idx}_quad/ch3_rxcdrhold] [get_bd_pins gt${idx}_quad/ch0_rxcdrhold]
    connect_bd_net [get_bd_pins xlslice_gt_txmaincursor/Dout] [get_bd_pins gt${idx}_quad/ch1_txmaincursor] [get_bd_pins gt${idx}_quad/ch2_txmaincursor] [get_bd_pins gt${idx}_quad/ch3_txmaincursor] [get_bd_pins gt${idx}_quad/ch0_txmaincursor]
    connect_bd_net [get_bd_pins xlslice_gt_txpostcursor/Dout] [get_bd_pins gt${idx}_quad/ch1_txpostcursor] [get_bd_pins gt${idx}_quad/ch2_txpostcursor] [get_bd_pins gt${idx}_quad/ch3_txpostcursor] [get_bd_pins gt${idx}_quad/ch0_txpostcursor]
    connect_bd_net [get_bd_pins xlslice_gt_txprecursor/Dout] [get_bd_pins gt${idx}_quad/ch1_txprecursor] [get_bd_pins gt${idx}_quad/ch2_txprecursor] [get_bd_pins gt${idx}_quad/ch3_txprecursor] [get_bd_pins gt${idx}_quad/ch0_txprecursor]
    connect_bd_net [get_bd_pins s_axi_aresetn] [get_bd_pins gt${idx}_quad/apb3presetn]
    connect_bd_net [get_bd_pins xlslice_gt_line_rate/Dout] [get_bd_pins gt${idx}_quad/ch0_rxrate] [get_bd_pins gt${idx}_quad/ch3_txrate] [get_bd_pins gt${idx}_quad/ch3_rxrate] [get_bd_pins gt${idx}_quad/ch2_txrate] [get_bd_pins gt${idx}_quad/ch2_rxrate] [get_bd_pins gt${idx}_quad/ch1_txrate] [get_bd_pins gt${idx}_quad/ch1_rxrate] [get_bd_pins gt${idx}_quad/ch0_txrate]
    connect_bd_net [get_bd_pins xlslice_gt_loopback/Dout] [get_bd_pins gt${idx}_quad/ch3_loopback] [get_bd_pins gt${idx}_quad/ch2_loopback] [get_bd_pins gt${idx}_quad/ch1_loopback] [get_bd_pins gt${idx}_quad/ch0_loopback]

    connect_bd_net [get_bd_pins util_ds_buf_mbufg_tx_0/MBUFG_GT_O2] [get_bd_pins gt${idx}_quad/ch0_txusrclk] [get_bd_pins gt${idx}_quad/ch1_txusrclk] [get_bd_pins gt${idx}_quad/ch2_txusrclk] [get_bd_pins gt${idx}_quad/ch3_txusrclk]
    connect_bd_net [get_bd_pins util_ds_buf_mbufg_rx_0/MBUFG_GT_O2] [get_bd_pins gt${idx}_quad/ch0_rxusrclk] [get_bd_pins gt${idx}_quad/ch1_rxusrclk] [get_bd_pins gt${idx}_quad/ch2_rxusrclk] [get_bd_pins gt${idx}_quad/ch3_rxusrclk]
    connect_bd_net [get_bd_pins util_ds_buf_0/IBUFDS_GTME5_O] [get_bd_pins gt${idx}_quad/GT_REFCLK0]

    connect_bd_intf_net [get_bd_intf_pins RX0_GT${idx}_IP_Interface] [get_bd_intf_pins gt${idx}_quad/RX0_GT_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins RX1_GT${idx}_IP_Interface] [get_bd_intf_pins gt${idx}_quad/RX1_GT_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins RX2_GT${idx}_IP_Interface] [get_bd_intf_pins gt${idx}_quad/RX2_GT_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins RX3_GT${idx}_IP_Interface] [get_bd_intf_pins gt${idx}_quad/RX3_GT_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins TX0_GT${idx}_IP_Interface] [get_bd_intf_pins gt${idx}_quad/TX0_GT_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins TX1_GT${idx}_IP_Interface] [get_bd_intf_pins gt${idx}_quad/TX1_GT_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins TX2_GT${idx}_IP_Interface] [get_bd_intf_pins gt${idx}_quad/TX2_GT_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins TX3_GT${idx}_IP_Interface] [get_bd_intf_pins gt${idx}_quad/TX3_GT_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins GT${idx}_Serial] [get_bd_intf_pins gt${idx}_quad/GT_Serial]
  }

  # Restore current instance
  current_bd_instance $oldCurInst
}


# Hierarchical cell: control_intf
proc create_hier_cell_control_intf { parentCell nameHier dual_dcmac} {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_control_intf() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S_AXI

  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M_AXI_DCMAC

  # Create pins
  create_bd_pin -dir I -type clk s_axi_aclk
  create_bd_pin -dir I -type clk clk_out_390
  create_bd_pin -dir I -type rst s_axi_aresetn
  create_bd_pin -dir O -from 31 -to 0 control_gt_rst
  create_bd_pin -dir O -from 31 -to 0 tx_datapath_ctrl
  create_bd_pin -dir O -from 31 -to 0 rx_datapath_ctrl
  create_bd_pin -dir O -from 31 -to 0 reset_txrx_path
  create_bd_pin -dir I -from 7 -to 0 gt0_tx_reset_done
  create_bd_pin -dir I -from 7 -to 0 gt0_rx_reset_done
  create_bd_pin -dir I -from 1 -to 0 gt0powergood

  # Create instance: smartconnect, and set properties
  set smartconnect [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect smartconnect ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_MI {5} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect

  # GT dynamic configuration parameters, setting up sensible values
  set txmaincursor 52
  set txprecursor 6
  set txpostcursor 6

  set gt_conf_value [format 0x%X [expr {(${txmaincursor} << 24) + (${txpostcursor} << 18) + (${txprecursor} << 12)}]]

  # Create instance: axi_gpio_gt_control, and set properties
  set axi_gpio_gt_control [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio axi_gpio_gt_control ]
  set_property -dict [list \
    CONFIG.C_ALL_OUTPUTS {1} \
    CONFIG.C_DOUT_DEFAULT ${gt_conf_value} \
    CONFIG.C_IS_DUAL {0} \
  ] $axi_gpio_gt_control

  # Create instance: axi_gpio_datapath, and set properties
  set axi_gpio_datapath [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio axi_gpio_datapath ]
  set_property -dict [list \
    CONFIG.C_ALL_OUTPUTS {1} \
    CONFIG.C_DOUT_DEFAULT {0x00000000} \
    CONFIG.C_ALL_OUTPUTS_2 {1} \
    CONFIG.C_IS_DUAL {1} \
    CONFIG.C_DOUT_DEFAULT_2 {0x00000000} \
  ] $axi_gpio_datapath

  # Create instance: axi_gpio_monitor, and set properties
  set axi_gpio_monitor [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio axi_gpio_monitor ]
  set_property -dict [list \
    CONFIG.C_ALL_INPUTS {1}
  ] $axi_gpio_monitor

  # Create instance: axi_gpio_reset_txrx, and set properties
  set axi_gpio_reset_txrx [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio axi_gpio_reset_txrx ]
  set_property -dict [list \
    CONFIG.C_ALL_OUTPUTS {1} \
    CONFIG.C_DOUT_DEFAULT {0x00000000} \
    CONFIG.C_IS_DUAL {0} \
  ] $axi_gpio_reset_txrx

  set xlconcat_monitor [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat xlconcat_monitor ]
  set_property -dict [list \
    CONFIG.IN0_WIDTH {8} \
    CONFIG.IN1_WIDTH {8} \
    CONFIG.IN2_WIDTH {2} \
    CONFIG.IN3_WIDTH {1} \
    CONFIG.NUM_PORTS {4} \
  ] $xlconcat_monitor

  set dualdcmac [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant dualdcmac ]
  set_property CONFIG.CONST_VAL {0} $dualdcmac

  if { ${dual_dcmac} == "1" } {
    set_property CONFIG.CONST_VAL {1} $dualdcmac
  }

  # Create interface connections
  connect_bd_intf_net -intf_net m_axi_0 [get_bd_intf_pins M_AXI_DCMAC] [get_bd_intf_pins smartconnect/M00_AXI]
  connect_bd_intf_net -intf_net m_axi_1 [get_bd_intf_pins smartconnect/M01_AXI] [get_bd_intf_pins axi_gpio_datapath/S_AXI]
  connect_bd_intf_net -intf_net m_axi_2 [get_bd_intf_pins smartconnect/M02_AXI] [get_bd_intf_pins axi_gpio_gt_control/S_AXI]
  connect_bd_intf_net -intf_net m_axi_3 [get_bd_intf_pins smartconnect/M03_AXI] [get_bd_intf_pins axi_gpio_monitor/S_AXI]
  connect_bd_intf_net -intf_net m_axi_4 [get_bd_intf_pins smartconnect/M04_AXI] [get_bd_intf_pins axi_gpio_reset_txrx/S_AXI]
  connect_bd_intf_net -intf_net s_axi_1 [get_bd_intf_pins S_AXI] [get_bd_intf_pins smartconnect/S00_AXI]

  # Create port connections
  connect_bd_net -net control_gt_rst_gpio_io_o [get_bd_pins axi_gpio_gt_control/gpio_io_o] [get_bd_pins control_gt_rst]
  connect_bd_net -net axi_gpio_datapath_gpio_io_o [get_bd_pins axi_gpio_datapath/gpio_io_o] [get_bd_pins rx_datapath_ctrl]
  connect_bd_net -net axi_gpio_datapath_gpio2_io_o [get_bd_pins axi_gpio_datapath/gpio2_io_o] [get_bd_pins tx_datapath_ctrl]
  connect_bd_net -net clk_wizard_0_clk_out2 [get_bd_pins clk_out_390] [get_bd_pins smartconnect/aclk1]
  connect_bd_net -net s_axi_aclk_1 [get_bd_pins s_axi_aclk] [get_bd_pins smartconnect/aclk] [get_bd_pins axi_gpio_datapath/s_axi_aclk] [get_bd_pins axi_gpio_monitor/s_axi_aclk] [get_bd_pins axi_gpio_gt_control/s_axi_aclk] [get_bd_pins axi_gpio_reset_txrx/s_axi_aclk]
  connect_bd_net -net s_axi_aresetn_1 [get_bd_pins s_axi_aresetn] [get_bd_pins smartconnect/aresetn] [get_bd_pins axi_gpio_datapath/s_axi_aresetn] [get_bd_pins axi_gpio_monitor/s_axi_aresetn] [get_bd_pins axi_gpio_gt_control/s_axi_aresetn] [get_bd_pins axi_gpio_reset_txrx/s_axi_aresetn]
  connect_bd_net -net qsfp_leds_gpio_io_o [get_bd_pins axi_gpio_reset_txrx/gpio_io_o] [get_bd_pins reset_txrx_path]

  connect_bd_net [get_bd_pins gt0_tx_reset_done] [get_bd_pins xlconcat_monitor/In0]
  connect_bd_net [get_bd_pins gt0_rx_reset_done] [get_bd_pins xlconcat_monitor/In1]
  connect_bd_net [get_bd_pins gt0powergood] [get_bd_pins xlconcat_monitor/In2]
  connect_bd_net [get_bd_pins xlconcat_monitor/dout] [get_bd_pins axi_gpio_monitor/gpio_io_i]
  connect_bd_net [get_bd_pins dualdcmac/dout] [get_bd_pins xlconcat_monitor/In3]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: DCMAC_subsys
proc create_hier_cell_DCMAC_subsys { parentCell nameHier dcmac_index dual_dcmac} {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_DCMAC_subsys() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 qsfp_clk_322mhz
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 s_axi
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp_gt0
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 M_AXIS_0
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 S_AXIS_0

  # Additional interfaces for dual DCMAC
  if { ${dual_dcmac} == "1" } {
    create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp_gt1
    create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 M_AXIS_1
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 S_AXIS_1
  }

  # Create pins
  create_bd_pin -dir I -from 31 -to 0 control_gt_rst
  create_bd_pin -dir I -from 31 -to 0 control_rx_datapath
  create_bd_pin -dir I -type clk axi_clk_390mhz
  create_bd_pin -dir I -type clk s_axi_aclk
  create_bd_pin -dir I -type rst s_axi_aresetn
  create_bd_pin -dir I -type clk core_clk_782mhz
  create_bd_pin -dir I -from 5 -to 0 -type clk ts_clk_bus_350mhz
  create_bd_pin -dir I -from 31 -to 0 control_tx_datapath
  create_bd_pin -dir O -from 7 -to 0 gt0_rx_reset_done
  create_bd_pin -dir O -from 7 -to 0 gt0_tx_reset_done
  create_bd_pin -dir I -type rst aresetn_rx_390mhz
  create_bd_pin -dir I -type rst aresetn_tx_390mhz
  create_bd_pin -dir O -type gt_usrclk GT0_ref_clk
  create_bd_pin -dir O -from 1 -to 0 gt0powergood

  # Create instance: xlslice_gt_reset, and set properties
  set xlslice_gt_reset [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_gt_reset ]
  set_property -dict [list \
    CONFIG.DIN_FROM {0} \
    CONFIG.DIN_TO {0} \
  ] $xlslice_gt_reset

  # Create instance: rx_alt_serdes, and set properties
  set rx_alt_serdes [create_bd_cell -type module -reference clock_to_serdes rx_alt_serdes]

  # Create instance: xlslice_rx_datapath_2, and set properties
  set xlslice_rx_datapath_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_rx_datapath_2 ]
  set_property -dict [list \
    CONFIG.DIN_FROM {2} \
    CONFIG.DIN_TO {2} \
  ] $xlslice_rx_datapath_2

  # Create instance: rx_flexif_clk_clock_bus, and set properties
  set rx_flexif_clk_clock_bus [create_bd_cell -type module -reference clock_to_clock_bus rx_flexif_clk_clock_bus]

  # Create instance: tx_alt_serdes, and set properties
  set tx_alt_serdes [create_bd_cell -type module -reference clock_to_serdes tx_alt_serdes]

  # Create instance: tx_serdes, and set properties
  set tx_serdes [create_bd_cell -type module -reference clock_to_serdes tx_serdes]

  set dcmac_name "dcmac_${dcmac_index}_core"

  if { ${dcmac_index} == "1" } {
    set dcmac_loc "DCMAC_X0Y2"
  } else {
    set dcmac_loc "DCMAC_X1Y1"
  }

  # Create instance: dcmac_core, and set properties
  set dcmac_core [ create_bd_cell -type ip -vlnv xilinx.com:ip:dcmac ${dcmac_name} ]

  set_property -dict [list \
    CONFIG.DCMAC_CONFIGURATION_TYPE {Static Configuration} \
    CONFIG.DCMAC_DATA_PATH_INTERFACE_C0 {391MHz Upto 6 Ports} \
    CONFIG.DCMAC_LOCATION_C0 $dcmac_loc \
    CONFIG.DCMAC_MODE_C0 {Coupled MAC+PCS} \
    CONFIG.FAST_SIM_MODE {0} \
    CONFIG.FEC_SLICE0_CFG_C0 {RS(544) CL119} \
    CONFIG.GT_PIPELINE_STAGES {7} \
    CONFIG.GT_REF_CLK_FREQ_C0 {322.265625} \
    CONFIG.GT_TYPE_C0 {GTM} \
    CONFIG.MAC_PORT0_CONFIG_C0 {200GAUI-4} \
    CONFIG.MAC_PORT0_ENABLE_C0 {1} \
    CONFIG.MAC_PORT0_ENABLE_TIME_STAMPING_C0 {0} \
    CONFIG.MAC_PORT0_RX_FLOW_C0 {0} \
    CONFIG.MAC_PORT0_RX_STRIP_C0 {1} \
    CONFIG.MAC_PORT0_TX_FLOW_C0 {0} \
    CONFIG.MAC_PORT0_TX_INSERT_C0 {1} \
    CONFIG.MAC_PORT1_ENABLE_C0 {1} \
    CONFIG.MAC_PORT1_RX_STRIP_C0 {1} \
    CONFIG.MAC_PORT2_ENABLE_C0 {0} \
    CONFIG.MAC_PORT3_ENABLE_C0 {0} \
    CONFIG.MAC_PORT4_ENABLE_C0 {0} \
    CONFIG.MAC_PORT5_ENABLE_C0 {0} \
    CONFIG.NUM_GT_CHANNELS {4} \
    CONFIG.PHY_OPERATING_MODE_C0 {N/A} \
    CONFIG.PORT0_1588v2_Clocking_C0 {Ordinary/Boundary Clock} \
    CONFIG.PORT0_1588v2_Operation_MODE_C0 {No operation} \
    CONFIG.PORT1_1588v2_Clocking_C0 {Ordinary/Boundary Clock} \
    CONFIG.PORT1_1588v2_Operation_MODE_C0 {No operation} \
    CONFIG.PORT2_1588v2_Clocking_C0 {Ordinary/Boundary Clock} \
    CONFIG.PORT2_1588v2_Operation_MODE_C0 {No operation} \
    CONFIG.PORT3_1588v2_Clocking_C0 {Ordinary/Boundary Clock} \
    CONFIG.PORT3_1588v2_Operation_MODE_C0 {No operation} \
    CONFIG.PORT4_1588v2_Clocking_C0 {Ordinary/Boundary Clock} \
    CONFIG.PORT4_1588v2_Operation_MODE_C0 {No operation} \
    CONFIG.PORT5_1588v2_Clocking_C0 {Ordinary/Boundary Clock} \
    CONFIG.PORT5_1588v2_Operation_MODE_C0 {No operation} \
    CONFIG.TIMESTAMP_CLK_PERIOD_NS {4.0000} \
  ] $dcmac_core

  if { ${dual_dcmac} == "1" } {
    set_property -dict [list \
      CONFIG.MAC_PORT2_ENABLE_C0 {1} \
      CONFIG.MAC_PORT2_RX_STRIP_C0 {1} \
      CONFIG.MAC_PORT3_ENABLE_C0 {1} \
      CONFIG.MAC_PORT3_RX_STRIP_C0 {1} \
    ] $dcmac_core
  }

  # Create instance: dcmac200g_ctl_port
  set dcmac200g_ctl_port [create_bd_cell -type module -reference dcmac200g_ctl_port dcmac200g_ctl_port]

  # Create instance: xlslice_tx_datapath_0, and set properties
  set xlslice_tx_datapath_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_tx_datapath_0 ]
  set_property -dict [list \
    CONFIG.DIN_FROM {0} \
    CONFIG.DIN_TO {0} \
  ] $xlslice_tx_datapath_0

  # Create instance: xlslice_tx_datapath_1, and set properties
  set xlslice_tx_datapath_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_tx_datapath_1 ]
  set_property -dict [list \
    CONFIG.DIN_FROM {1} \
    CONFIG.DIN_TO {1} \
  ] $xlslice_tx_datapath_1

  # Create instance: xlslice_tx_datapath_2, and set properties
  set xlslice_tx_datapath_2 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_tx_datapath_2 ]
  set_property -dict [list \
    CONFIG.DIN_FROM {2} \
    CONFIG.DIN_TO {2} \
  ] $xlslice_tx_datapath_2

  # Create instance: xlslice_rx_datapath_0, and set properties
  set xlslice_rx_datapath_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_rx_datapath_0 ]
  set_property -dict [list \
    CONFIG.DIN_FROM {0} \
    CONFIG.DIN_TO {0} \
  ] $xlslice_rx_datapath_0

  # Create instance: tx_flexif_clk_clock_bus, and set properties
  set tx_flexif_clk_clock_bus [create_bd_cell -type module -reference clock_to_clock_bus tx_flexif_clk_clock_bus]

  # Create instance: xlslice_tx_datapath_3, and set properties
  set xlslice_tx_datapath_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_tx_datapath_3 ]
  set_property -dict [list \
    CONFIG.DIN_FROM {3} \
    CONFIG.DIN_TO {3} \
  ] $xlslice_tx_datapath_3

  # Create instance: rx_serdes, and set properties
  set rx_serdes [create_bd_cell -type module -reference clock_to_serdes rx_serdes]

  # Create instance: xlslice_rx_datapath_1, and set properties
  set xlslice_rx_datapath_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_rx_datapath_1 ]
  set_property -dict [list \
    CONFIG.DIN_FROM {1} \
    CONFIG.DIN_TO {1} \
  ] $xlslice_rx_datapath_1

  # Create instance: xlslice_rx_datapath_3, and set properties
  set xlslice_rx_datapath_3 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_rx_datapath_3 ]
  set_property -dict [list \
    CONFIG.DIN_FROM {3} \
    CONFIG.DIN_TO {3} \
  ] $xlslice_rx_datapath_3

  # Create instance: gt0_rx_reset_done, and set properties
  set gt0_rx_reset_done [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat gt0_rx_reset_done ]
  set_property CONFIG.NUM_PORTS {4} $gt0_rx_reset_done

  # Create instance: gt0_tx_reset_done, and set properties
  set gt0_tx_reset_done [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat gt0_tx_reset_done ]
  set_property CONFIG.NUM_PORTS {4} $gt0_tx_reset_done

  set num_loops [expr {$dual_dcmac}]

  for {set i 0} {$i <= $num_loops} {incr i} {
    # Create instance: seg_to_axis, and set properties
    create_bd_cell -type module -reference axis_seg_to_unseg_converter "seg_to_axis${i}"
    # Create instance: axis_to_seg, and set properties
    create_bd_cell -type module -reference axis_unseg_to_seg_converter "axis_to_seg${i}"

    set_property CONFIG.FREQ_HZ 390998840 [get_bd_intf_pins "seg_to_axis${i}/m_axis0_pkt_out"]
    set_property CONFIG.FREQ_HZ 390998840 [get_bd_intf_pins "axis_to_seg${i}/s_axis0_pkt_in"]
  }

  # Create instance: dcmac_gt0_wrapper
  set dcmac_wrapper_name "dcmac_gt${dcmac_index}_wrapper"
  create_hier_cell_dcmac_gt_wrapper $hier_obj ${dcmac_wrapper_name} ${dcmac_index} ${dual_dcmac}

  # Create interface connections
  connect_bd_intf_net [get_bd_intf_pins ${dcmac_wrapper_name}/qsfp_clk_322mhz] [get_bd_intf_pins qsfp_clk_322mhz]
  connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_rx_serdes_interface_0] [get_bd_intf_pins ${dcmac_wrapper_name}/RX0_GT0_IP_Interface]
  connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_rx_serdes_interface_1] [get_bd_intf_pins ${dcmac_wrapper_name}/RX1_GT0_IP_Interface]
  connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_rx_serdes_interface_2] [get_bd_intf_pins ${dcmac_wrapper_name}/RX2_GT0_IP_Interface]
  connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_rx_serdes_interface_3] [get_bd_intf_pins ${dcmac_wrapper_name}/RX3_GT0_IP_Interface]
  connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_tx_serdes_interface_0] [get_bd_intf_pins ${dcmac_wrapper_name}/TX0_GT0_IP_Interface]
  connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_tx_serdes_interface_1] [get_bd_intf_pins ${dcmac_wrapper_name}/TX1_GT0_IP_Interface]
  connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_tx_serdes_interface_2] [get_bd_intf_pins ${dcmac_wrapper_name}/TX2_GT0_IP_Interface]
  connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_tx_serdes_interface_3] [get_bd_intf_pins ${dcmac_wrapper_name}/TX3_GT0_IP_Interface]
  connect_bd_intf_net [get_bd_intf_pins s_axi] [get_bd_intf_pins ${dcmac_name}/s_axi]

  if { ${dual_dcmac} == "1" } {
    connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_rx_serdes_interface_4] [get_bd_intf_pins ${dcmac_wrapper_name}/RX0_GT1_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_rx_serdes_interface_5] [get_bd_intf_pins ${dcmac_wrapper_name}/RX1_GT1_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_rx_serdes_interface_6] [get_bd_intf_pins ${dcmac_wrapper_name}/RX2_GT1_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_rx_serdes_interface_7] [get_bd_intf_pins ${dcmac_wrapper_name}/RX3_GT1_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_tx_serdes_interface_4] [get_bd_intf_pins ${dcmac_wrapper_name}/TX0_GT1_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_tx_serdes_interface_5] [get_bd_intf_pins ${dcmac_wrapper_name}/TX1_GT1_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_tx_serdes_interface_6] [get_bd_intf_pins ${dcmac_wrapper_name}/TX2_GT1_IP_Interface]
    connect_bd_intf_net [get_bd_intf_pins ${dcmac_name}/gtm_tx_serdes_interface_7] [get_bd_intf_pins ${dcmac_wrapper_name}/TX3_GT1_IP_Interface]
    connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/hsclk_plllock1] [get_bd_pins ${dcmac_name}/plllock_in_1]
    connect_bd_net [get_bd_pins ${dcmac_name}/pllreset_out_1] [get_bd_pins ${dcmac_wrapper_name}/hsclk_pllreset1]

    connect_bd_net [get_bd_pins ${dcmac_name}/iloreset_out_4] [get_bd_pins ${dcmac_wrapper_name}/gt1_ch0_iloreset]
    connect_bd_net [get_bd_pins ${dcmac_name}/iloreset_out_5] [get_bd_pins ${dcmac_wrapper_name}/gt1_ch1_iloreset]
    connect_bd_net [get_bd_pins ${dcmac_name}/iloreset_out_6] [get_bd_pins ${dcmac_wrapper_name}/gt1_ch2_iloreset]
    connect_bd_net [get_bd_pins ${dcmac_name}/iloreset_out_7] [get_bd_pins ${dcmac_wrapper_name}/gt1_ch3_iloreset]

    # We need to swap the GT connections for DCMAC1 to make sure the GT aligment is correct
    if { ${dcmac_index} == "1" } {
      connect_bd_intf_net [get_bd_intf_pins ${dcmac_wrapper_name}/GT1_Serial] [get_bd_intf_pins qsfp_gt0]
      connect_bd_intf_net [get_bd_intf_pins ${dcmac_wrapper_name}/GT0_Serial] [get_bd_intf_pins qsfp_gt1]

      connect_bd_intf_net [get_bd_intf_pins seg_to_axis1/m_axis0_pkt_out] [get_bd_intf_pins M_AXIS_0]
      connect_bd_intf_net [get_bd_intf_pins axis_to_seg1/s_axis0_pkt_in] [get_bd_intf_pins S_AXIS_0]
      connect_bd_intf_net [get_bd_intf_pins seg_to_axis0/m_axis0_pkt_out] [get_bd_intf_pins M_AXIS_1]
      connect_bd_intf_net [get_bd_intf_pins axis_to_seg0/s_axis0_pkt_in] [get_bd_intf_pins S_AXIS_1]
    } else {
      connect_bd_intf_net [get_bd_intf_pins ${dcmac_wrapper_name}/GT0_Serial] [get_bd_intf_pins qsfp_gt0]
      connect_bd_intf_net [get_bd_intf_pins ${dcmac_wrapper_name}/GT1_Serial] [get_bd_intf_pins qsfp_gt1]

      connect_bd_intf_net [get_bd_intf_pins seg_to_axis0/m_axis0_pkt_out] [get_bd_intf_pins M_AXIS_0]
      connect_bd_intf_net [get_bd_intf_pins axis_to_seg0/s_axis0_pkt_in] [get_bd_intf_pins S_AXIS_0]
      connect_bd_intf_net [get_bd_intf_pins seg_to_axis1/m_axis0_pkt_out] [get_bd_intf_pins M_AXIS_1]
      connect_bd_intf_net [get_bd_intf_pins axis_to_seg1/s_axis0_pkt_in] [get_bd_intf_pins S_AXIS_1]
    }
  } else {
    connect_bd_intf_net [get_bd_intf_pins ${dcmac_wrapper_name}/GT0_Serial] [get_bd_intf_pins qsfp_gt0]
    connect_bd_intf_net [get_bd_intf_pins seg_to_axis0/m_axis0_pkt_out] [get_bd_intf_pins M_AXIS_0]
    connect_bd_intf_net [get_bd_intf_pins axis_to_seg0/s_axis0_pkt_in] [get_bd_intf_pins S_AXIS_0]
  }

  # Create port connections
  connect_bd_net -net aresetn_axis_seg_in1_1 [get_bd_pins aresetn_tx_390mhz] [get_bd_pins axis_to_seg0/aresetn_axis_seg_in]
  connect_bd_net -net aresetn_axis_seg_in_1 [get_bd_pins aresetn_rx_390mhz] [get_bd_pins seg_to_axis0/aresetn_axis_seg_in]
  connect_bd_net -net axi_gpio_gt_control_gpio_io_o [get_bd_pins control_gt_rst] [get_bd_pins xlslice_gt_reset/Din] [get_bd_pins ${dcmac_wrapper_name}/gt_control_pins]
  connect_bd_net -net axi_gpio_rx_datapath_gpio_io_o [get_bd_pins control_rx_datapath] [get_bd_pins xlslice_rx_datapath_0/Din] [get_bd_pins xlslice_rx_datapath_1/Din] [get_bd_pins xlslice_rx_datapath_3/Din] [get_bd_pins xlslice_rx_datapath_2/Din]
  connect_bd_net -net axi_gpio_tx_datapath_gpio_io_o [get_bd_pins control_tx_datapath] [get_bd_pins xlslice_tx_datapath_1/Din] [get_bd_pins xlslice_tx_datapath_2/Din] [get_bd_pins xlslice_tx_datapath_3/Din] [get_bd_pins xlslice_tx_datapath_0/Din]
  connect_bd_net -net clk_wizard_0_clk_out1 [get_bd_pins core_clk_782mhz] [get_bd_pins ${dcmac_name}/tx_core_clk] [get_bd_pins ${dcmac_name}/rx_core_clk]
  connect_bd_net -net clk_wizard_0_clk_out2 [get_bd_pins axi_clk_390mhz] [get_bd_pins ${dcmac_name}/rx_axi_clk] [get_bd_pins ${dcmac_name}/tx_axi_clk] [get_bd_pins tx_flexif_clk_clock_bus/clk] [get_bd_pins ${dcmac_name}/rx_macif_clk] [get_bd_pins ${dcmac_name}/tx_macif_clk] [get_bd_pins rx_flexif_clk_clock_bus/clk] [get_bd_pins seg_to_axis0/aclk_axis_seg_in] [get_bd_pins axis_to_seg0/aclk_axis_seg_in]
  connect_bd_net [get_bd_pins ${dcmac_name}/iloreset_out_0] [get_bd_pins ${dcmac_wrapper_name}/gt0_ch0_iloreset]
  connect_bd_net [get_bd_pins ${dcmac_name}/iloreset_out_1] [get_bd_pins ${dcmac_wrapper_name}/gt0_ch1_iloreset]
  connect_bd_net [get_bd_pins ${dcmac_name}/iloreset_out_2] [get_bd_pins ${dcmac_wrapper_name}/gt0_ch2_iloreset]
  connect_bd_net [get_bd_pins ${dcmac_name}/iloreset_out_3] [get_bd_pins ${dcmac_wrapper_name}/gt0_ch3_iloreset]
  connect_bd_net [get_bd_pins ${dcmac_name}/pllreset_out_0] [get_bd_pins ${dcmac_wrapper_name}/hsclk_pllreset0]
  connect_bd_net [get_bd_pins ${dcmac_name}/rx_clr_out_0] [get_bd_pins ${dcmac_wrapper_name}/MBUFG_GT_CLR]
  connect_bd_net [get_bd_pins ${dcmac_name}/rx_clrb_leaf_out_0] [get_bd_pins ${dcmac_wrapper_name}/MBUFG_GT_CLRB_LEAF]
  connect_bd_net [get_bd_pins ${dcmac_name}/tx_clr_out_0] [get_bd_pins ${dcmac_wrapper_name}/MBUFG_GT_CLR1]
  connect_bd_net [get_bd_pins ${dcmac_name}/tx_clrb_leaf_out_0] [get_bd_pins ${dcmac_wrapper_name}/MBUFG_GT_CLRB_LEAF1]
  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/GT0_ref_clk] [get_bd_pins GT0_ref_clk]
  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/qsfp0_rx_usr_clk_332mhz] [get_bd_pins rx_alt_serdes/usrclk]
  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/qsfp0_rx_usr_clk_664mhz] [get_bd_pins rx_serdes/usrclk]
  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/qsfp0_tx_usr_clk_332mhz] [get_bd_pins tx_alt_serdes/usrclk]
  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/qsfp0_tx_usr_clk_664mhz] [get_bd_pins tx_serdes/usrclk]
  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/hsclk_plllock0] [get_bd_pins ${dcmac_name}/plllock_in_0]
  connect_bd_net -net gt_reset_rx_datapath_in_0_1 [get_bd_pins xlslice_rx_datapath_0/Dout] [get_bd_pins ${dcmac_name}/gt_reset_rx_datapath_in_0]
  connect_bd_net -net gt_reset_rx_datapath_in_1_1 [get_bd_pins xlslice_rx_datapath_1/Dout] [get_bd_pins ${dcmac_name}/gt_reset_rx_datapath_in_1]
  connect_bd_net -net gt_reset_rx_datapath_in_2_1 [get_bd_pins xlslice_rx_datapath_2/Dout] [get_bd_pins ${dcmac_name}/gt_reset_rx_datapath_in_2]
  connect_bd_net -net gt_reset_rx_datapath_in_3_1 [get_bd_pins xlslice_rx_datapath_3/Dout] [get_bd_pins ${dcmac_name}/gt_reset_rx_datapath_in_3]
  connect_bd_net -net gt_reset_tx_datapath_in_0_1 [get_bd_pins xlslice_tx_datapath_0/Dout] [get_bd_pins ${dcmac_name}/gt_reset_tx_datapath_in_0]
  connect_bd_net -net gt_reset_tx_datapath_in_1_1 [get_bd_pins xlslice_tx_datapath_1/Dout] [get_bd_pins ${dcmac_name}/gt_reset_tx_datapath_in_1]
  connect_bd_net -net gt_reset_tx_datapath_in_2_1 [get_bd_pins xlslice_tx_datapath_2/Dout] [get_bd_pins ${dcmac_name}/gt_reset_tx_datapath_in_2]
  connect_bd_net -net gt_reset_tx_datapath_in_3_1 [get_bd_pins xlslice_tx_datapath_3/Dout] [get_bd_pins ${dcmac_name}/gt_reset_tx_datapath_in_3]
  connect_bd_net -net gt0_rx_reset_done_dout [get_bd_pins gt0_rx_reset_done/dout] [get_bd_pins gt0_rx_reset_done]
  connect_bd_net -net gt0_tx_reset_done_dout [get_bd_pins gt0_tx_reset_done/dout] [get_bd_pins gt0_tx_reset_done]
  connect_bd_net -net rx_flexif_clk_clock_bus_clockbus [get_bd_pins rx_flexif_clk_clock_bus/clockbus] [get_bd_pins ${dcmac_name}/rx_flexif_clk]
  connect_bd_net -net rx_serdes_clk2_1 [get_bd_pins rx_alt_serdes/serdes_clk] [get_bd_pins ${dcmac_name}/rx_alt_serdes_clk]
  connect_bd_net -net rx_serdes_clk_1 [get_bd_pins rx_serdes/serdes_clk] [get_bd_pins ${dcmac_name}/rx_serdes_clk]
  connect_bd_net -net s_axi_aresetn_1 [get_bd_pins s_axi_aresetn] [get_bd_pins ${dcmac_name}/s_axi_aresetn] [get_bd_pins ${dcmac_wrapper_name}/s_axi_aresetn]
  connect_bd_net -net ts_clk_clk_clock_bus_clockbus [get_bd_pins ts_clk_bus_350mhz] [get_bd_pins ${dcmac_name}/ts_clk]
  connect_bd_net -net tx_flexif_clk_clock_bus_clockbus [get_bd_pins tx_flexif_clk_clock_bus/clockbus] [get_bd_pins ${dcmac_name}/tx_flexif_clk]
  connect_bd_net -net tx_serdes_clk2_1 [get_bd_pins tx_alt_serdes/serdes_clk] [get_bd_pins ${dcmac_name}/tx_alt_serdes_clk]
  connect_bd_net -net tx_serdes_clk_1 [get_bd_pins tx_serdes/serdes_clk] [get_bd_pins ${dcmac_name}/tx_serdes_clk]

  for {set i 0} {$i <= $num_loops} {incr i} {
    # AXI4 stream converter connections
    for {set lane 0} {$lane <= 3} {incr lane} {
      set lane_dcmac ${lane}
      if { ${i} == "1" } {
        set lane_dcmac "[expr {$lane + 4}]"

      }
      connect_bd_net [get_bd_pins axis_to_seg${i}/Unseg2SegEna${lane}_out] [get_bd_pins ${dcmac_name}/tx_axis_tuser_ena${lane_dcmac}]
      connect_bd_net [get_bd_pins axis_to_seg${i}/Unseg2SegDat${lane}_out] [get_bd_pins ${dcmac_name}/tx_axis_tdata${lane_dcmac}]
      connect_bd_net [get_bd_pins axis_to_seg${i}/Unseg2SegSop${lane}_out] [get_bd_pins ${dcmac_name}/tx_axis_tuser_sop${lane_dcmac}]
      connect_bd_net [get_bd_pins axis_to_seg${i}/Unseg2SegEop${lane}_out] [get_bd_pins ${dcmac_name}/tx_axis_tuser_eop${lane_dcmac}]
      connect_bd_net [get_bd_pins axis_to_seg${i}/Unseg2SegErr${lane}_out] [get_bd_pins ${dcmac_name}/tx_axis_tuser_err${lane_dcmac}]
      connect_bd_net [get_bd_pins axis_to_seg${i}/Unseg2SegMty${lane}_out] [get_bd_pins ${dcmac_name}/tx_axis_tuser_mty${lane_dcmac}]

      connect_bd_net [get_bd_pins ${dcmac_name}/rx_axis_tdata${lane_dcmac}] [get_bd_pins seg_to_axis${i}/Seg2UnSegDat${lane}_in]
      connect_bd_net [get_bd_pins ${dcmac_name}/rx_axis_tuser_ena${lane_dcmac}] [get_bd_pins seg_to_axis${i}/Seg2UnSegEna${lane}_in]
      connect_bd_net [get_bd_pins ${dcmac_name}/rx_axis_tuser_eop${lane_dcmac}] [get_bd_pins seg_to_axis${i}/Seg2UnSegEop${lane}_in]
      connect_bd_net [get_bd_pins ${dcmac_name}/rx_axis_tuser_err${lane_dcmac}] [get_bd_pins seg_to_axis${i}/Seg2UnSegErr${lane}_in]
      connect_bd_net [get_bd_pins ${dcmac_name}/rx_axis_tuser_mty${lane_dcmac}] [get_bd_pins seg_to_axis${i}/Seg2UnSegMty${lane}_in]
      connect_bd_net [get_bd_pins ${dcmac_name}/rx_axis_tuser_sop${lane_dcmac}] [get_bd_pins seg_to_axis${i}/Seg2UnSegSop${lane}_in]
      save_bd_design
    }
  }

  connect_bd_net [get_bd_pins ${dcmac_name}/rx_axis_tvalid_0] [get_bd_pins seg_to_axis0/rx_axis_tvalid_i]
  connect_bd_net [get_bd_pins ${dcmac_name}/tx_axis_tready_0] [get_bd_pins axis_to_seg0/tx_axis_tready_in]
  connect_bd_net [get_bd_pins axis_to_seg0/tx_axis_tvalid_out] [get_bd_pins ${dcmac_name}/tx_axis_tvalid_0]

  for {set lane 0} {$lane <= 3} {incr lane} {
    connect_bd_net [get_bd_pins ${dcmac_name}/gt_tx_reset_done_out_${lane}] [get_bd_pins gt0_tx_reset_done/In${lane}]
    connect_bd_net [get_bd_pins ${dcmac_name}/gt_rx_reset_done_out_${lane}] [get_bd_pins gt0_rx_reset_done/In${lane}]
  }

  for {set id 0} {$id <= 19} {incr id} {
    connect_bd_net [get_bd_pins dcmac200g_ctl_port/ctl_tx_vl_marker_id${id}] [get_bd_pins ${dcmac_name}/ctl_vl_marker_id${id}]
  }

  if { ${dual_dcmac} == "1" } {
    connect_bd_net [get_bd_pins aresetn_tx_390mhz] [get_bd_pins axis_to_seg1/aresetn_axis_seg_in]
    connect_bd_net [get_bd_pins aresetn_rx_390mhz] [get_bd_pins seg_to_axis1/aresetn_axis_seg_in]
    connect_bd_net [get_bd_pins axi_clk_390mhz] [get_bd_pins seg_to_axis1/aclk_axis_seg_in] [get_bd_pins axis_to_seg1/aclk_axis_seg_in]
    connect_bd_net [get_bd_pins ${dcmac_name}/rx_axis_tvalid_2] [get_bd_pins seg_to_axis1/rx_axis_tvalid_i]
    connect_bd_net [get_bd_pins ${dcmac_name}/tx_axis_tready_2] [get_bd_pins axis_to_seg1/tx_axis_tready_in]
    connect_bd_net [get_bd_pins axis_to_seg1/tx_axis_tvalid_out] [get_bd_pins ${dcmac_name}/tx_axis_tvalid_2]
    connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/gt1_ch0_iloresetdone] [get_bd_pins ${dcmac_name}/ilo_reset_done_4]
    connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/gt1_ch1_iloresetdone] [get_bd_pins ${dcmac_name}/ilo_reset_done_5]
    connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/gt1_ch2_iloresetdone] [get_bd_pins ${dcmac_name}/ilo_reset_done_6]
    connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/gt1_ch3_iloresetdone] [get_bd_pins ${dcmac_name}/ilo_reset_done_7]
  }

  connect_bd_net [get_bd_pins dcmac200g_ctl_port/default_vl_length_200GE_or_400GE] [get_bd_pins ${dcmac_name}/ctl_rx_custom_vl_length_minus1] [get_bd_pins ${dcmac_name}/ctl_tx_custom_vl_length_minus1]

  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/gtpowergood_0] [get_bd_pins ${dcmac_name}/gtpowergood_in] [get_bd_pins gt0powergood]
  connect_bd_net [get_bd_pins xlslice_gt_reset/Dout] [get_bd_pins ${dcmac_name}/gt_reset_all_in]
  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/gt0_ch0_iloresetdone] [get_bd_pins ${dcmac_name}/ilo_reset_done_0]
  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/gt0_ch1_iloresetdone] [get_bd_pins ${dcmac_name}/ilo_reset_done_1]
  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/gt0_ch2_iloresetdone] [get_bd_pins ${dcmac_name}/ilo_reset_done_2]
  connect_bd_net [get_bd_pins ${dcmac_wrapper_name}/gt0_ch3_iloresetdone] [get_bd_pins ${dcmac_name}/ilo_reset_done_3]

  connect_bd_net [get_bd_pins s_axi_aclk] [get_bd_pins ${dcmac_name}/s_axi_aclk] [get_bd_pins ${dcmac_wrapper_name}/apb3clk_quad]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: clk_n_resets
proc create_hier_cell_clk_n_resets { parentCell nameHier } {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_clk_n_resets() - Empty argument(s)!"}
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

  # Create pins
  create_bd_pin -dir O -type clk clk_out_390
  create_bd_pin -dir I -from 7 -to 0 gt0_tx_reset_done
  create_bd_pin -dir I -type clk gt_ref_clk_322mhz
  create_bd_pin -dir O -type clk clk_out_782
  create_bd_pin -dir I -type rst s_axi_aresetn
  create_bd_pin -dir O -from 0 -to 0 -type rst aresetn_tx_390mhz
  create_bd_pin -dir O -from 0 -to 0 -type rst aresetn_rx_390mhz
  create_bd_pin -dir I -from 7 -to 0 gt0_rx_reset_done
  create_bd_pin -dir O -from 5 -to 0 clockbus_350
  create_bd_pin -dir I -from 31 -to 0 reset_txrx_path

  # Create instance: syncer_tx_reset, and set properties
  set syncer_tx_reset [create_bd_cell -type module -reference dcmac_syncer_reset syncer_tx_reset]

  # Create instance: clk_wizard_0, and set properties
  set clk_wizard_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wizard clk_wizard_0 ]
  set_property -dict [list \
    CONFIG.CLKOUT_DRIVES {BUFG,BUFG,BUFG,BUFG,BUFG,BUFG,BUFG} \
    CONFIG.CLKOUT_DYN_PS {None,None,None,None,None,None,None} \
    CONFIG.CLKOUT_GROUPING {Auto,Auto,Auto,Auto,Auto,Auto,Auto} \
    CONFIG.CLKOUT_MATCHED_ROUTING {false,false,false,false,false,false,false} \
    CONFIG.CLKOUT_PORT {clk_out1,clk_out2,clk_out3,clk_out4,clk_out5,clk_out6,clk_out7} \
    CONFIG.CLKOUT_REQUESTED_DUTY_CYCLE {50.000,50.000,50.000,50.000,50.000,50.000,50.000} \
    CONFIG.CLKOUT_REQUESTED_OUT_FREQUENCY {782,390.625,350,100.000,100.000,100.000,100.000} \
    CONFIG.CLKOUT_REQUESTED_PHASE {0.000,0.000,0.000,0.000,0.000,0.000,0.000} \
    CONFIG.CLKOUT_USED {true,true,true,false,false,false,false} \
    CONFIG.OVERRIDE_PRIMITIVE {false} \
    CONFIG.PRIM_IN_FREQ {322.265625} \
    CONFIG.PRIM_SOURCE {Global_buffer} \
    CONFIG.USE_LOCKED {true} \
  ] $clk_wizard_0


  # Create instance: sys_reset_tx, and set properties
  set sys_reset_tx [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset sys_reset_tx ]

  # Create instance: sys_reset_rx, and set properties
  set sys_reset_rx [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset sys_reset_rx ]

  # Create instance: syncer_rx_reset, and set properties
  set syncer_rx_reset [create_bd_cell -type module -reference dcmac_syncer_reset syncer_rx_reset]

  # Create instance: ts_clk_clk_clock_bus, and set properties
  set ts_clk_clk_clock_bus [create_bd_cell -type module -reference clock_to_clock_bus ts_clk_clk_clock_bus]

  # Create instance: util_vector_logic_not, and set properties
  set util_vector_logic_not [ create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic util_vector_logic_not]
  set_property -dict [list \
    CONFIG.C_OPERATION {not} \
    CONFIG.C_SIZE {32} \
  ] $util_vector_logic_not

  set xlslice_reset_rx0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_reset_rx0 ]
  set_property  -dict [list \
    CONFIG.DIN_FROM {0} \
    CONFIG.DIN_TO {0} \
  ] $xlslice_reset_rx0

  set xlslice_reset_tx0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_reset_tx0 ]
  set_property  -dict [list \
    CONFIG.DIN_FROM {1} \
    CONFIG.DIN_TO {1} \
  ] $xlslice_reset_tx0

  set xlslice_reset_rx1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_reset_rx1 ]
  set_property  -dict [list \
    CONFIG.DIN_FROM {2} \
    CONFIG.DIN_TO {2} \
  ] $xlslice_reset_rx1

  set xlslice_reset_tx1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice xlslice_reset_tx1 ]
  set_property  -dict [list \
    CONFIG.DIN_FROM {3} \
    CONFIG.DIN_TO {3} \
  ] $xlslice_reset_tx1

  set xlconcat_rx0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat xlconcat_rx0 ]
  set_property -dict [list \
    CONFIG.IN0_WIDTH.VALUE_SRC USER \
    CONFIG.IN1_WIDTH.VALUE_SRC USER \
    CONFIG.IN0_WIDTH {4} \
    CONFIG.IN1_WIDTH {1} \
  ] $xlconcat_rx0

  set xlconcat_tx0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat xlconcat_tx0 ]
  set_property -dict [list \
    CONFIG.IN0_WIDTH.VALUE_SRC USER \
    CONFIG.IN1_WIDTH.VALUE_SRC USER \
    CONFIG.IN0_WIDTH {4} \
    CONFIG.IN1_WIDTH {1} \
  ] $xlconcat_tx0


  set and_reduced_rx [create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilreduced_logic and_reduced_rx]
  set_property -dict [list \
    CONFIG.C_SIZE {5} \
  ] $and_reduced_rx

  set and_reduced_tx [create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilreduced_logic and_reduced_tx]
  set_property -dict [list \
    CONFIG.C_SIZE {5} \
  ] $and_reduced_tx

  # Create port connections
  connect_bd_net -net clk_wizard_0_clk_out1 [get_bd_pins clk_wizard_0/clk_out1] [get_bd_pins clk_out_782]
  connect_bd_net -net clk_wizard_0_clk_out2 [get_bd_pins clk_wizard_0/clk_out2] [get_bd_pins clk_out_390] [get_bd_pins sys_reset_tx/slowest_sync_clk] [get_bd_pins sys_reset_rx/slowest_sync_clk] [get_bd_pins syncer_rx_reset/clk] [get_bd_pins syncer_tx_reset/clk]
  connect_bd_net -net clk_wizard_0_clk_out3 [get_bd_pins clk_wizard_0/clk_out3] [get_bd_pins ts_clk_clk_clock_bus/clk]
  connect_bd_net -net clk_wizard_0_locked [get_bd_pins clk_wizard_0/locked] [get_bd_pins syncer_rx_reset/clk_wizard_lock] [get_bd_pins syncer_tx_reset/clk_wizard_lock]
  connect_bd_net -net dcmac_0_gt_wrapper_IBUFDS_ODIV2 [get_bd_pins gt_ref_clk_322mhz] [get_bd_pins clk_wizard_0/clk_in1]
  connect_bd_net -net s_axi_aresetn_1 [get_bd_pins s_axi_aresetn] [get_bd_pins sys_reset_rx/aux_reset_in] [get_bd_pins sys_reset_tx/aux_reset_in]
  connect_bd_net -net syncer_rx_reset_resetn [get_bd_pins syncer_rx_reset/resetn] [get_bd_pins sys_reset_rx/ext_reset_in]
  connect_bd_net -net syncer_tx_reset_resetn [get_bd_pins syncer_tx_reset/resetn] [get_bd_pins sys_reset_tx/ext_reset_in]
  connect_bd_net -net sys_reset_rx_peripheral_aresetn [get_bd_pins sys_reset_rx/peripheral_aresetn] [get_bd_pins aresetn_rx_390mhz]
  connect_bd_net -net sys_reset_tx_peripheral_aresetn [get_bd_pins sys_reset_tx/peripheral_aresetn] [get_bd_pins aresetn_tx_390mhz]
  connect_bd_net -net ts_clk_clk_clock_bus_clockbus [get_bd_pins ts_clk_clk_clock_bus/clockbus] [get_bd_pins clockbus_350]

  connect_bd_net [get_bd_pins gt0_rx_reset_done] [get_bd_pins xlconcat_rx0/In0]
  connect_bd_net [get_bd_pins xlslice_reset_rx0/Dout] [get_bd_pins xlconcat_rx0/In1]
  connect_bd_net [get_bd_pins xlconcat_rx0/dout] [get_bd_pins and_reduced_rx/Op1]
  connect_bd_net [get_bd_pins and_reduced_rx/Res]  [get_bd_pins syncer_rx_reset/resetn_async]
  connect_bd_net [get_bd_pins gt0_tx_reset_done] [get_bd_pins xlconcat_tx0/In0]
  connect_bd_net [get_bd_pins xlslice_reset_tx0/Dout] [get_bd_pins xlconcat_tx0/In1]
  connect_bd_net [get_bd_pins xlconcat_tx0/dout] [get_bd_pins and_reduced_tx/Op1]
  connect_bd_net [get_bd_pins and_reduced_tx/Res] [get_bd_pins syncer_tx_reset/resetn_async]
  connect_bd_net [get_bd_pins reset_txrx_path] [get_bd_pins util_vector_logic_not/Op1]
  connect_bd_net [get_bd_pins util_vector_logic_not/Res] [get_bd_pins xlslice_reset_rx0/Din]
  connect_bd_net [get_bd_pins util_vector_logic_not/Res] [get_bd_pins xlslice_reset_rx1/Din]
  connect_bd_net [get_bd_pins util_vector_logic_not/Res] [get_bd_pins xlslice_reset_tx0/Din]
  connect_bd_net [get_bd_pins util_vector_logic_not/Res] [get_bd_pins xlslice_reset_tx1/Din]

  # Restore current instance
  current_bd_instance $oldCurInst
}

# Hierarchical cell: qsfp_0_n_1
proc create_hier_cell_qsfp { parentCell nameHier dcmac_index dual_dcmac} {

  variable script_folder

  if { $parentCell eq "" || $nameHier eq "" } {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "create_hier_cell_qsfp() - Empty argument(s)!"}
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
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 qsfp_clk_322mhz
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 s_axi
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp_gt0
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 M_AXIS_0
  create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 S_AXIS_0

  # Additional port for dual DCMAC
  if { ${dual_dcmac} == "1" } {
    create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 qsfp_gt1
    create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 M_AXIS_1
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 S_AXIS_1
  }

  # Create pins
  create_bd_pin -dir I -type clk ap_clk
  #create_bd_pin -dir I -type clk ap_clk_eth0
  create_bd_pin -dir I -type rst ap_rst_n

  set num_loops [expr {$dual_dcmac}]

  for {set i 0} {$i <= $num_loops} {incr i} {
    # Create instance: adwc0_512_1024, and set properties
    set adwc_512_1024 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_dwidth_converter "adwc${i}_512_1024" ]
    set_property -dict [list \
      CONFIG.HAS_TKEEP {1} \
      CONFIG.HAS_TLAST {1} \
      CONFIG.HAS_TSTRB {0} \
      CONFIG.M_TDATA_NUM_BYTES {128} \
      CONFIG.S_TDATA_NUM_BYTES {64} \
      CONFIG.TUSER_BITS_PER_BYTE {1} \
    ] $adwc_512_1024

    # Create instance: adwc0_1024_512, and set properties
    set adwc_1024_512 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_dwidth_converter "adwc${i}_1024_512" ]
    set_property -dict [list \
      CONFIG.HAS_TKEEP {1} \
      CONFIG.HAS_TLAST {1} \
      CONFIG.HAS_TSTRB {0} \
      CONFIG.M_TDATA_NUM_BYTES {64} \
      CONFIG.S_TDATA_NUM_BYTES {128} \
      CONFIG.TUSER_BITS_PER_BYTE {1} \
    ] $adwc_1024_512

    # Create instance: tx_packet_fifo_cdc, and set properties
    set tx_packet_fifo_cdc [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo "tx${i}_packet_fifo_cdc" ]
    set_property -dict [list \
      CONFIG.HAS_TLAST.VALUE_SRC USER \
      CONFIG.FIFO_DEPTH {512} \
      CONFIG.FIFO_MODE {2} \
      CONFIG.HAS_TKEEP {1} \
      CONFIG.HAS_TLAST {1} \
      CONFIG.TDATA_NUM_BYTES {64} \
      CONFIG.IS_ACLK_ASYNC {1} \
    ] $tx_packet_fifo_cdc

    # Create instance: rx_fifo_cdc, and set properties
    set rx_fifo_cdc [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo "rx${i}_fifo_cdc" ]
    set_property -dict [list \
      CONFIG.FIFO_DEPTH {128} \
      CONFIG.FIFO_MODE {1} \
      CONFIG.HAS_TKEEP {1} \
      CONFIG.HAS_TLAST {1} \
      CONFIG.TDATA_NUM_BYTES {64} \
      CONFIG.IS_ACLK_ASYNC {1} \
    ] $rx_fifo_cdc
  }
  # Create instance: clk_n_resets
  create_hier_cell_clk_n_resets $hier_obj clk_n_resets

  # Create instance: DCMAC_subsys
  create_hier_cell_DCMAC_subsys $hier_obj DCMAC_subsys ${dcmac_index} ${dual_dcmac}

  # Create instance: control_intf
  create_hier_cell_control_intf $hier_obj control_intf ${dual_dcmac}

  # Create interface connections
  connect_bd_intf_net -intf_net DCMAC_subsys_M_AXIS_0 [get_bd_intf_pins adwc0_1024_512/S_AXIS] [get_bd_intf_pins DCMAC_subsys/M_AXIS_0]
  connect_bd_intf_net -intf_net DCMAC_subsys_qsfp_gt [get_bd_intf_pins qsfp_gt0] [get_bd_intf_pins DCMAC_subsys/qsfp_gt0]
  connect_bd_intf_net -intf_net adwc0_1024_512_M_AXIS [get_bd_intf_pins adwc0_1024_512/M_AXIS] [get_bd_intf_pins rx0_fifo_cdc/S_AXIS]
  connect_bd_intf_net -intf_net rx0_fifo_cdc_M_AXIS [get_bd_intf_pins rx0_fifo_cdc/M_AXIS] [get_bd_intf_pins M_AXIS_0]
  connect_bd_intf_net -intf_net adwc0_512_1024_M_AXIS [get_bd_intf_pins adwc0_512_1024/M_AXIS] [get_bd_intf_pins DCMAC_subsys/S_AXIS_0]
  connect_bd_intf_net -intf_net m_axi_0 [get_bd_intf_pins control_intf/M_AXI_DCMAC] [get_bd_intf_pins DCMAC_subsys/s_axi]
  connect_bd_intf_net -intf_net packet_fifo_M_AXIS [get_bd_intf_pins tx0_packet_fifo_cdc/M_AXIS] [get_bd_intf_pins adwc0_512_1024/S_AXIS]
  connect_bd_intf_net -intf_net qsfp_clk_322mhz_1 [get_bd_intf_pins qsfp_clk_322mhz] [get_bd_intf_pins DCMAC_subsys/qsfp_clk_322mhz]
  connect_bd_intf_net -intf_net s_axi_1 [get_bd_intf_pins s_axi] [get_bd_intf_pins control_intf/S_AXI]
  connect_bd_intf_net -intf_net s_axi_1 [get_bd_intf_pins s_axi] [get_bd_intf_pins control_intf/S_AXI]
  connect_bd_intf_net [get_bd_intf_pins S_AXIS_0] [get_bd_intf_pins tx0_packet_fifo_cdc/S_AXIS]

  # Create port connections
  connect_bd_net -net axi_gpio_gt_control_gpio_io_o [get_bd_pins control_intf/control_gt_rst] [get_bd_pins DCMAC_subsys/control_gt_rst]
  connect_bd_net -net axi_gpio_rx_datapath_gpio_io_o [get_bd_pins control_intf/rx_datapath_ctrl] [get_bd_pins DCMAC_subsys/control_rx_datapath]
  connect_bd_net -net axi_gpio_tx_datapath_gpio_io_o [get_bd_pins control_intf/tx_datapath_ctrl] [get_bd_pins DCMAC_subsys/control_tx_datapath]
  connect_bd_net [get_bd_pins control_intf/gt0powergood] [get_bd_pins DCMAC_subsys/gt0powergood]
  connect_bd_net [get_bd_pins control_intf/reset_txrx_path] [get_bd_pins clk_n_resets/reset_txrx_path]

  connect_bd_net -net gt_ref_clk_322mhz_1 [get_bd_pins DCMAC_subsys/GT0_ref_clk] [get_bd_pins clk_n_resets/gt_ref_clk_322mhz]
  connect_bd_net -net clk_wizard_0_clk_out1 [get_bd_pins clk_n_resets/clk_out_782] [get_bd_pins DCMAC_subsys/core_clk_782mhz]
  connect_bd_net -net clk_wizard_0_clk_out2 [get_bd_pins clk_n_resets/clk_out_390] [get_bd_pins adwc0_512_1024/aclk] [get_bd_pins adwc0_1024_512/aclk] [get_bd_pins tx0_packet_fifo_cdc/m_axis_aclk] [get_bd_pins DCMAC_subsys/axi_clk_390mhz] [get_bd_pins control_intf/clk_out_390] [get_bd_pins rx0_fifo_cdc/s_axis_aclk]
  connect_bd_net -net gt0_rx_reset_done_dout [get_bd_pins DCMAC_subsys/gt0_rx_reset_done] [get_bd_pins clk_n_resets/gt0_rx_reset_done] [get_bd_pins control_intf/gt0_rx_reset_done]
  connect_bd_net -net gt0_tx_reset_done_dout [get_bd_pins DCMAC_subsys/gt0_tx_reset_done] [get_bd_pins clk_n_resets/gt0_tx_reset_done] [get_bd_pins control_intf/gt0_tx_reset_done]
  connect_bd_net -net s_axi_aclk_1 [get_bd_pins ap_clk] [get_bd_pins DCMAC_subsys/s_axi_aclk] [get_bd_pins control_intf/s_axi_aclk]
  connect_bd_net -net s_axi_aresetn_1 [get_bd_pins ap_rst_n] [get_bd_pins clk_n_resets/s_axi_aresetn] [get_bd_pins DCMAC_subsys/s_axi_aresetn] [get_bd_pins control_intf/s_axi_aresetn]
  connect_bd_net -net sys_reset_rx_peripheral_aresetn [get_bd_pins clk_n_resets/aresetn_rx_390mhz] [get_bd_pins DCMAC_subsys/aresetn_rx_390mhz] [get_bd_pins adwc0_1024_512/aresetn] [get_bd_pins rx0_fifo_cdc/s_axis_aresetn]
  connect_bd_net -net sys_reset_tx_peripheral_aresetn [get_bd_pins clk_n_resets/aresetn_tx_390mhz] [get_bd_pins adwc0_512_1024/aresetn] [get_bd_pins tx0_packet_fifo_cdc/s_axis_aresetn] [get_bd_pins DCMAC_subsys/aresetn_tx_390mhz]
  connect_bd_net -net ts_clk_clk_clock_bus_clockbus [get_bd_pins clk_n_resets/clockbus_350] [get_bd_pins DCMAC_subsys/ts_clk_bus_350mhz]

  connect_bd_net [get_bd_pins ap_clk] [get_bd_pins tx0_packet_fifo_cdc/s_axis_aclk] [get_bd_pins rx0_fifo_cdc/m_axis_aclk]

  if { ${dual_dcmac} == "1" } {
    connect_bd_intf_net [get_bd_intf_pins qsfp_gt1] [get_bd_intf_pins DCMAC_subsys/qsfp_gt1]
    connect_bd_intf_net [get_bd_intf_pins adwc1_1024_512/S_AXIS] [get_bd_intf_pins DCMAC_subsys/M_AXIS_1]
    connect_bd_intf_net [get_bd_intf_pins adwc1_512_1024/M_AXIS] [get_bd_intf_pins DCMAC_subsys/S_AXIS_1]
    connect_bd_intf_net [get_bd_intf_pins rx1_fifo_cdc/M_AXIS] [get_bd_intf_pins M_AXIS_1]
    connect_bd_intf_net [get_bd_intf_pins tx1_packet_fifo_cdc/M_AXIS] [get_bd_intf_pins adwc1_512_1024/S_AXIS]
    connect_bd_intf_net [get_bd_intf_pins adwc1_1024_512/M_AXIS] [get_bd_intf_pins rx1_fifo_cdc/S_AXIS]
    connect_bd_intf_net [get_bd_intf_pins S_AXIS_1] [get_bd_intf_pins tx1_packet_fifo_cdc/S_AXIS]

    connect_bd_net [get_bd_pins clk_n_resets/clk_out_390] [get_bd_pins adwc1_512_1024/aclk] [get_bd_pins adwc1_1024_512/aclk] [get_bd_pins tx1_packet_fifo_cdc/m_axis_aclk] [get_bd_pins rx1_fifo_cdc/s_axis_aclk]
    connect_bd_net [get_bd_pins ap_clk] [get_bd_pins tx1_packet_fifo_cdc/s_axis_aclk] [get_bd_pins rx1_fifo_cdc/m_axis_aclk]
    connect_bd_net [get_bd_pins clk_n_resets/aresetn_tx_390mhz] [get_bd_pins adwc1_512_1024/aresetn] [get_bd_pins tx1_packet_fifo_cdc/s_axis_aresetn]
    connect_bd_net [get_bd_pins clk_n_resets/aresetn_rx_390mhz] [get_bd_pins adwc1_1024_512/aresetn] [get_bd_pins rx1_fifo_cdc/s_axis_aresetn]
  }

  save_bd_design
  # Restore current instance
  current_bd_instance $oldCurInst
}

# Generic function that creates the qsfp block
proc create_qsfp_hierarchy { dcmac_index dual_dcmac} {

  if {![string is integer -strict $dcmac_index] || !($dcmac_index == 0 || $dcmac_index == 1)} {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "dcmac_index (with value $dcmac_index) is not correct. Valid values are 0 and 1"}
     return
  }

  if {![string is integer -strict $dual_dcmac] || !($dual_dcmac == 0 || $dual_dcmac == 1)} {
     catch {common::send_gid_msg -ssname BD::TCL -id 2092 -severity "ERROR" "dual_dcmac (with value $dual_dcmac) is not correct. Valid values are 0 or 1"}
     return
  }

  # TODO use dual_dcmac
  if { ${dcmac_index} == "0" } {
    set new_index $dcmac_index
    set offset_increment 0
  } else {
    set new_index "[expr {$dcmac_index + 1}]"
    set offset_increment 0x1000000
  }

  set qsfp_hier_name "qsfp_${new_index}_n_[expr {$new_index + 1}]"

  create_hier_cell_qsfp [current_bd_instance .] ${qsfp_hier_name} ${dcmac_index} ${dual_dcmac}
  save_bd_design

  set qsfp_gt0_4x [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 "qsfp${new_index}_4x" ]

  set qsfp_gt_clk_name [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 "qsfp${new_index}_322mhz" ]
  set_property -dict [ list \
    CONFIG.FREQ_HZ {322265625} \
  ] $qsfp_gt_clk_name
  save_bd_design

  connect_bd_intf_net [get_bd_intf_ports ${qsfp_gt0_4x}] [get_bd_intf_pins ${qsfp_hier_name}/qsfp_gt0]
  if { ${dual_dcmac} == "1" } {
    set qsfp_gt1_4x [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:gt_rtl:1.0 "qsfp[expr {$new_index + 1}]_4x" ]
    connect_bd_intf_net [get_bd_intf_ports ${qsfp_gt1_4x}] [get_bd_intf_pins ${qsfp_hier_name}/qsfp_gt1]
  }
  connect_bd_intf_net [get_bd_intf_ports ${qsfp_gt_clk_name}] [get_bd_intf_pins ${qsfp_hier_name}/qsfp_clk_322mhz]

  connect_bd_net [get_bd_pins cips/pl0_ref_clk] [get_bd_pins ${qsfp_hier_name}/ap_clk]
  connect_bd_net [get_bd_pins clock_reset/resetn_pl_ic] [get_bd_pins ${qsfp_hier_name}/ap_rst_n]
  save_bd_design

  # Manager ports are going to be inferred from M00_AXI through a xbar
  set num_managers [get_property CONFIG.NUM_MI [get_bd_cells bar_sc]]
  set new_num_managers "[expr {$num_managers + 1}]"
  set manager_port "M0${num_managers}_AXI"

  set_property -dict [list \
    CONFIG.NUM_MI ${new_num_managers} \
    CONFIG.NUM_SI {1} \
  ] [get_bd_cells bar_sc]

  save_bd_design

  connect_bd_intf_net [get_bd_intf_pins bar_sc/${manager_port}] [get_bd_intf_pins ${qsfp_hier_name}/s_axi]

  # Create address segments
  foreach pcie_noc {CPM_PCIE_NOC_0 CPM_PCIE_NOC_1} {
    assign_bd_address -offset [expr {0x020102000000 + ${offset_increment}}] -range 256K -target_address_space [get_bd_addr_spaces cips/${pcie_noc}] [get_bd_addr_segs /${qsfp_hier_name}/DCMAC_subsys/dcmac_${dcmac_index}_core/s_axi/Reg] -force
    assign_bd_address -offset [expr {0x020102040000 + ${offset_increment}}] -range 256 -target_address_space [get_bd_addr_spaces cips/${pcie_noc}] [get_bd_addr_segs /${qsfp_hier_name}/control_intf/axi_gpio_gt_control/S_AXI/Reg]  -force
    assign_bd_address -offset [expr {0x020102040200 + ${offset_increment}}] -range 256 -target_address_space [get_bd_addr_spaces cips/${pcie_noc}] [get_bd_addr_segs /${qsfp_hier_name}/control_intf/axi_gpio_monitor/S_AXI/Reg] -force
    assign_bd_address -offset [expr {0x020102040400 + ${offset_increment}}] -range 256 -target_address_space [get_bd_addr_spaces cips/${pcie_noc}] [get_bd_addr_segs /${qsfp_hier_name}/control_intf/axi_gpio_datapath/S_AXI/Reg] -force
    assign_bd_address -offset [expr {0x020102040600 + ${offset_increment}}] -range 256 -target_address_space [get_bd_addr_spaces cips/${pcie_noc}] [get_bd_addr_segs /${qsfp_hier_name}/control_intf/axi_gpio_reset_txrx/S_AXI/Reg] -force
    save_bd_design
  }

  save_bd_design
}

proc create_bar_sc { parentCell } {
  variable script_folder

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

  delete_bd_objs [get_bd_intf_nets axi_noc_cips_M00_AXI]
  set bar_sc [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 bar_sc ]
  set_property -dict [list \
    CONFIG.NUM_SI {1} \
  ] [get_bd_cells bar_sc]
  connect_bd_net [get_bd_pins cips/pl0_ref_clk] [get_bd_pins bar_sc/aclk]
  connect_bd_net [get_bd_pins clock_reset/resetn_pl_ic] [get_bd_pins bar_sc/aresetn]
  connect_bd_intf_net [get_bd_intf_pins bar_sc/S00_AXI] [get_bd_intf_pins axi_noc_cips/M00_AXI]
  connect_bd_intf_net [get_bd_intf_pins bar_sc/M00_AXI] [get_bd_intf_pins base_logic/s_axi_pcie_mgmt_slr0]

  # Fix PL AXILite aperture
  # Total of 256MB can be used, this command exposes all of it
  set_property -dict [ list \
   CONFIG.DATA_WIDTH {32} \
   CONFIG.APERTURES {{0x201_0000_0000 0x1000_0000}} \
   CONFIG.CATEGORY {pl} \
 ] [get_bd_intf_pins /axi_noc_cips/M00_AXI]
  current_bd_instance $oldCurInst
}

proc add_dcmac {} {
    source "dcmac_config.tcl"
    if { ${DCMAC0_ENABLED} == "1" || ${DCMAC1_ENABLED} == "1" } {
        create_bar_sc ""
    }
    # Create network hierarchy
    if { ${DCMAC0_ENABLED} == "1" } {
        create_qsfp_hierarchy 0 ${DUAL_QSFP_DCMAC0}
    }
    if { ${DCMAC1_ENABLED} == "1" } {
        create_qsfp_hierarchy 1 ${DUAL_QSFP_DCMAC1}
    }
}

add_dcmac