# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

import mmap
from warnings import warn
import numpy as np
BAR_SIZE = 128 * 1024 * 1024  # 256 MB


def _get_bar_path(dev, resource: int = 2, debug=True):
    """Generate BAR path based on the PCIe Bus ID"""
    dev_path = f"/sys/bus/pci/devices/0000:{dev}:00.2/resource{resource}"
    if debug:
        print(f"Using BDF: {dev_path}")
    return dev_path


class PCIeMapBar:
    """Wrapper class to allows MMIO read and write operations from PCIe BAR"""
    def __init__(self, device: str = 'e2', resource: int = 2,
                 barsize: int = BAR_SIZE, debug: bool = True):
        self._bar = None
        self._barpath = _get_bar_path(device, resource, debug)
        self._barsize = barsize

    def open(self):
        """Open BAR"""
        with open(self._barpath, "r+b") as f:
            self._bar = mmap.mmap(f.fileno(), self._barsize, mmap.MAP_SHARED,
                                  mmap.PROT_READ | mmap.PROT_WRITE)
        self.mem = np.frombuffer(self._bar, np.uint32, (self._barsize+3) >> 2)

    def close(self):
        """Close BAR"""
        if self._bar is not None:
            del self.mem
            self._bar.close()
            self._bar = None

    def read(self, byte_offset: int) -> int:
        """Read 4 bytes from BAR 'byte_offset'"""
        if byte_offset & 0x3:
            warn(f"Byte offset {byte_offset} is not aligned to 32-bit words." +
                 "Aligning to previous 32-bit boundary")
        if self._bar is None:
            raise RuntimeError('BAR is not opened')

        return int(self.mem[byte_offset >> 2])

    def write(self, byte_offset: int, value: int):
        """Write 4 bytes to BAR 'byte_offset'"""
        if byte_offset & 0x3:
            warn(f"Byte offset {byte_offset} is not aligned to 32-bit words." +
                 "Aligning to previous 32-bit boundary")
        if self._bar is None:
            raise RuntimeError('BAR is not opened')
        value_32 = value & 0xFFFFFFFF
        if value_32 != value:
            warn("Trying to write a value larger than 32 bits to the PCIe " +
                 "device, truncating to 32 bits")

        self.mem[byte_offset >> 2] = value_32
