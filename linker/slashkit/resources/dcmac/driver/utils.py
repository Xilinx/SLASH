# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

import argparse
import numpy as np
import os

int_types = (int, np.int8, np.int16, np.int32, np.uint8, np.uint16, np.uint32)


def hex_or_int(value):
    try:
        if value.startswith(('0x', '0X')):
            return int(value, 16)
        return int(value)
    except ValueError:
        raise argparse.ArgumentTypeError(f"Invalid value: {value}. Must be an "
                                         "integer or hexadecimal.")


def rshift(value: int, shift: int = 0, bitwidth: int = 1):
    """Right shift value and mask with 'bitwidth'"""
    value = value >> shift
    mask = (2**bitwidth)-1
    return int(value & mask)


def get_ip_offset(baseoffset: int, mac_id: int):
    """Get IP offset based on 'baseoffset' and 'mac_id'"""

    return baseoffset + (0x100_0000 * mac_id)


def add_common_args(parser, enable_mac: bool = True, verbose: bool = False):
    """Add common arguments to the parser"""    
    default_dev = os.environ['V80_DEV'] if 'V80_DEV' in os.environ else 'e2'
    parser.add_argument('-d', '--dev', help=f"PCIe device Bus ID, e.g., '{default_dev}'",
                        default=default_dev)
    if enable_mac:
        default_dcmac_id = os.environ['V80_DCMAC_ID'] if 'V80_DCMAC_ID' in os.environ else '0'
        parser.add_argument('-m', '--dcmac', help="DCMAC ID either 0 or 1",
                            default=default_dcmac_id, choices=[0, 1], type=int)
    if verbose:
        parser.add_argument('-v', '--verbose', type=int, default=0,
                            choices=[0, 1], help='Verbosity mode')

    return parser
