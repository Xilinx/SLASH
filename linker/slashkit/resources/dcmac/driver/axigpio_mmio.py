# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT


import argparse
from generic_mmio import GenericMMIO, int_types
from utils import add_common_args, get_ip_offset


key_map_offset = {
    'gpio_tri': 0x4,
    'gpio2_tri': 0xc,
    'gier': 0x11C,
    'ip_ier': 0x128,
    'ip_isr': 0x120,
}


class AxiGpioMMIO(GenericMMIO):
    """Driver to work with AXI GPIO"""

    _data_off = 0
    _tri_off = 4

    def write(self, gpio: int = 0, value: int = 0):
        """Write to """
        offset = self._data_off + 0x8 * gpio
        super().write(offset, value)

    def read(self, gpio: int = 0):
        offset = self._data_off + 0x8 * gpio
        return super().read(offset)

    def _get(self, key: str):
        offset = key_map_offset[key]
        return super().read(offset)

    def _set(self, key: str, val: int):
        if not isinstance(val, int_types):
            raise ValueError(f"'{val=}' is not a '{int_types}' type")
        offset = key_map_offset[key]
        super().write(offset, val)

    def _create_property(key: str):
        """Create property getter and setter for given key."""
        return property(
            lambda self: self._get(key),
            lambda self, val: self._set(key, val)
        )

    gpio_tri = _create_property('gpio_tri')
    gpio2_tri = _create_property('gpio2_tri')
    gier = _create_property('gier')
    ip_ier = _create_property('ip_ier')
    ip_isr = _create_property('ip_isr')


def main(args):
    value = 0
    if args.reset:
        value = (2**32) - 1

    offset0 = get_ip_offset(0x204_0000, args.dcmac)
    obj = AxiGpioMMIO(args.dev, offset0)
    obj.write(0, value)
    print(f'GPIO1: 0x{obj.base_address:X}, value={obj.read(0)}')
    obj.write(1, value)
    print(f'GPIO2: 0x{obj.base_address:X}, value={obj.read(1)}')
    del obj

    offset1 = get_ip_offset(0x204_0100, args.dcmac)
    obj = AxiGpioMMIO(args.dev, offset1)
    obj.write(0, value)
    print(f'GPIO1: 0x{obj.base_address:X}, value={obj.read(0)}')
    obj.write(1, value)
    print(f'GPIO2: 0x{obj.base_address:X}, value={obj.read(1)}')
    del obj

    offset2 = get_ip_offset(0x204_0300, args.dcmac)
    obj = AxiGpioMMIO(args.dev, offset2)
    obj.write(0, value)
    print(f'GPIO1: 0x{obj.base_address:X}, value={obj.read(0)}')
    obj.write(1, value)
    print(f'GPIO2: 0x{obj.base_address:X}, value={obj.read(1)}')
    del obj


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-r', '--reset', action='store_true',
                        help='Reset Logic')
    parser = add_common_args(parser)

    args = parser.parse_args()

    main(args)
