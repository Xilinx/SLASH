# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT


import argparse
import numpy as np
import time
from axigpio_mmio import AxiGpioMMIO
from generic_mmio import hex_or_int, int_types
from utils import add_common_args, get_ip_offset


shift_map = {
    'gt_reset': {'shift': 0, 'bits': 1},
    'gt_line_rate': {'shift': 1, 'bits': 8},
    'loopback': {'shift': 9, 'bits': 3},
    'txprecursor': {'shift': 12, 'bits': 6},
    'txpostcursor': {'shift': 18, 'bits': 6},
    'txmaincursor': {'shift': 24, 'bits': 6},
    'rxcdrhold': {'shift': 31, 'bits': 1},
}


def _get_shift_and_mask(key: str) -> tuple[int, int]:
    """Return the shift and mask give the key"""
    map = shift_map[key]
    return map['shift'], (1 << map['bits']) - 1


def _get_updated_value(key: str, cval: int, nval: int) -> int:
    """Generate the updated 32-bit value applying a shift and mask"""

    shift, mask = _get_shift_and_mask(key)
    shiftedmask = np.uint32(mask << shift & 0xFFFF_FFFF)
    cval_cleared = np.uint32(cval & ~shiftedmask)
    shiftedval = np.uint32(((nval & mask) << shift) & 0xFFFF_FFFF)

    return np.uint32(shiftedval | cval_cleared)


class AxiGTController(AxiGpioMMIO):

    def __init__(self, device: str = 'e2', base_offset: int = 0x0,
                 gpio_index: int = 0):
        self._gpio_index = gpio_index
        super().__init__(device, base_offset)

    def _get(self, key: str, gpio: int = 0):
        shift, mask = _get_shift_and_mask(key)
        return (self.read(gpio) >> shift) & mask

    def _set(self, key: str, gpio: int, val: int):
        if not isinstance(val, int_types):
            raise ValueError(f"'{val=}' is not a '{int_types}' type")

        cval = self.read(gpio)
        uval = _get_updated_value(key, cval, val)
        self.write(gpio, uval)

    def _create_property(key: str, gpio: int = 0):
        """Create property getter and setter for given key."""
        return property(
            lambda self: self._get(key, gpio),
            lambda self, val: self._set(key, gpio, val)
        )

    gt_reset = _create_property('gt_reset')
    gt_line_rate = _create_property('gt_line_rate')
    loopback = _create_property('loopback')
    txprecursor = _create_property('txprecursor')
    txpostcursor = _create_property('txpostcursor')
    txmaincursor = _create_property('txmaincursor')
    rxcdrhold = _create_property('rxcdrhold')


def main(args):
    offset = get_ip_offset(0x204_0000, args.dcmac)
    obj = AxiGTController(args.dev, offset, gpio_index=0)

    if args.reset:
        print('Resetting GT')
        obj.gt_reset = 1
        time.sleep(0.1)
        obj.gt_reset = 0
        return

    if args.loopback is not None:
        obj.loopback = args.loopback
        time.sleep(0.1)
        print(f'Loopback mode set to: {obj.loopback}')
        return

    if args.linerate:
        obj.gt_line_rate = args.linerate
        time.sleep(0.1)
        print(f'Line rate mode set to: {obj.gt_line_rate}')
        return

    print(f'{obj.gt_reset=}')
    print(f'{obj.gt_line_rate=}')
    print(f'{obj.loopback=}')
    print(f'{obj.txprecursor=}')
    print(f'{obj.txpostcursor=}')
    print(f'{obj.txmaincursor=}')
    print(f'{obj.rxcdrhold=}')
    del obj


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-l', '--loopback', type=hex_or_int, help="Loopback "
                        f" mode, a {shift_map['loopback']['bits']}-bit value",
                        default=None)
    parser.add_argument('-r', '--reset', action='store_true',
                        help='Reset GT')
    parser.add_argument('-s', '--linerate', help="Line-rate mode a "
                        f"{shift_map['gt_line_rate']['bits']}-bit value",
                        type=hex_or_int)
    parser = add_common_args(parser)

    main(parser.parse_args())
