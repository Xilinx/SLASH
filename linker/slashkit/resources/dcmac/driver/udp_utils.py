# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

import ipaddress
import numpy as np
from enum import Enum
from tabulate import tabulate
from IPython.display import JSON
from default_ip import DefaultIP
from netlayer_regs import nl_regs


def _byte_ordering_endianess(num, length=4):
    """
    Convert from little endian to big endian and vice versa

    Parameters
    ----------
    num: int
      input number

    length:
      number of bytes of the input number

    Returns
    -------
    An integer with the endianness changed with respect to input number

    """
    if not isinstance(num, int):
        raise ValueError("num must be an integer")

    if not isinstance(length, int):
        raise ValueError("length must be an positive integer")
    elif length < 0:
        raise ValueError("length cannot be negative")

    aux = 0
    for i in range(length):
        byte_index = num >> ((length - 1 - i) * 8) & 0xFF
        aux += byte_index << (i * 8)
    return aux


class NetworkLayer(DefaultIP):
    """This class wraps the common function of the Network Layer IP

    """

    bindto = ["xilinx.com:kernel:networklayer:1.0"]

    _socketType = np.dtype(
        [
            ("theirIP", str, 16),
            ("theirPort", np.uint16),
            ("myPort", np.uint16),
            ("valid", bool),
        ]
    )

    def __init__(self, device: str = 'e2', base_offset: int = 0x0,
                 debug: bool = False):
        super().__init__(device, base_offset, debug)
        self.registers = nl_regs
        self.sockets = np.zeros(16, dtype=self._socketType)
        self.freq = None

    def populate_socket_table(self, debug: bool = False):
        """
        Populate a socket table

        Optionals
        ---------
        debug: bool
            If enables read the current status of the UDP Table

        Returns
        -------
        If debug is enable read the current status of the UDP Table

        """

        theirIP_offset = self.registers['udp_theirIP_offset']['offset']
        theirPort_offset = self.registers['udp_theirPort_offset']['offset']
        udp_myPort_offset = self.registers['udp_myPort_offset']['offset']
        udp_valid_offset = self.registers['udp_valid_offset']['offset']
        numSocketsHW = int(self.read(self.registers['udp_number_sockets']['offset']))

        if numSocketsHW < len(self.sockets):
            raise Exception(f"Socket list length ({len(self.sockets)}) is "
                            "bigger than the number of sockets in hardware "
                            f"({numSocketsHW})")

        # Iterate over the socket object
        for i in range(numSocketsHW):
            ti_offset = theirIP_offset + i * 8
            tp_offset = theirPort_offset + i * 8
            mp_offset = udp_myPort_offset + i * 8
            v_offset = udp_valid_offset + i * 8

            theirIP = 0
            if self.sockets[i]["theirIP"]:
                theirIP = int(ipaddress.IPv4Address(self.sockets[i]
                                                    ["theirIP"]))

            self.write(ti_offset, theirIP)
            self.write(tp_offset, int(self.sockets[i]["theirPort"]))
            self.write(mp_offset, int(self.sockets[i]["myPort"]))
            self.write(v_offset, int(self.sockets[i]["valid"]))

        if debug:
            return self.get_socket_table()

    def get_socket_table(self) -> dict:
        """ Reads the socket table

        Returns
        -------
        Returns socket table
        """

        theirIP_offset = self.registers['udp_theirIP_offset']['offset']
        theirPort_offset = self.registers['udp_theirPort_offset']['offset']
        udp_myPort_offset = self.registers['udp_myPort_offset']['offset']
        udp_valid_offset = self.registers['udp_valid_offset']['offset']
        numSocketsHW = int(self.read(self.registers['udp_number_sockets']['offset']))

        socket_dict = dict()
        socket_dict['Number of Sockets'] = numSocketsHW
        socket_dict['socket'] = dict()
        # Iterate over all the UDP table
        for i in range(numSocketsHW):
            ti_offset = theirIP_offset + i * 8
            tp_offset = theirPort_offset + i * 8
            mp_offset = udp_myPort_offset + i * 8
            v_offset = udp_valid_offset + i * 8
            isvalid = self.read(v_offset)
            if isvalid:
                ti = self.read(ti_offset)
                tp = self.read(tp_offset)
                mp = self.read(mp_offset)
                socket_dict['socket'][i] = dict()
                socket_dict['socket'][i]['theirIP'] = \
                    str(ipaddress.IPv4Address(ti))
                socket_dict['socket'][i]['theirPort'] = tp
                socket_dict['socket'][i]['myPort'] = mp

        print(f'{socket_dict=}')
        return JSON(socket_dict, rootname='socket_table')

    def invalidate_socket_table(self):
        """ Clear the Socket table """

        udp_valid_offset = self.registers['udp_valid_offset']['offset']
        numSocketsHW = int(self.registers['udp_number_sockets'])
        for i in range(numSocketsHW):
            self.write(int(udp_valid_offset + i * 8), 0)

    def get_arp_table(self, num_entries: int=256, verbose: int=0) -> dict:
        """Read the ARP table from the FPGA return a dict

        Parameters
        ----------
        Optionals
        ---------
        num_entries: int
            number of entries in the table to be consider when printing

        Returns
        -------
        Prints the content of valid entries in the ARP in a friendly way
        """

        if not isinstance(num_entries, int):
            raise ValueError("Number of entries must be integer.")
        elif num_entries < 0:
            raise ValueError("Number of entries cannot be negative.")
        elif num_entries > 256:
            raise ValueError("Number of entries cannot be bigger than 256.")

        mac_addr_offset = self.registers['arp_mac_addr_offset']['offset']
        ip_addr_offset = self.registers['arp_ip_addr_offset']['offset']
        valid_addr_offset = self.registers['arp_valid_offset']['offset']

        arptable = dict()

        valid_entry = None
        for i in range(num_entries):
            if (i % 4) == 0:
                valid_entry = self.read(valid_addr_offset + (i // 4) * 4)

            isvalid = (valid_entry >> ((i % 4) * 8)) & 0x1
            if isvalid or verbose > 0:
                mac_lsb = self.read(mac_addr_offset + (i * 2 * 4))
                mac_msb = self.read(mac_addr_offset + ((i * 2 + 1) * 4))
                ip_addr = self.read(ip_addr_offset + (i * 4))
                mac_addr = (2 ** 32) * mac_msb + mac_lsb
                mac_hex = "{:012x}".format(
                    _byte_ordering_endianess(mac_addr, 6))
                mac_str = ":".join(
                    mac_hex[i: i + 2] for i in range(0, len(mac_hex), 2)
                )
                ip_addr_print = _byte_ordering_endianess(ip_addr)
                arptable[i] = {
                    "MAC address": mac_str,
                    "IP address": str(ipaddress.IPv4Address(ip_addr_print))
                }

        headers = ["Index", "MAC Address", "IP Address"]
        table_data = []
        for key, value in arptable.items():
            mac_address = value["MAC address"]
            ip_address = value["IP address"]
            table_data.append([key, mac_address, ip_address])

        print(tabulate(table_data, headers=headers, tablefmt="pretty"))
        #return JSON(arptable, rootname='ARP Table')

    def write_arp_entry(self, mac: str, ip: str):
        """
        Add an entry to the ARP table

        Parameters
        ----------
        mac: str
            MAC address in the format XX:XX:XX:XX:XX:XX
        ip: str
            IP address in the format XXX.XXX.XXX.XXX

        Note, VNx requires all IPs in the ARP table to be in the same
        /24 subnet (mask 255.255.255.0) as the IP assigned to the FPGA port.

        There are 256 entries in the ARP table, one for each possible IP
        in the subnet, the least significant 8 bits of the IP are used to
        index into the ARP table.
        """

        if not isinstance(mac, str):
            raise ValueError("MAC address must be a string.")
        elif not isinstance(ip, str):
            raise ValueError("IP address must be a string.")

        mac_int = int("0x{}".format(mac.replace(":", "")), 16)
        big_mac_int = _byte_ordering_endianess(mac_int, 6)
        mac_msb = (big_mac_int >> 32) & 0xFFFFFFFF
        mac_lsb = big_mac_int & 0xFFFFFFFF

        ip_int = int(ipaddress.IPv4Address(ip))
        big_ip_int = _byte_ordering_endianess(ip_int, 4)

        mac_addr_offset = self.registers['arp_mac_addr_offset']['offset']
        ip_addr_offset = self.registers['arp_ip_addr_offset']['offset']
        valid_addr_offset = self.registers['arp_valid_offset']['offset']

        i = ip_int % 256
        self.write(ip_addr_offset + (i * 4), big_ip_int)
        self.write(mac_addr_offset + (i * 2 * 4), mac_lsb)
        self.write(mac_addr_offset + ((i * 2 + 1) * 4), mac_msb)

        # Valid
        old_valid_entry = self.read(valid_addr_offset + (i // 4) * 4)
        this_valid = 1 << ((i % 4) * 8)
        self.write(valid_addr_offset + (i // 4) * 4,
                   old_valid_entry | this_valid)

    def invalidate_arp_table(self):
        """
        Clear the ARP table
        """
        valid_addr_offset = self.registers['arp_valid_offset']['offset']

        for i in range(0, 256//4, 4):
            self.write(valid_addr_offset + i, 0)

    def arp_discovery(self):
        """
        Launch ARP discovery
        """

        # The ARP discovery is trigger with the rising edge
        self.write(self.registers['arp_discovery']['offset'], 0)
        self.write(self.registers['arp_discovery']['offset'], 1)
        self.write(self.registers['arp_discovery']['offset'], 0)

    def get_network_info(self) -> dict:
        """Returns a dictionary with the current configuration
        """
        mac_addr = int(self.read_long(self.registers['mac_address']['offset']))
        ip_addr = int(self.read(self.registers['ip_address']['offset']))
        ip_gw = int(self.read(self.registers['gateway']['offset']))
        ip_mask = int(self.read(self.registers['ip_mask']['offset']))

        mac_hex = "{:012x}".format(mac_addr)
        mac_str = ":".join(mac_hex[i: i + 2]
                           for i in range(0, len(mac_hex), 2))

        config = {
            "HWaddr": mac_str,
            "inet addr": str(ipaddress.IPv4Address(ip_addr)),
            "gateway addr": str(ipaddress.IPv4Address(ip_gw)),
            "Mask": str(ipaddress.IPv4Address(ip_mask)),
        }
        print(f'{config=}')
        return JSON(config, rootname='Network Information')

    def set_ip_address(self, ipaddrsrt, gwaddr="None", debug=False):
        """
        Update IP address as well as least significant octet of the
        MAC address with the least significant octet of the IP address

        Parameters
        ----------
        ipaddrsrt : string
            New IP address

        gwaddr : string
            New IP gateway address, if not defined a default gateway is used
        debug: bool
            if enable it will return the current configuration

        Returns
        -------
        Current interface configuration only if debug == True

        """

        if not isinstance(ipaddrsrt, str):
            raise ValueError("ipaddrsrt must be an string type")

        if not isinstance(gwaddr, str):
            raise ValueError("gwaddr must be an string type")

        if not isinstance(debug, bool):
            raise ValueError("debug must be a bool type")

        ipaddr = int(ipaddress.IPv4Address(ipaddrsrt))
        self.write(self.registers['ip_address']['offset'], ipaddr)
        if gwaddr == "None":
            self.write(self.registers['gateway']['offset'], (ipaddr & 0xFFFFFF00) + 1)
        else:
            self.write(self.registers['gateway']['offset'], int(ipaddress.IPv4Address(gwaddr)))


        #currentMAC = int(self.read(self.registers['mac_address']['offset']))
        #newMAC = (currentMAC & 0xFFFFFFFFF00) + (ipaddr & 0xFF)
        #self.write(self.registers['mac_address']['offset'], newMAC)

        if debug:
            return self.get_network_info()

    def set_mac_address(self, mac_addr: str):
        """ Update the MAC address of the interface

        Parameters
        ----------
        mac_addr : str
            MAC address in the format XX:XX:XX:XX:XX:XX
        """
        if not isinstance(mac_addr, str):
            raise ValueError("MAC address must be a string.")

        mac_int = int("0x{}".format(mac_addr.replace(":", "")), 16)
        mac_low = mac_int & 0xFFFFFFFF
        mac_high = (mac_int >> 32) & 0xFFFFFFFF
        self.write(self.registers['mac_address']['offset'], mac_low)
        self.write(self.registers['mac_address']['offset'] + 4, mac_high)

    def reset_debug_stats(self) -> None:
        """Reset debug probes
        """

        self.write(self.registers['debug_reset_counters']['offset'], 1)

    def get_debug_stats(self, debug: bool=True) -> dict:
        """Return a dictionary with the value of the Network Layer probes"""

        rmap = self.registers
        probes = dict()
        probes["tx_path"] = dict()
        probes["rx_path"] = dict()

        probes["rx_path"] = {
            "ethernet": {
                "packets": int(self.read(rmap['eth_in_packets']['offset'])),
                "bytes": int(self.read(rmap['eth_in_bytes']['offset'])),
                "cycles": int(self.read(rmap['eth_in_cycles']['offset']))
            },
            "packet_handler": {
                "packets": int(self.read(rmap['pkth_in_packets']['offset'])),
                "bytes": int(self.read(rmap['pkth_in_bytes']['offset'])),
                "cycles": int(self.read(rmap['pkth_in_cycles']['offset']))
            },
            "arp": {
                "packets": int(self.read(rmap['arp_in_packets']['offset'])),
                "bytes": int(self.read(rmap['arp_in_bytes']['offset'])),
                "cycles": int(self.read(rmap['arp_in_cycles']['offset']))
            },
            "icmp": {
                "packets": int(self.read(rmap['icmp_in_packets']['offset'])),
                "bytes": int(self.read(rmap['icmp_in_bytes']['offset'])),
                "cycles": int(self.read(rmap['icmp_in_cycles']['offset']))
            },
            "udp": {
                "packets": int(self.read(rmap['udp_in_packets']['offset'])),
                "bytes": int(self.read(rmap['udp_in_bytes']['offset'])),
                "cycles": int(self.read(rmap['udp_in_cycles']['offset']))
            },
            "app": {
                "packets": int(self.read(rmap['app_in_packets']['offset'])),
                "bytes": int(self.read(rmap['app_in_bytes']['offset'])),
                "cycles": int(self.read(rmap['app_in_cycles']['offset']))
            }
        }

        probes['tx_path'] = {
            "arp": {
                "packets": int(self.read(rmap['arp_out_packets']['offset'])),
                "bytes": int(self.read(rmap['arp_out_bytes']['offset'])),
                "cycles": int(self.read(rmap['arp_out_cycles']['offset']))
            },
            "icmp": {
                "packets": int(self.read(rmap['icmp_out_packets']['offset'])),
                "bytes": int(self.read(rmap['icmp_out_bytes']['offset'])),
                "cycles": int(self.read(rmap['icmp_out_cycles']['offset']))
            },
            "ethernet_header_inserter": {
                "packets": int(self.read(rmap['ethhi_out_packets']['offset'])),
                "bytes": int(self.read(rmap['ethhi_out_bytes']['offset'])),
                "cycles": int(self.read(rmap['ethhi_out_cycles']['offset']))
            },
            "ethernet": {
                "packets": int(self.read(rmap['eth_out_packets']['offset'])),
                "bytes": int(self.read(rmap['eth_out_bytes']['offset'])),
                "cycles": int(self.read(rmap['eth_out_cycles']['offset']))
            },
            "udp": {
                "packets": int(self.read(rmap['udp_out_packets']['offset'])),
                "bytes": int(self.read(rmap['udp_out_bytes']['offset'])),
                "cycles": int(self.read(rmap['udp_out_cycles']['offset']))
            },
            "app": {
                "packets": int(self.read(rmap['app_out_packets']['offset'])),
                "bytes": int(self.read(rmap['app_out_bytes']['offset'])),
                "cycles": int(self.read(rmap['app_out_cycles']['offset']))
            }
        }

        for path, stats in probes.items():
            table_data = []
            for protocol, v in stats.items():
                tot_bytes = v['bytes']
                tot_cycles = v['cycles']
                thr_bs = 0
                if tot_cycles != 0:
                    tot_time = (1 / (390.625 * 10 ** 6)) * tot_cycles
                    thr_bs = (tot_bytes * 8) / tot_time
                table_data.append([protocol, v['packets'], tot_bytes, tot_cycles, f'{thr_bs/10**6:.2f}'])

            print(f"Debug {path} probes")
            print(tabulate(table_data, headers=[f'Probe {path}', 'Packets', 'Bytes', 'Cycles', 'BW (Mb/s)'], tablefmt='pretty'))

        return JSON(probes, rootname='debug_probes')

    @property
    def get_freq(self):
        return int(self.read(self.registers['frequency']['offset']))


class TgMode(Enum):
    """Supported Traffic generator Modes"""
    PRODUCER = 0
    LATENCY = 1
    LOOPBACK = 2
    CONSUMER = 3


class TrafficGenerator(DefaultIP):
    """ This class wraps the common function of the Traffic Generator IP
    """

    bindto = ["xilinx.com:kernel:traffic_generator:1.0"]

    def __init__(self, description):
        super().__init__(description=description)
        self.start = self._call = self._start_sw = self.start_sw = self.call = self._start_ert
        self.freq = None

    def _start_ert(self, mode: TgMode, dest: int=0, packets: int=None,
              beats: int=None, tbwp: int=None):
        """Starts the Traffic generator

        Parameters
        ----------
        mode: TgMode
            Operation mode
        dest: int
            Index in the socket table

        Optional
        --------
        packets: int
            Number of packets
        num_beats: int
            Number of transactions per piece of payload
        tbwp:
            Clock ticks between two consecutive payload packets
        """
        if mode == TgMode.PRODUCER or mode == TgMode.LATENCY:
            if packets is None:
                raise RuntimeError("packets must be specified when mode is {}"
                                   .format(mode))
            elif beats is None:
                raise RuntimeError("beats must be specified when mode is {}"
                                   .format(mode))
            elif tbwp is None:
                raise RuntimeError("tbwp must be specified when mode is {}"
                                   .format(mode))

            self.register_map.number_packets = packets
            self.register_map.number_beats = beats
            self.register_map.time_between_packets = tbwp

        self.register_map.mode = int(mode.value)
        self.register_map.dest_id = dest
        self.register_map.CTRL.AP_START = 1

    def reset_fsm(self):
        """Reset internal FSM"""
        self.register_map.reset_fsm = 1

    def compute_app_throughput(self, direction: str="rx") -> float:
        """
        Read the application monitoring registers and compute
        throughput, it also returns other useful information

        Parameters
        ----------
        direction: string
            'rx' or 'tx'

        Returns
        -------
        Total number of packets seen by the monitoring probe,
        throughput and total time
        """

        if direction not in ["rx", "tx"]:
            raise ValueError(
                "Only 'rx' and 'tx' strings are supported \
                on direction argument"
            )

        if direction == "rx":
            tot_bytes = int(self.register_map.in_traffic_bytes)
            tot_cycles = int(self.register_map.in_traffic_cycles)
            tot_pkts = int(self.register_map.in_traffic_packets)
        else:
            tot_bytes = int(self.register_map.out_traffic_bytes)
            tot_cycles = int(self.register_map.out_traffic_cycles)
            tot_pkts = int(self.register_map.out_traffic_packets)

        tot_time = (1 / (self.freq * 10 ** 6)) * tot_cycles
        thr_bs = (tot_bytes * 8) / tot_time

        return tot_pkts, thr_bs / (10 ** 9), tot_time

    def reset_stats(self):
        """
        Reset embedded probes
        """
        self.register_map.debug_reset = 1


class CounterIP(DefaultIP):
    """ This class wraps the common function of counter IP

    """

    bindto = ["xilinx.com:hls:krnl_counters:1.0"]

    def __init__(self, description):
        super().__init__(description=description)
        self._fullpath = description['fullpath']
        self.start = self.start_sw = self.start_none = \
            self.start_ert = self.call

    def _setup_packet_prototype(self):
        pass

    def call(self, *args, **kwargs):
        raise RuntimeError("{} is a free running kernel and cannot be "
                           "starter or called".format(self._fullpath))

    @property
    def counters(self):
        """ Return counters

        """

        counters = {
            'packets': int(self.register_map.packets),
            'beats': int(self.register_map.beats),
            'bytes': int(self.register_map.bytes),
        }

        return counters

    def reset_counters(self):
        """ Reset internal counters

        """

        self.register_map.reset = 0
        self.register_map.reset = 1
        self.register_map.reset = 0


class CollectorIP(DefaultIP):
    """ This class wraps the common function the collector Kernel

    """

    bindto = ["xilinx.com:hls:collector:1.0"]

    def __init__(self, description):
        super().__init__(description=description)

    @property
    def received_packets(self):
        # When a register is written by the kernel for non free running kernels
        # the default offset refers to the value that the kernel reads
        # the actual register where the kernel writes is not exposed in the
        # signature, so we need to compute the offset and use mmio to read it

        rx_pkts_offset = self.register_map.received_packets.address + \
            self.register_map.received_packets.width//8 + 4
        return self.read(rx_pkts_offset)