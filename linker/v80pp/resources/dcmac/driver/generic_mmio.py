# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

import argparse
from pcie_bar import PCIeMapBar
from utils import int_types, hex_or_int, add_common_args


class GenericMMIO:
    def __init__(self, device: str = 'e2', base_offset: int = 0x0,
                 debug: bool = False):
        self._base_offset = base_offset
        self._pciemmio = PCIeMapBar(device, debug=debug)
        self._pciemmio.open()
        if debug:
            print(f"Base address: {hex(self._base_offset)}")

    def write(self, reg_offset: int = 0, value: int = 0):
        if not isinstance(value, int_types):
            raise ValueError(f"'{value=}' is not a {int_types} type")
        self._pciemmio.write(self._base_offset + reg_offset, value)

    def read(self, reg_offset: int = 0):
        return self._pciemmio.read(self._base_offset + reg_offset)

    def read_long(self, offset) -> int:
        """Read 8 bytes from BAR 'offset'"""
        val_low = self.read(offset)
        val_high = self.read(offset + 4)
        return int((val_high << 32) + val_low)

    def __del__(self):
        self._pciemmio.close()

    @property
    def base_address(self):
        return self._base_offset


def main(args):
    obj = GenericMMIO(args.dev, args.baseoffset)

    if args.write:
        obj.write(args.offset, args.value)

    if args.read:
        val = obj.read(args.offset)
        print(f'Offset: 0x{obj._base_offset + args.offset:X}, value=0x{val:X}')
    del obj


if __name__ == "__main__":
    parser = argparse.ArgumentParser()

    parser.add_argument('-b', '--baseoffset', type=hex_or_int,
                        help='Base Offset', required=True)

    parser.add_argument('-o', '--offset', type=hex_or_int,
                        help='Offset', required=True)

    parser.add_argument('-r', '--read', action='store_true',
                        help='Read')
    parser.add_argument('-w', '--write', action='store_true',
                        help='Write')
    parser.add_argument('-v', '--value', type=hex_or_int,
                        help='Value to be written', default=0)

    parser = add_common_args(parser, False)
    args = parser.parse_args()
    main(args)
