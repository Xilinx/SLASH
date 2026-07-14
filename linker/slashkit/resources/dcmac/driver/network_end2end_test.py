# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

import argparse
import time
from dcmac_mmio import DCMAC
from utils import add_common_args, get_ip_offset
from udp_utils import NetworkLayer
from trafficgen import TrafficProducer

"""This file aims at doing a test of the Ethernet or UDP layer between two interfaces in
board, interface 0 and 2. It will initialize the DCMAC and then setup the
interfaces IP, MAC addresses as well as the UDP socket table.
"""

DCMAC_BASEADDR = 0x200_0000
TRAFFICGEN_BASEADDR = 0x400_2000
NL_BASEADDR = 0x400_0000


def main(args):
    dcmac0 = DCMAC(args.dev, base_offset=get_ip_offset(DCMAC_BASEADDR, 0))
    dcmac1 = DCMAC(args.dev, base_offset=get_ip_offset(DCMAC_BASEADDR, 1))

    print(f'{dcmac0.link_up=}')
    print(f'{dcmac1.link_up=}')

    if not (dcmac0.link_up and dcmac1.link_up):
        print('Link not detected in at least one of the DCMACs')
        return

    # reset TX first then RX
    if args.udp:
        """Basic network layer config"""
        nl0 = NetworkLayer(args.dev, base_offset=get_ip_offset(NL_BASEADDR, 0))
        nl1 = NetworkLayer(args.dev, base_offset=get_ip_offset(NL_BASEADDR, 2))

        print(f'nl0._base_offset=0x{nl0._base_offset:0X}')
        print(f'nl1._base_offset=0x{nl1._base_offset:0X}')

        ip_if0 = '192.168.10.5'
        ip_if1 = '192.168.10.6'
        nl0.set_ip_address(ip_if0)
        nl1.set_ip_address(ip_if1)
        nl0.set_mac_address('b8:3f:d2:24:51:c0')
        nl1.set_mac_address('b8:3f:d2:24:51:c1')

        print(f'NL0: {nl0.get_network_info()}')
        print(f'NL1: {nl1.get_network_info()}')

        """Reset debug stats"""
        nl0.reset_debug_stats()
        nl1.reset_debug_stats()

        """Start ARP Discovery"""
        nl0.arp_discovery()
        time.sleep(1)
        nl1.arp_discovery()
        time.sleep(1)

        print(f'NL0 ARP Table: {nl0.get_arp_table(12, verbose=1)}')
        print(f'NL1 ARP Table: {nl1.get_arp_table(12, verbose=1)}')

        """Populate socket table"""
        port_tx = 50446
        port_rx = 60133
        nl0.sockets[0] = (ip_if1, port_tx, port_rx, True)
        nl0.populate_socket_table(debug=True)
        nl1.sockets[0] = (ip_if0, port_rx, port_tx, True)
        nl1.populate_socket_table(debug=True)

    if not args.skip_clear:
        # Discard the link bring-up transient (e.g. the initial BAD_CODE_COUNT)
        # so the post-traffic numbers reflect only the packets we send below.
        for d in (dcmac0, dcmac1):
            d.pm_tick(direction='tx')
            d.pm_tick(direction='rx')

    """Now we can generate some traffic"""

    tp0 = TrafficProducer(args.dev, resource=0, base_offset=0x004C_0000)
    tp1 = TrafficProducer(args.dev, resource=0, base_offset=0x0050_0000)

    tp0.flits = 22
    tp0.dest = 0
    tp0.start()
    time.sleep(1)

    tp1.flits = 22
    tp1.dest = 0
    tp1.start()
    time.sleep(1)

    if args.udp:
        """Get Statistics"""
        print('\n')
        nl0.get_debug_stats(True)
        print('\n')
        nl1.get_debug_stats(True)
        print('\n')

    print(f'{dcmac0.tx_stats(verbose=1)=}')
    print(f'{dcmac0.rx_stats(verbose=1)=}')

    print(f'{dcmac1.tx_stats(verbose=1)=}')
    print(f'{dcmac1.rx_stats(verbose=1)=}')

    if args.udp:
        print(f'{nl0.get_freq=}')
        print(f'{nl1.get_freq=}')


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-u', '--udp', action='store_true',
                        help='Use UDP logic')
    parser.add_argument('--skip-clear', action='store_true',
                        help='Skip reading/clearing DCMAC stats before sending traffic')
    parser = add_common_args(parser, verbose=True)
    args = parser.parse_args()
    main(args)
