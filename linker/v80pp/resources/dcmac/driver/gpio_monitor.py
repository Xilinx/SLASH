# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

import argparse
from axigpio_mmio import AxiGpioMMIO
from utils import add_common_args, get_ip_offset


shift_map = {
    'gt0_tx_reset_done': {'shift': 0, 'bits': 4},
    'gt1_tx_reset_done': {'shift': 4, 'bits': 4},
    'gt0_rx_reset_done': {'shift': 8, 'bits': 4},
    'gt1_rx_reset_done': {'shift': 12, 'bits': 4},
    'gtpowergood': {'shift': 16, 'bits': 1},
    'dual_dcmac': {'shift': 18, 'bits': 1},
}


def _get_shift_and_mask(key: str) -> tuple[int, int]:
    """Return the shift and mask give the key"""
    map = shift_map[key]
    return map['shift'], (1 << map['bits']) - 1


class AxiGPIOMonitor(AxiGpioMMIO):

    def __init__(self, device: str = 'e2', base_offset: int = 0x0,
                 gpio_index: int = 0):
        self._gpio_index = gpio_index
        super().__init__(device, base_offset)

    def _get(self, key: str, gpio: int = 0):
        shift, mask = _get_shift_and_mask(key)
        return (self.read(gpio) >> shift) & mask

    def _create_property(key: str, gpio: int = 0):
        """Create property getter and setter for given key."""
        return property(
            lambda self: self._get(key, gpio)
        )

    gt0_tx_reset_done = _create_property('gt0_tx_reset_done')
    gt1_tx_reset_done = _create_property('gt1_tx_reset_done')
    gt0_rx_reset_done = _create_property('gt0_rx_reset_done')
    gt1_rx_reset_done = _create_property('gt1_rx_reset_done')
    gtpowergood = _create_property('gtpowergood')
    dual_dcmac = _create_property('dual_dcmac')


def main(args):
    offset = get_ip_offset(0x204_0200, args.dcmac)
    obj = AxiGPIOMonitor(args.dev, offset, gpio_index=0)

    print(f'{obj.gt0_tx_reset_done=}')
    print(f'{obj.gt0_rx_reset_done=}')
    print(f'{obj.gt1_tx_reset_done=}')
    print(f'{obj.gt1_rx_reset_done=}')
    print(f'{obj.gtpowergood=}')
    del obj


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser = add_common_args(parser)

    main(parser.parse_args())
