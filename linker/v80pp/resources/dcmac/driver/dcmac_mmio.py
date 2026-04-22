# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

import argparse
import pprint
import time
from tabulate import tabulate
from generic_mmio import GenericMMIO
from dcmac_reg import registers, tx_stats_base_reg, rx_stats_base_reg
from utils import rshift, add_common_args, get_ip_offset

class DCMAC(GenericMMIO):
    """"DCMAC MMIO Driver"""

    def __init__(self, device: str = 'e2', base_offset: int = 0x0):
        super().__init__(device, base_offset)
        self.set_pm_tick_trigger()

    def write(self, offset, value):
        if isinstance(offset, str):
            offset = registers[offset]['offset']
        super().write(offset, value)

    def read(self, offset):
        if isinstance(offset, str):
            offset = registers[offset]['offset']
        return super().read(offset)

    #TODO: __getattr__ and __setattr__ need more validation
    def __getattr__(self, name):
        """Get the value of a register"""
        if name in registers:
            val = self.read(name)
            if registers[name].get('fields'):
                field_dict = dict()
                for k, v in registers[name]['fields'].items():
                    field_dict[k] = rshift(val, v['start'], v['length'])
                return field_dict
            else:
                return val
        else:
            raise AttributeError(f"'{self.__class__.__name__}' object has "
                                 f"no attribute '{name}'")

    #TODO: validate
    def __setattr__(self, name, value):
        """Set the value of a register"""
        if name in registers:
            # TODO:  accept value as a dictionary to set individual fields
            if isinstance(value, dict):
                values_dict = value
                value = self.read(name)
                reg_fields = registers[name].get('fields', {})
                for field_name, field_value in values_dict.items():
                    if field_name in reg_fields:
                        field = registers[name]['fields'][field_name]
                        start, length = field['start'], field['length']
                        mask = ((1 << length) - 1) << start
                        value &= ~ mask # clear the bits
                        value |= (field_value << start) & mask # set the bits
                    else:
                        raise ValueError(f"Field '{field_name}' not found "
                                         f"in register '{name}'")

            self.write(name, value)
        else:
            super().__setattr__(name, value)

    # TODO: implement functions to read and write channel registers without
    # having to specify the channel offset, just the channel number
    # def read_chn_reg(self, chn_reg_name, channel):
    #     offset = registers['C0_' + chn_reg_name]['offset']
    #     return self._pciemmio.read(self._offset + offset)

    def read_reg_field(self, reg_name: str, field: str):
        """Read a field from  reg'"""
        field = registers[reg_name]['fields'][field]
        val = self.read(reg_name)
        return rshift(val, field['start'], field['length'])

    def read_long(self, offset) -> int:
        """Read 8 bytes from BAR 'offset'"""
        if isinstance(offset, str):
            offset = registers[offset]['offset']
        val_low = self.read(offset)
        val_high = self.read(offset + 4)
        return int((val_high << 32) + val_low)

    @property
    def revision(self):
        return self.read('CONFIGURATION_REVISION')

    @property
    def ip_dict(self):
        vals = {}
        for k, v in registers.items():
            readval = self.read(v['offset'])
            vals[k] = {'value': readval}
            if v.get('fields'):
                vals[k]['fields'] = {}
                for k1, v1 in v['fields'].items():
                    vals[k]['fields'][k1] = rshift(readval, v1['start'], v1['length'])

        return vals

    @property
    def status(self):
        status_dict = {}
        for reg_name, spec in registers.items():
            if "STATUS" in reg_name:
                fields = spec.get('fields', False)
                readval = self.read(reg_name)
                subkey = "real-time" if "_RT_" in reg_name else "latched"
                entry_name = reg_name if "_RT_" not in reg_name else reg_name.replace("_RT", "")
                if entry_name not in status_dict:
                    if fields:
                        status_dict[entry_name] = {}
                        for f in fields:

                            status_dict[entry_name][f] = {"latched": "-", "real-time": "-", "default": fields[f]['default']}
                    else:
                        status_dict[entry_name] = {"latched": "-", "real-time": "-", "default": "-"}

                if fields:
                    for f_name, f_spec in fields.items():
                        status_dict[entry_name][f_name][subkey] = rshift(readval, f_spec['start'], f_spec['length'])
                else:
                    status_dict[entry_name][subkey] = readval
        return status_dict

    def print_status(self, only_modified_fields: bool = False):
        status_dict = self.status
        table = []
        table += [["Register", "Field", "Latched", "Real-Time", "Default"]]
        for reg_name, fields in status_dict.items():
            row_count = 0
            for field_name, field in fields.items():
                default_val = field["default"]
                if only_modified_fields:
                    if field["latched"] == default_val and field["real-time"] == default_val:
                        continue
                table += [[reg_name if row_count == 0 else "", field_name, field["latched"], field["real-time"], default_val]]
                row_count += 1
            if row_count > 0:
                table += [["--------------------", "--------------------", "--------", "--------", "--------"]]
        if only_modified_fields:
            if len(table) == 1:
                print("All status Registers have default values")
                return
            print("Status Registers with non-default values")
        else:
            print("Status Registers")
        print(tabulate(table, headers="firstrow", tablefmt="pretty"))

    @property
    def config(self):
        config_dict = {}
        config_regs = ['GLOBAL_MODE', 'C0_TX_MODE_REG', 'C0_RX_MODE_REG']
        for reg_name in config_regs:
            spec = registers[reg_name]
            fields = spec.get('fields', False)
            readval = self.read(spec['offset'])
            config_dict[reg_name] = {}
            for f_name, f_spec in fields.items():
                val = rshift(readval, f_spec['start'], f_spec['length'])
                config_dict[reg_name][f_name] = {"value": val, "default": fields[f_name]['default']}

        return config_dict

    def print_config(self, only_modified_fields: bool = False):
        config_dict = self.config
        table = []
        table += [["Register", "Field", "Value", "Default"]]
        for reg_name, fields in config_dict.items():
            row_count = 0
            for field_name, field in fields.items():
                default_val = field["default"]
                if only_modified_fields:
                    if field["value"] == default_val:
                        continue
                table += [[reg_name if row_count == 0 else "", field_name, field["value"], default_val]]
                row_count += 1
            if row_count > 0:
                table += [["--------------------", "--------------------", "--------", "--------", "--------"]]
        if only_modified_fields:
            print("Configuration Registers with non-default values")
        else:
            print("Configuration Registers")
        print(tabulate(table, headers="firstrow", tablefmt="pretty"))

    def tx_stats(self, port: int = 0, debug: bool = False,
                 verbose: int = 0):
        """Reads and print TX stats for the given port"""

        if isinstance(port, int) and not 0 <= port < 1:
            raise ValueError("'port' must be either 0 or 1")

        baseoffset = 0x1000 * (port + 1) + 0x0200

        # Sets pm tick to be triggered by registers
        value = self.read(baseoffset - 0x200 + 0x40)
        pm_tick_bit = registers['C0_TX_MODE_REG']['fields']['c0_ctl_tx_tick_reg_mode_sel']['start']
        value |= (1 * (2**pm_tick_bit))
        self.write(baseoffset - 0x200 + 0x40, value)

        # trigger ALL_CHANNEL_MAC_TICK_REG_TX
        #offset = registers['ALL_CHANNEL_MAC_TICK_REG_TX']['offset']
        #self.write(offset, 0)
        #self.write(offset, 1)
        #self.write(offset, 0)

        # trigger pm tick
        self.write(baseoffset - 0x200 + 0xFC, 0)
        self.write(baseoffset - 0x200 + 0xFC, 1)

        for i in range(10):
            val = self.read(baseoffset - 0x200 + 0x808)
            if val != 0:
                break

        heading = [[f"TX Stats {port=}", "Value"]]
        if debug:
            heading[0].append('Offset Address')
        table = self._stats(baseoffset, 'tx', heading, debug, verbose)
        print(tabulate(table, headers="firstrow", tablefmt="pretty"))

    def rx_stats(self, port: int = 0, debug: bool = False,
                 verbose: int = 0):
        """Reads and print RX stats for the given port"""

        if isinstance(port, int) and not 0 <= port < 1:
            raise ValueError("'port' must be either 0 or 1")

        baseoffset = 0x1000 * (port + 1) + 0x0400

        # Sets pm tick to be triggered by registers
        value = self.read(baseoffset - 0x400 + 0x44)
        pm_tick_bit = registers['C0_RX_MODE_REG']['fields']['c0_ctl_rx_tick_reg_mode_sel']['start']
        value |= (1 * (2**pm_tick_bit))
        self.write(baseoffset - 0x400 + 0x44, value)

        # trigger ALL_CHANNEL_MAC_TICK_REG_RX
        #offset = registers['ALL_CHANNEL_MAC_TICK_REG_RX']['offset']
        #self.write(offset, 0)
        #self.write(offset, 1)
        #self.write(offset, 0)

        # trigger pm tick
        self.write(baseoffset - 0x400 + 0xF4, 0)
        self.write(baseoffset - 0x400 + 0xF4, 1)

        for i in range(10):
            val = self.read(baseoffset - 0x400 + 0xC08)
            if val != 0:
                break

        heading = [[f"RX Stats {port=}", "Value"]]
        if debug:
            heading[0].append('Offset Address')
        table = self._stats(baseoffset, 'rx', heading, debug, verbose)
        print(tabulate(table, headers="firstrow", tablefmt="pretty"))

    def _stats(self, baseoffset: int, dir: str, tableheading: str,
               debug: bool, verbose: int = 0):

        table = tableheading
        stats_base_reg = tx_stats_base_reg if dir == 'tx' else rx_stats_base_reg
        for k, v in stats_base_reg.items():
            if 'LSB' in k:
                readval = self.read_long(baseoffset + v['offset'])
            elif 'MSB' in k:
                continue
            else:
                readval = self.read(baseoffset + v['offset'])

            if readval == 0 and verbose < 1:
                continue
            key = k.replace('_LSB', '')
            ltable = [key, readval]
            if debug:
                ltable.append(f"0x{(baseoffset + v['offset']):X}")
            table.append(ltable)

        return table

    def clear_latched_flags(self):
        MASK = (1 << 32) - 1
        for reg_name, spec in registers.items():
            if "STATUS" in reg_name:
                self.write(spec['offset'], MASK)

    def set_pm_tick_trigger(self) -> int:
        """Sets pm tick to be triggered by registers"""
        value = self.read(registers['GLOBAL_MODE']['offset'])
        tx_reg_bit = registers['GLOBAL_MODE']['fields']['ctl_tx_all_ch_tick_reg_mode_sel']['start']
        rx_reg_bit = registers['GLOBAL_MODE']['fields']['ctl_rx_all_ch_tick_reg_mode_sel']['start']

        val_tx = 1 * (2**tx_reg_bit)
        val_rx = 1 * (2**rx_reg_bit)
        val_tx += val_rx

        value |= val_tx
        self.write(registers['GLOBAL_MODE']['offset'], value)
        return self.read(registers['GLOBAL_MODE']['offset'])

    def reset_tx(self, clear_status_history: bool = True):
        """Forces a resets on the transmitting DCMAC core
        It Follows the reset procedure outlined in the DCMAC user guide pg369,
        page 161 ("Transmit Fixed Ethernet Startup Procedure when Using tx_core_reset")
        """
        rst_successful = True
        offset = lambda x: registers[x]['offset']
        rst_core_regs = [offset('GLOBAL_CONTROL_REG_TX')]
        rst_serdes_regs = [offset('C0_PORT_CONTROL_REG_TX') + 0x1000 * i for i in range(6)]
        rst_flush_regs = [offset('C0_CHANNEL_CONTROL_REG_TX') + 0x1000 * i for i in range(6)]
        for reg in rst_core_regs + rst_serdes_regs + rst_flush_regs:
            self.write(reg, 2**32-1)
        time.sleep(0.1)
        # first release port RSTs, then core reset
        for reg in rst_serdes_regs + rst_core_regs:
            self.write(reg, 0x0)

        # wait for tx_local_fault
        for _ in range(10):
            if self.tx_aligned:
                # print('TX status: OK')
                break
            time.sleep(0.2)
        else:
            print('TX status: local fault')
            rst_successful = False

        # release flush
        for reg in rst_flush_regs:
            self.write(reg, 0x0)

        if clear_status_history:
            self.clear_latched_flags()
        return rst_successful

    def reset_rx(self, clear_status_history: bool = True):
        """Forces a resets on the receiving DCMAC core
        It Follows the reset procedure outlined in the DCMAC user guide pg369,
        page 162 ("Receive Fixed Ethernet Startup Procedure when Using rx_core_reset")
        """
        ACTIVE_PORTS = 6 # TODO: this should be set by the user
        offset = lambda x: registers[x]['offset']
        rst_core_regs = [(offset('GLOBAL_CONTROL_REG_RX'), 7)]
        rst_serdes_regs = [(offset('C0_PORT_CONTROL_REG_RX') + 0x1000 * i, 2) for i in range(6)]
        rst_flush_regs = [(offset('C0_CHANNEL_CONTROL_REG_RX') + 0x1000 * i, 1) for i in range(6)]
        for reg,reset_code in rst_core_regs + rst_serdes_regs + rst_flush_regs:
            self.write(reg, reset_code)

        time.sleep(0.5)
        # first release core resets, then flush and finally serdes resets
        for reg,_ in rst_core_regs + rst_flush_regs[:ACTIVE_PORTS] + rst_serdes_regs[:ACTIVE_PORTS]:
            self.write(reg, 0)

        # check Rx alignment
        rst_successful = True
        for i in range(10):
            if self.rx_aligned:
                break
            print(".", end= "", flush=True)
            time.sleep(0.25)
        else:
            print('WARN: Chn 0 RX failed to achieve alignment')
            rst_successful = False

        if clear_status_history:
            self.clear_latched_flags()
        return rst_successful

    @property
    def tx_aligned(self):
        # TODO: this should take the channel number as an argument and
        # return the corresponding channel status
        return self.C0_STAT_CHAN_TX_MAC_RT_STATUS_REG['c0_stat_tx_local_fault'] == 0

    @property
    def rx_aligned(self):
        # TODO: this should take the channel number as an argument and
        # return the corresponding channel status
        chn_status_dict = self.C0_STAT_PORT_RX_PHY_RT_STATUS_REG
        return chn_status_dict['c0_stat_rx_status'] == 1 and \
                chn_status_dict['c0_stat_rx_aligned'] == 1

    @property
    def link_up(self):
        return self.rx_aligned and self.tx_aligned

def main(args):
    offset = get_ip_offset(0x200_0000, args.dcmac)
    obj = DCMAC(args.dev, offset)

    if args.tx:
        obj.reset_tx()

    if args.rx:
        obj.reset_rx()

    if args.rx or args.tx or args.clear:
        time.sleep(0.5)
        obj.clear_latched_flags()

    if args.status or not (args.rx or args.tx or args.print or args.show_config):
        obj.print_status(only_modified_fields=args.verbose < 1)

    if args.show_config:
        obj.print_config(only_modified_fields=args.verbose < 1)

    if args.print:
        # pprint.pp(obj.ip_dict)
        obj.tx_stats(0, True, verbose=args.verbose)
        obj.rx_stats(0, True, verbose=args.verbose)
    del obj


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-r', '--rx', action='store_true',
                        help='Reset RX')
    parser.add_argument('-t', '--tx', action='store_true',
                        help='Reset TX')
    parser.add_argument('-s', '--status', action='store_true',
                        help='Print status')
    # default only status
    parser.add_argument('-p', '--print', action='store_true',
                        help='Print stats')
    parser.add_argument('-c', '--clear', action='store_true',
                        help='Clear latched flags')
    parser.add_argument('-C', '--show-config', action='store_true',
                        help='Show configuration')
    parser = add_common_args(parser, verbose=True)

    args = parser.parse_args()
    main(args)