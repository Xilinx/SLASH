# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

import argparse
from default_ip import DefaultIP
from utils import add_common_args, get_ip_offset


class TrafficGenerator(DefaultIP):
    """Specialization to support TrafficGenerator IP"""

    _flits_offset = 0x10
    _dest_offset = 0x18

    @property
    def dest(self):
        value = self.read(self._dest_offset)
        return value

    @dest.setter
    def dest(self, value: int):
        if not isinstance(value, int):
            raise ValueError(f"{value=} must be an integer")
        elif value < 0:
            raise ValueError(f"{value=} must be a positive integer")

        self.write(self._dest_offset, value)

    @property
    def flits(self):
        value = self.read(self._flits_offset)
        return value

    @flits.setter
    def flits(self, value: int):
        if not isinstance(value, int):
            raise ValueError(f"{value=} must be an integer")
        elif value < 1:
            raise ValueError(f"{value=} must be bigger than 0")

        self.write(self._flits_offset, value)


def main(args):
    intf = 0
    offset = get_ip_offset(0x400_0000, args.dcmac*2 +intf)
    tgen = TrafficGenerator(args.dev, offset)

    tgen.flits = args.flits
    tgen.dest = 0
    tgen.start()
    del tgen


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-f', '--flits', type=int, default=10,
                        help='Number of 64-Byte flits', required=False)

    parser = add_common_args(parser)
    args = parser.parse_args()

    main(args)
