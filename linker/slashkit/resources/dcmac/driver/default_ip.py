# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

from generic_mmio import GenericMMIO
from utils import rshift


def decode_control_register(value: int) -> dict:
    cregister = {}
    cregister['ap_start': bool(rshift(value))]
    cregister['ap_done': bool(rshift(value, 1))]
    cregister['ap_idle': bool(rshift(value, 2))]
    cregister['ap_ready': bool(rshift(value, 3))]
    cregister['ap_continue': bool(rshift(value, 4))]
    cregister['auto_restart': bool(rshift(value, 7))]
    return cregister

cregs = {
    "controlreg": {'offset': 0x0, 'type': 'rw',
                   'fields': {'ap_start': {'start': 0, 'length': 1, 'default': 0, 'type': 'rw'},
                              'ap_done': {'start': 1, 'length': 1, 'default': 0, 'type': 'ro'},
                              'ap_idle': {'start': 2, 'length': 1, 'default': 0, 'type': 'ro'},
                              'ap_ready': {'start': 3, 'length': 1, 'default': 0, 'type': 'ro'},
                              'ap_continue': {'start': 4, 'length': 1, 'default': 0, 'type': 'rw'},
                              'auto_restart': {'start': 7, 'length': 1, 'default': 0, 'type': 'rw'}
                              }
                    },
    "globalintreg": {'offset': 0x4, 'type': 'rw'},
    "intenable": {'offset': 0x8, 'type': 'rw'},
    "intstatus": {'offset': 0x10, 'type': 'rw'},
}


class DefaultIP(GenericMMIO):
    """Generic IP Driver"""
    _controlreg = 0x00
    _globalintreg = 0x04
    _intenable = 0x08
    _intstatus = 0x10

    def __init__(self, device: str = 'e2', base_offset: int = 0x0,
                 debug: bool = False, regs: dict = None):
        super().__init__(device, base_offset, debug)
        self.registers = cregs if regs is None else regs

    def start(self, value: int = 1):
        """Start IP once"""
        self.write(self._controlreg, value)

    def autostart(self):
        """Autostart IP"""
        self.start(0x81)

    def controlreg(self) -> dict:
        value = self.read(self._controlreg)
        cregisters = decode_control_register(value)
        print(cregisters)
        return cregisters

    def global_interrupt(self):
        value = self.read(self._globalintreg)
        gintenable = {'global_interrupt_enable': bool(rshift(value))}
        print(gintenable)
        return gintenable

    def interrupt_enable(self):
        value = self.read(self._intenable)
        intenable = {'interrupt_enable': bool(rshift(value))}
        print(intenable)
        return intenable

    def interrupt_status(self):
        value = self.read(self._intstatus)
        intstatus = {'interrupt_status': bool(rshift(value))}
        print(intstatus)
        return intstatus
