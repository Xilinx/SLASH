# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

registers = {
    'CONFIGURATION_REVISION': {'offset': 0x0, 'type': 'ro'},
    'GLOBAL_MODE': {
        'offset': 0x4, 'type': 'rw',
        'fields': {
            'ctl_tx_independent_tsmac_and_phy_mode': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_tx_all_ch_tick_reg_mode_sel': {'start': 1, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_rx_independent_tsmac_and_phy_mode': {'start': 4, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_rx_all_ch_tick_reg_mode_sel': {'start': 5, 'length': 1, 'default': '0', 'type': 'rw'},
            'ctl_tx_axis_cfg': {'start': 8, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_rx_axis_cfg': {'start': 12, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_tx_pcs_active_ports': {'start': 16, 'length': 3, 'default': 5, 'type': 'rw'},
            'ctl_rx_pcs_active_ports': {'start': 20, 'length': 3, 'default': 5, 'type': 'rw'},
            'ctl_rx_fec_errind_mode': {'start': 24, 'length': 1, 'default': 1, 'type': 'rw'},
            'ctl_tx_fec_ck_unique_flip': {'start': 25, 'length': 1, 'default': 1, 'type': 'rw'},
            'ctl_rx_fec_ck_unique_flip': {'start': 26, 'length': 1, 'default': 1, 'type': 'rw'}
        }
    },
    'TEST_DEBUG': {
        'offset': 0x8, 'type': 'rw',
        'fields': {
            'ctl_test_mode_pin_char': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_test_mode_memcel': {'start': 4, 'length': 4, 'default': 0, 'type': 'rw'},
            'ctl_rx_phy_debug_select': {'start': 8, 'length': 5, 'default': 0, 'type': 'rw'},
            'ctl_rx_mac_debug_select': {'start': 13, 'length': 4, 'default': 0, 'type': 'rw'},
            'ctl_tx_phy_debug_select': {'start': 17, 'length': 4, 'default': 0, 'type': 'rw'},
            'ctl_tx_mac_debug_select': {'start': 21, 'length': 4, 'default': 0, 'type': 'rw'},
            'ctl_rx_ecc_err_clear': {'start': 25, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_tx_ecc_err_clear': {'start': 26, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_tx_ecc_err_count_tick': {'start': 27, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_axi_af_thresh_override': {'start': 28, 'length': 4, 'default': 8, 'type': 'rw'}
        }
    },
    'EMA_CONFIGURATION': {
        'offset': 0xC, 'type': 'rw',
        'fields': {
            'ctl_mem_ctrl': {'start': 0, 'length': 10, 'default': 0x11b, 'type': 'rw'},
            'emaa': {'start': 0, 'length': 3, 'default': 0x3, 'type': 'rw'},
            'emab': {'start': 3, 'length': 3, 'default': 0x3, 'type': 'rw'},
            'emasa': {'start': 6, 'length': 1, 'default': 0x0, 'type': 'rw'},
            'stov': {'start': 7, 'length': 1, 'default': 0x0, 'type': 'rw'},
            'mc_mem_ctrl_enable': {'start': 8, 'length': 1, 'default': 0x1, 'type': 'rw'}
        }
    },
    'CLOCK_DISABLE': {
        'offset': 0x10, 'type': 'rw',
        'fields': {
            'ctl_mem_disable_rx_axi_clk': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_tx_axi_clk': {'start': 1, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_rx_macif_clk': {'start': 2, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_tx_macif_clk': {'start': 3, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_rx_core_clk': {'start': 4, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_tx_core_clk': {'start': 5, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_rx_flexif_clk': {'start': 6, 'length': 6, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_tx_flexif_clk': {'start': 12, 'length': 6, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_rx_serdes_clk': {'start': 18, 'length': 6, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_tx_serdes_clk': {'start': 24, 'length': 6, 'default': 0, 'type': 'rw'}
        }
    },
    'BLOCK_DISABLE': {
        'offset': 0x14, 'type': 'rw',
        'fields': {
            'ctl_mem_disable_rx_pcs_cpcs': {'start': 0, 'length': 6, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_rx_pcs_align_buffer': {'start': 6, 'length': 6, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_rx_pcs_decoder': {'start': 12, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_tx_pcs_cpcs': {'start': 16, 'length': 6, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_tx_ts2phy': {'start': 22, 'length': 1, 'default': 0, 'type': 'rw'},
            'ctl_mem_disable_tx_pcs_encoder': {'start': 23, 'length': 1, 'default': 0, 'type': 'rw'}
        }
    },
    'CONFIGURATION_EMPTY0': {
        'offset': 0x18, 'type': 'rw',
        'fields': {
            'ctl_rsvd0': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'CONFIGURATION_EMPTY1': {
        'offset': 0x1C, 'type': 'rw',
        'fields': {
            'ctl_rsvd1': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'CONFIGURATION_EMPTY2': {
        'offset': 0x20, 'type': 'rw',
        'fields': {
            'ctl_rsvd2': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'CONFIGURATION_EMPTY3': {
        'offset': 0x24, 'type': 'rw',
        'fields': {
            'ctl_rsvd3': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'CONFIGURATION_EMPTY4': {
        'offset': 0x28, 'type': 'rw',
        'fields': {
            'ctl_rsvd4': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'CONFIGURATION_EMPTY5': {
        'offset': 0x2C, 'type': 'rw',
        'fields': {
            'ctl_rsvd5': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'CONFIGURATION_EMPTY6': {
        'offset': 0x30, 'type': 'rw',
        'fields': {
            'ctl_rsvd6': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'CONFIGURATION_EMPTY7': {
        'offset': 0x34, 'type': 'rw',
        'fields': {
            'ctl_rsvd7': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'MAC_CONFIG_REG_TX_WR': {
        'offset': 0x38, 'type': 'rw',
        'fields': {
            'mac_tx_cfg_data': {'start': 0, 'length': 8, 'default': 0, 'type': 'rw'},
            'mac_tx_cfg_index': {'start': 8, 'length': 5, 'default': 0, 'type': 'rw'},
            'mac_tx_cfg_channel': {'start': 16, 'length': 6, 'default': 0, 'type': 'rw'},
            'mac_tx_cfg_wr': {'start': 24, 'length': 1, 'default': 0, 'type': 'rw'},
            'mac_tx_cfg_enable': {'start': 28, 'length': 1, 'default': 0, 'type': 'rw'}
        }
    },
    'MAC_CONFIG_REG_TX_RD': {
        'offset': 0x3C, 'type': 'rw',
        'fields': {
            'mac_tx_cfg_data_rd': {'start': 0, 'length': 8, 'default': 0, 'type': 'rw'}
        }
    },
    'GLOBAL_CONTROL_REG_RX': {
        'offset': 0xF0, 'type': 'rw',
        'fields': {
            'soft_rx_core_reset': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'},
            'soft_rx_macif_reset': {'start': 1, 'length': 1, 'default': 0, 'type': 'rw'},
            'soft_rx_axi_reset': {'start': 2, 'length': 1, 'default': 0, 'type': 'rw'},
        },
    },
    'ALL_CHANNEL_MAC_TICK_REG_RX': {
        'offset': 0xF4, 'type': 'rw',
        'fields': {
            'rx_all_channel_mac_soft_pm_tick': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'}
        },
    },
    'GLOBAL_CONTROL_REG_TX': {
        'offset': 0xF8, 'type': 'rw',
        'fields': {
            'soft_tx_core_reset': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'},
            'soft_tx_macif_reset': {'start': 1, 'length': 1, 'default': 0, 'type': 'rw'},
            'soft_tx_axi_reset': {'start': 2, 'length': 1, 'default': 0, 'type': 'rw'}
        },
    },
    'ALL_CHANNEL_MAC_TICK_REG_TX': {
        'offset': 0xFC, 'type': 'rw',
        'fields': {
            'rx_all_channel_mac_soft_pm_tick': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'}
        },
    },
    'STAT_TX_ECC_ERR_REG': {
        'offset': 0x1B0, 'type': 'ro',
        'fields': {
            'stat_tx_ecc0_err0': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'stat_tx_ecc0_err1': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'stat_tx_ecc1_err0': {'start': 2, 'length': 1, 'default': 0, 'type': 'ro'},
            'stat_tx_ecc1_err1': {'start': 3, 'length': 1, 'default': 0, 'type': 'ro'},
            'stat_tx_ecc2_err0': {'start': 4, 'length': 1, 'default': 0, 'type': 'ro'},
            'stat_tx_ecc2_err1': {'start': 5, 'length': 1, 'default': 0, 'type': 'ro'},
        },
    },
    'C0_CHANNEL_CONFIGURATION_TX': {
        'offset': 0x1000, 'type': 'rw',
        'fields': {
            'c0_ctl_tx_fcs_ins_enable': {'start': 0, 'length': 1, 'default': 1, 'type': 'rw'},
            'c0_ctl_tx_ignore_fcs': {'start': 1, 'length': 1, 'default': 1, 'type': 'rw'},
            'c0_ctl_tx_send_lfi': {'start': 2, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_send_rfi': {'start': 3, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_send_idle': {'start': 4, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_custom_preamble_enable': {'start': 5, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_ipg_value': {'start': 8, 'length': 4, 'default': 0xC, 'type': 'rw'},
            'c0_ctl_tx_corrupt_fcs_on_err': {'start': 16, 'length': 2, 'default': 0, 'type': 'rw'},
        }
    },
    'C0_CHANNEL_CONFIGURATION_RX': {
        'offset': 0x1004, 'type': 'rw',
        'fields': {
            'c0_ctl_rx_is_clause_49': {'start': 0, 'length': 1, 'default': 1, 'type': 'rw'},
            'c0_ctl_rx_delete_fcs': {'start': 1, 'length': 1, 'default': 1, 'type': 'rw'},
            'c0_ctl_rx_ignore_fcs': {'start': 2, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_process_lfi': {'start': 3, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_check_sfd': {'start': 4, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_check_preamble': {'start': 5, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_ignore_inrange': {'start': 6, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_max_packet_len': {'start': 16, 'length': 14, 'default': 0x2580, 'type': 'rw'},
        }
    },
    'C0_CHANNEL_CONTROL_REG_RX': {
        'offset': 0x1030, 'type': 'rw',
        'fields': {
            'c0_soft_rx_mac_channel_flush': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'}
        }
    },
    'C0_CHANNEL_CONTROL_REG_TX': {
        'offset': 0x1038, 'type': 'rw',
        'fields': {
            'c0_soft_tx_mac_channel_flush': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'}
        }
    },
    'C0_TX_MODE_REG': {
        'offset': 0x1040, 'type': 'rw',
        'fields': {
            'c0_ctl_tx_data_rate': {'start': 0, 'length': 2, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_use_custom_vl_length_minus1': {'start': 2, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_use_custom_vl_marker_ids': {'start': 3, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_tick_reg_mode_sel': {'start': 4, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_flexif_select': {'start': 5, 'length': 2, 'default': 1, 'type': 'rw'},
            'c0_ctl_tx_flexif_am_mode': {'start': 7, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_flexif_pcs_wide_mode': {'start': 8, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_pma_lane_mux': {'start': 9, 'length': 2, 'default': 1, 'type': 'rw'},
            'c0_ctl_tx_alt_serdes_clk_mux_disable': {'start': 11, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_fec_mode': {'start': 16, 'length': 5, 'default': 4, 'type': 'rw'},
            'c0_ctl_tx_fec_transcode_bypass': {'start': 21, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_tx_fec_four_lane_pmd': {'start': 22, 'length': 1, 'default': 0, 'type': 'rw'}
        }
    },
    'C0_RX_MODE_REG': {
        'offset': 0x1044, 'type': 'rw',
        'fields': {
            'c0_ctl_rx_data_rate': {'start': 0, 'length': 2, 'default': 0, 'type': 'rw'},
            'c0_ctl_pcs_rx_ts_en': {'start': 4, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_test_pattern': {'start': 8, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_use_custom_vl_length_minus1': {'start': 9, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_use_custom_vl_marker_ids': {'start': 10, 'length': 2, 'default': 1, 'type': 'rw'},
            'c0_ctl_rx_tick_reg_mode_sel': {'start': 11, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_pma_lane_mux': {'start': 12, 'length': 2, 'default': 1, 'type': 'rw'},
            'c0_ctl_rx_fec_mode': {'start': 16, 'length': 4, 'default': 4, 'type': 'rw'},
            'c0_ctl_rx_fec_bypass_indication': {'start': 21, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_fec_bypass_correction': {'start': 22, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_fec_transcode_clause49': {'start': 23, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_fec_alignment_bypass': {'start': 24, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_fec_transcode_bypass': {'start': 25, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_degrade_enable': {'start': 26, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_ctl_rx_fec_ext_align_buff_enable': {'start': 27, 'length': 1, 'default': 0, 'type': 'rw'}
        }
    },
    'C0_RX_FEC_SLICE_CONFIGURATION1': {
        'offset': 0x1048, 'type': 'rw',
        'fields': {
            'c0_ctl_rx_degrade_interval': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'C0_RX_FEC_SLICE_CONFIGURATION2': {
        'offset': 0x104C, 'type': 'rw',
        'fields': {
            'c0_ctl_rx_degrade_act_thresh': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'C0_RX_FEC_SLICE_CONFIGURATION3': {
        'offset': 0x1050, 'type': 'rw',
        'fields': {
            'c0_ctl_rx_degrade_deact_thresh': {'start': 0, 'length': 32, 'default': 0, 'type': 'rw'}
        }
    },
    'C0_CONFIGURATION_RX': {
        'offset': 0x10A0, 'type': 'rw',
        'fields': {
            'c0_ctl_rx_flexif_select': {'start': 0, 'length': 2, 'default': 1, 'type': 'rw'},
            'c0_ctl_rx_flexif_pcs_wide_mode': {'start': 2, 'length': 1, 'default': 0, 'type': 'rw'}
        }
    },
    'C0_PORT_CONTROL_REG_RX': {
        'offset': 0x10F0, 'type': 'rw',
        'fields': {
            'c0_soft_rx_flexif_reset': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'},
            'c0_soft_rx_serdes_reset': {'start': 1, 'length': 1, 'default': 0, 'type': 'rw'}
        }
    },
    'C0_PORT_TICK_REG_RX': {
        'offset': 0x10F4, 'type': 'rw',
        'fields': {
            'c0_rx_port_soft_pm_tick': {'start': 0, 'length': 1, 'default': 1, 'type': 'rw'}
        }
    },
    'C0_PORT_CONTROL_REG_TX': {
        'offset': 0x10F8, 'type': 'rw',
        'fields': {
            'c0_soft_tx_flexif_reset': {'start': 0, 'length': 1, 'default': 1, 'type': 'rw'},
            'c0_soft_tx_serdes_reset': {'start': 1, 'length': 1, 'default': 0, 'type': 'rw'}
        }
    },
    'C0_PORT_TICK_REG_TX': {
        'offset': 0x10FC, 'type': 'rw',
        'fields': {
            'c0_tx_port_soft_pm_tick': {'start': 0, 'length': 1, 'default': 1, 'type': 'rw'}
        }
    },
    'C0_STAT_CHAN_TX_MAC_STATUS_REG': {
        'offset': 0x1100, 'type': 'ro',
        'fields': {
            'c0_stat_tx_local_fault': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_tsmac_ovf': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_tsmac_unf': {'start': 2, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_packet_small': {'start': 3, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_sic_overflow': {'start': 4, 'length': 1, 'default': 0, 'type': 'ro'}
        }
    },
    'C0_STAT_CHAN_TX_MAC_RT_STATUS_REG': {
        'offset': 0x1104, 'type': 'ro',
        'fields': {
            'c0_stat_tx_local_fault': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_tsmac_ovf': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_tsmac_unf': {'start': 2, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_packet_small': {'start': 3, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_sic_overflow': {'start': 4, 'length': 1, 'default': 0, 'type': 'ro'}
        }
    },
    'C0_STAT_CHAN_TX_STATISTICS_READY': {
        'offset': 0x1108, 'type': 'ro',
        'fields': {
            'c0_stat_tx_channel_mac_statistics_ready': {'start': 0, 'length': 1, 'default': 1, 'type': 'ro'}
        }
    },
    'C0_STAT_CHAN_RX_MAC_STATUS_REG': {
        'offset': 0x1140, 'type': 'ro',
        'fields': {
            'c0_stat_rx_remote_fault': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_local_fault': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_internal_local_fault': {'start': 2, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_received_local_fault': {'start': 3, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_bad_preamble': {'start': 4, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_bad_sfd': {'start': 5, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_got_signal_os': {'start': 6, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_invalid_start': {'start': 7, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_bad_code': {'start': 8, 'length': 1, 'default': 0, 'type': 'ro'},
        }
    },
    'C0_STAT_CHAN_RX_MAC_RT_STATUS_REG': {
        'offset': 0x1144, 'type': 'ro',
        'fields': {
            'c0_stat_rx_remote_fault': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_local_fault': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_internal_local_fault': {'start': 2, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_received_local_fault': {'start': 3, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_bad_preamble': {'start': 4, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_bad_sfd': {'start': 5, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_got_signal_os': {'start': 6, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_invalid_start': {'start': 7, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_bad_code': {'start': 8, 'length': 1, 'default': 0, 'type': 'ro'},
        }
    },
    'C0_STAT_CHAN_RX_STATISTICS_READY': {
        'offset': 0x1148, 'type': 'ro',
        'fields': {
            'c0_stat_tx_channel_mac_statistics_ready': {'start': 0, 'length': 1, 'default': 1, 'type': 'ro'}
        }
    },

    'C0_STAT_PORT_TX_MAC_STATUS_REG': {
        'offset': 0x1180, 'type': 'ro',
        'fields': {
            'c0_stat_tx_axis_unf': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_axis_err': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'}
        }
    },
    'C0_STAT_PORT_TX_MAC_RT_STATUS_REG': {
        'offset': 0x1184, 'type': 'ro',
        'fields': {
            'c0_stat_tx_axis_unf': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_axis_err': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'}
        }
    },
    'C0_STAT_PORT_RX_MAC_STATUS_REG': {
        'offset': 0x11C0, 'type': 'ro',
        'fields': {
            'c0_stat_rx_axis_fifo_overflow': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_axis_err': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_phy2ts_buf_err': {'start': 2, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_macif_fifo_ovf': {'start': 3, 'length': 1, 'default': 0, 'type': 'ro'}
        }
    },
    'C0_STAT_PORT_RX_MAC_RT_STATUS_REG': {
        'offset': 0x11C4, 'type': 'ro',
        'fields': {
            'c0_stat_rx_axis_fifo_overflow': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_axis_err': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_phy2ts_buf_err': {'start': 2, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_macif_fifo_ovf': {'start': 3, 'length': 1, 'default': 0, 'type': 'ro'}
        }
    },
    'C0_STAT_PORT_TX_PHY_STATUS_REG': {
        'offset': 0x1800, 'type': 'ro',
        'fields': {
            'c0_stat_tx_pcs_bad_code': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_flex_fifo_err': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_flex_coa': {'start': 2, 'length': 1, 'default': 0, 'type': 'ro'}
        }
    },
    'C0_STAT_PORT_TX_PHY_RT_STATUS_REG': {
        'offset': 0x1804, 'type': 'ro',
        'fields': {
            'c0_stat_tx_pcs_bad_code': {'start': 0, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_flex_fifo_err': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_tx_flex_coa': {'start': 2, 'length': 1, 'default': 0, 'type': 'ro'}
        }
    },
    'C0_STAT_PORT_TX_STATISTICS_READY': {
        'offset': 0x1808, 'type': 'ro'
    },
    'C0_STAT_PORT_TX_FEC_STATUS_REG': {
        'offset': 0x180C, 'type': 'ro',
        'fields': {
            'c0_stat_tx_fec_pcs_lane_align': {'start': 0, 'length': 1, 'default': 1, 'type': 'ro'},
            'c0_stat_tx_fec_pcs_block_lock': {'start': 1, 'length': 1, 'default': 1, 'type': 'ro'},
            'c0_stat_tx_fec_pcs_am_lock': {'start': 2, 'length': 1, 'default': 1, 'type': 'ro'}
        }
    },
    'C0_STAT_PORT_TX_FEC_RT_STATUS_REG': {
        'offset': 0x1810, 'type': 'ro',
        'fields': {
            'c0_stat_tx_fec_pcs_lane_align': {'start': 0, 'length': 1, 'default': 1, 'type': 'ro'},
            'c0_stat_tx_fec_pcs_block_lock': {'start': 1, 'length': 1, 'default': 1, 'type': 'ro'},
            'c0_stat_tx_fec_pcs_am_lock': {'start': 2, 'length': 1, 'default': 1, 'type': 'ro'}
        }
    },
    'C0_STAT_PORT_RX_PHY_STATUS_REG': {
        'offset': 0x1C00, 'type': 'ro',
        'fields': {
            'c0_stat_rx_status': {'start': 0, 'length': 1, 'default': 1, 'type': 'ro'},
            'c0_stat_rx_block_lock': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_aligned': {'start': 2, 'length': 1, 'default': 1, 'type': 'ro'},
            'c0_stat_rx_misaligned': {'start': 3, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_aligned_err': {'start': 4, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_hi_ber': {'start': 5, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_framing_err': {'start': 6, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_pcs_bad_code': {'start': 7, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_synced': {'start': 8, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_synced_err': {'start': 9, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_bip_err': {'start': 10, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_flex_fifo_err': {'start': 11, 'length': 1, 'default': 0, 'type': 'ro'},
        }
    },
    'C0_STAT_PORT_RX_PHY_RT_STATUS_REG': {
        'offset': 0x1C04, 'type': 'ro',
        'fields': {
            'c0_stat_rx_status': {'start': 0, 'length': 1, 'default': 1, 'type': 'ro'},
            'c0_stat_rx_block_lock': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_aligned': {'start': 2, 'length': 1, 'default': 1, 'type': 'ro'},
            'c0_stat_rx_misaligned': {'start': 3, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_aligned_err': {'start': 4, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_hi_ber': {'start': 5, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_framing_err': {'start': 6, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_pcs_bad_code': {'start': 7, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_synced': {'start': 8, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_synced_err': {'start': 9, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_bip_err': {'start': 10, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_flex_fifo_err': {'start': 11, 'length': 1, 'default': 0, 'type': 'ro'},
        }
    },
    'C0_STAT_PORT_RX_STATISTICS_READY': {
        'offset': 0x1C08, 'type': 'ro'
    },
    'C0_STAT_PORT_RX_BLOCK_LOCK_REG': {
        'offset': 0x1C0C, 'type': 'ro'
    },
    'C0_STAT_PORT_RX_LANE_SYNC_REG': {
        'offset': 0x1C10, 'type': 'ro'
    },
    'C0_STAT_PORT_RX_LANE_SYNC_ERR_REG': {
        'offset': 0x1C14, 'type': 'ro'
    },

    'C0_STAT_PORT_RX_FEC_STATUS_REG': {
        'offset': 0x1C34, 'type': 'ro',
        'fields': {
            'c0_stat_rx_fec_aligned': {'start': 0, 'length': 1, 'default': 1, 'type': 'ro'},
            'c0_stat_rx_fec_hi_ser': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_fec_lane_lock': {'start': 2, 'length': 4, 'default': 15, 'type': 'ro'},
            'c0_stat_rx_fec_degraded_ser': {'start': 6, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_fec_rm_degraded': {'start': 7, 'length': 1, 'default': 0, 'type': 'ro'}
        }
    },
    'C0_STAT_PORT_RX_FEC_RT_STATUS_REG': {
        'offset': 0x1C38, 'type': 'ro',
        'fields': {
            'c0_stat_rx_fec_aligned': {'start': 0, 'length': 1, 'default': 1, 'type': 'ro'},
            'c0_stat_rx_fec_hi_ser': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_fec_lane_lock': {'start': 2, 'length': 4, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_fec_degraded_ser': {'start': 6, 'length': 1, 'default': 0, 'type': 'ro'},
            'c0_stat_rx_fec_rm_degraded': {'start': 7, 'length': 1, 'default': 0, 'type': 'ro'}
        }
    },
}


tx_stats_base_reg = {
    'TOTAL_BYTES_LSB': {'offset': 0x00, 'type': 'ro'},
    'TOTAL_BYTES_MSB': {'offset': 0x04, 'type': 'ro'},
    'TOTAL_GOOD_BYTES_LSB': {'offset': 0x08, 'type': 'ro'},
    'TOTAL_GOOD_BYTES_MSB': {'offset': 0x0C, 'type': 'ro'},
    'TOTAL_PACKETS_LSB': {'offset': 0x10, 'type': 'ro'},
    'TOTAL_PACKETS_MSB': {'offset': 0x14, 'type': 'ro'},
    'TOTAL_GOOD_PACKETS_LSB': {'offset': 0x18, 'type': 'ro'},
    'TOTAL_GOOD_PACKETS_MSB': {'offset': 0x1C, 'type': 'ro'},
    'FRAME_ERROR_LSB': {'offset': 0x20, 'type': 'ro'},
    'FRAME_ERROR_MSB': {'offset': 0x24, 'type': 'ro'},
    'BAD_FCS_LSB': {'offset': 0x28, 'type': 'ro'},
    'BAD_FCS_MSB': {'offset': 0x2C, 'type': 'ro'},
    'PACKET_64_BYTES_LSB': {'offset': 0x30, 'type': 'ro'},
    'PACKET_64_BYTES_MSB': {'offset': 0x34, 'type': 'ro'},
    'PACKET_65_127_BYTES_LSB': {'offset': 0x38, 'type': 'ro'},
    'PACKET_65_127_BYTES_MSB': {'offset': 0x3C, 'type': 'ro'},
    'PACKET_128_255_BYTES_LSB': {'offset': 0x40, 'type': 'ro'},
    'PACKET_128_255_BYTES_MSB': {'offset': 0x44, 'type': 'ro'},
    'PACKET_256_511_BYTES_LSB': {'offset': 0x48, 'type': 'ro'},
    'PACKET_256_511_BYTES_MSB': {'offset': 0x4C, 'type': 'ro'},
    'PACKET_512_1023_BYTES_LSB': {'offset': 0x50, 'type': 'ro'},
    'PACKET_512_1023_BYTES_MSB': {'offset': 0x54, 'type': 'ro'},
    'PACKET_1024_1518_BYTES_LSB': {'offset': 0x58, 'type': 'ro'},
    'PACKET_1024_1518_BYTES_MSB': {'offset': 0x5C, 'type': 'ro'},
    'PACKET_1519_1522_BYTES_LSB': {'offset': 0x60, 'type': 'ro'},
    'PACKET_1519_1522_BYTES_MSB': {'offset': 0x64, 'type': 'ro'},
    'PACKET_1523_1548_BYTES_LSB': {'offset': 0x68, 'type': 'ro'},
    'PACKET_1523_1548_BYTES_MSB': {'offset': 0x6C, 'type': 'ro'},
    'PACKET_1549_2047_BYTES_LSB': {'offset': 0x70, 'type': 'ro'},
    'PACKET_1549_2047_BYTES_MSB': {'offset': 0x74, 'type': 'ro'},
    'PACKET_2048_4095_BYTES_LSB': {'offset': 0x78, 'type': 'ro'},
    'PACKET_2048_4095_BYTES_MSB': {'offset': 0x7C, 'type': 'ro'},
    'PACKET_4096_8191_BYTES_LSB': {'offset': 0x80, 'type': 'ro'},
    'PACKET_4096_8191_BYTES_MSB': {'offset': 0x84, 'type': 'ro'},
    'PACKET_8192_9215_BYTES_LSB': {'offset': 0x88, 'type': 'ro'},
    'PACKET_8192_9215_BYTES_MSB': {'offset': 0x8C, 'type': 'ro'},
    'PACKET_LARGE': {'offset': 0x90, 'type': 'ro'},
    'UNICAST_LSB': {'offset': 0x98, 'type': 'ro'},
    'UNICAST_MSB': {'offset': 0x9C, 'type': 'ro'},
    'MULTICAST_LSB': {'offset': 0xA0, 'type': 'ro'},
    'MULTICAST_MSB': {'offset': 0xA4, 'type': 'ro'},
    'BROADCAST_LSB': {'offset': 0xA8, 'type': 'ro'},
    'BROADCAST_MSB': {'offset': 0xAC, 'type': 'ro'},
    'VLAN_LSB': {'offset': 0xB0, 'type': 'ro'},
    'VLAN_MSB': {'offset': 0xB4, 'type': 'ro'},
    'PAUSE_LSB': {'offset': 0xB8, 'type': 'ro'},
    'PAUSE_MSB': {'offset': 0xBC, 'type': 'ro'},
    'USER_PAUSE_LSB': {'offset': 0xC0, 'type': 'ro'},
    'USER_PAUSE_MSB': {'offset': 0xC4, 'type': 'ro'},
    'MAC_CYCLE_COUNT_LSB': {'offset': 0xC8, 'type': 'ro'},
    'MAC_CYCLE_COUNT_MSB': {'offset': 0xCC, 'type': 'ro'},
    'ECC_CORRECTABLE_COUNT': {'offset': 0xD0, 'type': 'ro'},
    'ECC_UNCORRECTABLE_COUNT': {'offset': 0xD8, 'type': 'ro'},
}


rx_stats_base_reg = {
    'TOTAL_BYTES_LSB': {'offset': 0x00, 'type': 'ro'},
    'TOTAL_BYTES_MSB': {'offset': 0x04, 'type': 'ro'},
    'TOTAL_GOOD_BYTES_LSB': {'offset': 0x08, 'type': 'ro'},
    'TOTAL_GOOD_BYTES_MSB': {'offset': 0x0C, 'type': 'ro'},
    'TOTAL_PACKETS_LSB': {'offset': 0x10, 'type': 'ro'},
    'TOTAL_PACKETS_MSB': {'offset': 0x14, 'type': 'ro'},
    'TOTAL_GOOD_PACKETS_LSB': {'offset': 0x18, 'type': 'ro'},
    'TOTAL_GOOD_PACKETS_MSB': {'offset': 0x1C, 'type': 'ro'},
    'PACKET_SMALL_LSB': {'offset': 0x20, 'type': 'ro'},
    'PACKET_SMALL_MSB': {'offset': 0x24, 'type': 'ro'},
    'BAD_CODE_COUNT_LSB': {'offset': 0x28, 'type': 'ro'},
    'BAD_CODE_COUNT_MSB': {'offset': 0x2C, 'type': 'ro'},
    'BAD_FCS_LSB': {'offset': 0x30, 'type': 'ro'},
    'BAD_FCS_MSB': {'offset': 0x34, 'type': 'ro'},
    'PACKET_BAD_FCS_LSB': {'offset': 0x38, 'type': 'ro'},
    'PACKET_BAD_FCS_MSB': {'offset': 0x3C, 'type': 'ro'},
    'STOMPED_FCS_LSB': {'offset': 0x40, 'type': 'ro'},
    'STOMPED_FCS_MSB': {'offset': 0x44, 'type': 'ro'},
    'TRUNCATED_LSB': {'offset': 0x48, 'type': 'ro'},
    'TRUNCATED_MSB': {'offset': 0x4C, 'type': 'ro'},
    'PACKET_64_BYTES_LSB': {'offset': 0x50, 'type': 'ro'},
    'PACKET_64_BYTES_MSB': {'offset': 0x54, 'type': 'ro'},
    'PACKET_65_127_BYTES_LSB': {'offset': 0x58, 'type': 'ro'},
    'PACKET_65_127_BYTES_MSB': {'offset': 0x5C, 'type': 'ro'},
    'PACKET_128_255_BYTES_LSB': {'offset': 0x60, 'type': 'ro'},
    'PACKET_128_255_BYTES_MSB': {'offset': 0x64, 'type': 'ro'},
    'PACKET_256_511_BYTES_LSB': {'offset': 0x68, 'type': 'ro'},
    'PACKET_256_511_BYTES_MSB': {'offset': 0x6C, 'type': 'ro'},
    'PACKET_512_1023_BYTES_LSB': {'offset': 0x70, 'type': 'ro'},
    'PACKET_512_1023_BYTES_MSB': {'offset': 0x74, 'type': 'ro'},
    'PACKET_1024_1518_BYTES_LSB': {'offset': 0x78, 'type': 'ro'},
    'PACKET_1024_1518_BYTES_MSB': {'offset': 0x7C, 'type': 'ro'},
    'PACKET_1519_1522_BYTES_LSB': {'offset': 0x80, 'type': 'ro'},
    'PACKET_1519_1522_BYTES_MSB': {'offset': 0x84, 'type': 'ro'},
    'PACKET_1523_1548_BYTES_LSB': {'offset': 0x88, 'type': 'ro'},
    'PACKET_1523_1548_BYTES_MSB': {'offset': 0x8C, 'type': 'ro'},
    'PACKET_1549_2047_BYTES_LSB': {'offset': 0x90, 'type': 'ro'},
    'PACKET_1549_2047_BYTES_MSB': {'offset': 0x94, 'type': 'ro'},
    'PACKET_2048_4095_BYTES_LSB': {'offset': 0x98, 'type': 'ro'},
    'PACKET_2048_4095_BYTES_MSB': {'offset': 0x9C, 'type': 'ro'},
    'PACKET_4096_8191_BYTES_LSB': {'offset': 0xA0, 'type': 'ro'},
    'PACKET_4096_8191_BYTES_MSB': {'offset': 0xA4, 'type': 'ro'},
    'PACKET_8192_9215_BYTES_LSB': {'offset': 0xA8, 'type': 'ro'},
    'PACKET_8192_9215_BYTES_MSB': {'offset': 0xAC, 'type': 'ro'},
    'TOOLONG': {'offset': 0xB0, 'type': 'ro'},
    'PACKET_LARGE': {'offset': 0xB8, 'type': 'ro'},
    'JABBER': {'offset': 0xC0, 'type': 'ro'},
    'OVERSIZE': {'offset': 0xC8, 'type': 'ro'},
    'UNICAST_LSB': {'offset': 0xD0, 'type': 'ro'},
    'UNICAST_MSB': {'offset': 0xD4, 'type': 'ro'},
    'MULTICAST_LSB': {'offset': 0xD8, 'type': 'ro'},
    'MULTICAST_MSB': {'offset': 0xDC, 'type': 'ro'},
    'BROADCAST_LSB': {'offset': 0xE0, 'type': 'ro'},
    'BROADCAST_MSB': {'offset': 0xE4, 'type': 'ro'},
    'VLAN_LSB': {'offset': 0xE8, 'type': 'ro'},
    'VLAN_MSB': {'offset': 0xEC, 'type': 'ro'},
    'PAUSE_LSB': {'offset': 0xF0, 'type': 'ro'},
    'PAUSE_MSB': {'offset': 0xF4, 'type': 'ro'},
    'USER_PAUSE_LSB': {'offset': 0xF8, 'type': 'ro'},
    'USER_PAUSE_MSB': {'offset': 0xFC, 'type': 'ro'},
    'INRANGEERR_LSB': {'offset': 0x100, 'type': 'ro'},
    'INRANGEERR_MSB': {'offset': 0x104, 'type': 'ro'},
    'MAC_CYCLE_COUNT_LSB': {'offset': 0x108, 'type': 'ro'},
    'MAC_CYCLE_COUNT_MSB': {'offset': 0x10C, 'type': 'ro'},
}
