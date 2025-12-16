# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

import argparse
import time
from dcmac_init import dcmac_logic_init
from dcmac_mmio import DCMAC
from utils import add_common_args, get_ip_offset
from udp_utils import NetworkLayer, RTLTrafficGenerator, TgMode

"""This file aims at benchmarking thorughput on the V80 using the UDP stack. 
It uses interface 0 and 2. It will initialize the DCMAC and then setup the
interfaces IP, MAC addresses as well as the UDP socket table and traffic generators.
"""

DCMAC_BASEADDR = 0x200_0000
TRAFFICGEN_BASEADDR = 0x400_2000
NL_BASEADDR = 0x400_0000
TG_INCREMENT = 512


class ArgsClass:
    dcmac = 0
    init = False
    print = 1
    dev = None
    verbose = 1
    loopback = None
    keep_alive = 0
    align_rx = 1
    traffic_test = 0


def main(args):
    """Initialize DCMAC in each interface"""
    init_args = ArgsClass()
    init_args.dev = args.dev
    """Init DCMAC 0"""
    dcmac_logic_init(init_args)

    """Init DCMAC 0"""
    init_args.dcmac = 1
    dcmac_logic_init(init_args)

    # reset TX first then RX

    """Basic network layer config"""
    nl0 = NetworkLayer(args.dev, get_ip_offset(NL_BASEADDR, 0))
    nl1 = NetworkLayer(args.dev, get_ip_offset(NL_BASEADDR, 2))

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
    tx_socket = 10
    rx_socket = 3
    nl0.sockets[tx_socket] = (ip_if1, port_tx, port_rx, True)
    nl0.populate_socket_table(debug=True)
    nl1.sockets[rx_socket] = (ip_if0, port_rx, port_tx, True)
    nl1.populate_socket_table(debug=True)

    """Now we can configure the Traffic Generators"""

    freq = 200
    tgen_tx_id = 3
    tgen_rx_id = 3
    tgen_tx = RTLTrafficGenerator(args.dev,
                                  get_ip_offset(TRAFFICGEN_BASEADDR + TG_INCREMENT * tgen_tx_id, 0))
    tgen_rx = RTLTrafficGenerator(args.dev,
                                  get_ip_offset(TRAFFICGEN_BASEADDR + TG_INCREMENT * tgen_rx_id, 2))

    tgen_tx.freq = freq
    tgen_rx.freq = freq
    # Set RX in consumer mode
    tgen_rx.consumer_mode()

    # overhead is UDP (8), IP (20), Ethernet(14) and FCS (4), IFG (12), preamble (7), start frame delimiter (1)
    overhead = 8 + 20 + 14 + 4 + 12 + 7 + 1
    experiment_dict = {}
    local_dict = {}

    for pkt in [1_000]:
        local_dict = {}
        for i in range(22, 23):
            tgen_tx.reset_stats()
            tgen_rx.reset_stats()
            beats = i + 1
            tgen_tx.start(TgMode.PRODUCER, tx_socket, pkt, beats, 0)
            while int(tgen_tx.read_output_packet()) != pkt:
                time.sleep(0.8)

            # Get results from local and remote worker
            rx_tot_pkt, rx_thr, rx_time = tgen_rx.compute_app_throughput('rx')
            tx_tot_pkt, tx_thr, tx_time = tgen_tx.compute_app_throughput('tx')
            #Create dict entry for this particular experiment
            entry_dict = {'size': (beats * 64), 'rx_pkts' : rx_tot_pkt, 'tx_thr': tx_thr, 'rx_thr': rx_thr}
            local_dict[beats] = entry_dict
            # Reset probes to prepare for next computation
            theoretical = (beats * 64 * 100)/((beats*64) + overhead)
            print(f'Sent {pkt:14,} size: {beats*64:4}-Byte done!	'
                  f'Got {rx_tot_pkt:14,} took {rx_time:8.4f} sec, '
                  f'thr: {rx_thr:.3f} Gbps, theoretical: {theoretical:.3f} Gbps'
                  f', difference: {theoretical-rx_thr:.4f} Gbps')

            time.sleep(0.5)
        experiment_dict[pkt] = local_dict
    if args.verbose > 0:
        dcmac0 = DCMAC(args.dev, get_ip_offset(DCMAC_BASEADDR, 0))
        dcmac1 = DCMAC(args.dev, get_ip_offset(DCMAC_BASEADDR, 1))

        print(f'{dcmac0.tx_stats(verbose=1)=}')
        print(f'{dcmac0.rx_stats(verbose=1)=}')

        print(f'{dcmac1.tx_stats(verbose=1)=}')
        print(f'{dcmac1.rx_stats(verbose=1)=}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser = add_common_args(parser, verbose=True)
    args = parser.parse_args()
    main(args)
