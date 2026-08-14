# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

from default_ip import DefaultIP


class TrafficProducer(DefaultIP):
    """Specialization to support TrafficProducer IP"""

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

