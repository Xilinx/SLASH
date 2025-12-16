# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

rtl_tg_regs = {
    'mode': {'offset': 0x10, 'size': 4, 'type': 'rw'},
    'dest_id': {'offset': 0x14, 'size': 4, 'type': 'rw'},
    'number_packets': {'offset': 0x18, 'size': 8, 'type': 'rw'},
    'number_beats': {'offset': 0x20, 'size': 4, 'type': 'rw'},
    'time_between_packets': {'offset': 0x24, 'size': 4, 'type': 'rw'},
    'reset_fsm': {'offset': 0x28, 'size': 4, 'type': 'rw'},
    'debug_fsms': {'offset': 0x2C, 'size': 4, 'type': 'rw'},
    'out_traffic_cycles': {'offset': 0x34, 'size': 8, 'type': 'rw'},
    'out_traffic_bytes': {'offset': 0x3C, 'size': 8, 'type': 'rw'},
    'out_traffic_packets': {'offset': 0x44, 'size': 8, 'type': 'rw'},
    'in_traffic_cycles': {'offset': 0x4C, 'size': 8, 'type': 'rw'},
    'in_traffic_bytes': {'offset': 0x54, 'size': 8, 'type': 'rw'},
    'in_traffic_packets': {'offset': 0x5C, 'size': 8, 'type': 'rw'},
    'summary_cycles': {'offset': 0x64, 'size': 8, 'type': 'rw'},
    'summary_bytes': {'offset': 0x6C, 'size': 8, 'type': 'rw'},
    'summary_packets': {'offset': 0x74, 'size': 8, 'type': 'rw'},
    'debug_reset': {'offset': 0x7C, 'size': 4, 'type': 'rw'}
}
