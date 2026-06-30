# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

import argparse
import time
from axi_gt_controller import AxiGTController
from dcmac_mmio import DCMAC
from generic_mmio import hex_or_int
from gpio_monitor import AxiGPIOMonitor
from axigpio_mmio import AxiGpioMMIO
from utils import add_common_args, get_ip_offset

def dcmac_reset_procedure(reset_tx: bool = True, dcmac_idx: int = 0):
    """Reset DCMAC and GTs"""
    global intf_id, dcmac, gt_gpio, monitor, gtdatapath
    print(f'Working with {dcmac_idx=}')
    #  1: reset GTs
    if reset_tx:
        print(" Resetting Tx GTs ", end= "", flush=True)
        gt_gpio.gt_reset = 1
        time.sleep(0.001)
        gt_gpio.gt_reset = 0
    else:
        print(" Resetting only GTs RX datapath ", end= "", flush=True)
        gtdatapath.write(0x0, 0xF)
        time.sleep(0.01)
        gtdatapath.write(0x0, 0x0)

    #  2: Wait for GT reset to finish
    for _ in range(20):
        signed_to_check = [f'gt{intf_id}_tx_reset_done',f'gt{intf_id}_rx_reset_done']
        ready = True
        for signal in signed_to_check:
            ready &= getattr(monitor, signal) == 0xF
        if ready:
            print(f"Done -> ", end= "", flush=True)
            break
        print(".", end= "", flush=True)
        time.sleep(0.1)
    else:
        print(f"GTs not comming out of reset after 2 sec. Exiting...")
        print(f" Debug info: {monitor.gt0_tx_reset_done=}, {monitor.gt1_tx_reset_done=}")
        print(f"             {monitor.gtpowergood=}")
        time.sleep(0.1)
    # time.sleep(0.5)

    #  3: reset DCMAC Tx
    tx_rst_success = None
    if reset_tx:
        print("Resetting DCMAC Tx -> ", end= "", flush=True)
        # status will be cleared after Rx reset, if successful
        tx_rst_success = dcmac.reset_tx(clear_status_history= False)

    #  4: reset DCMAC Rx
    print("Resetting DCMAC Rx ", end= "", flush=True)
    rx_rst_success = dcmac.reset_rx(clear_status_history= True)
    return tx_rst_success, rx_rst_success

def dcmac_logic_init(args):
    global intf_id, dcmac, gt_gpio, monitor, gtdatapath

    intf_id = 0 # TODO: in the future, we'll have 2 interfaces per DCMAC
    dcmac = DCMAC(args.dev, get_ip_offset(0x200_0000, args.dcmac))
    gt_gpio = AxiGTController(args.dev, get_ip_offset(0x204_0000, args.dcmac),
                              gpio_index=0)
    monitor = AxiGPIOMonitor(args.dev, get_ip_offset(0x204_0200, args.dcmac), gpio_index=0)
    gtdatapath = AxiGpioMMIO(args.dev, get_ip_offset(0x204_0400, args.dcmac))

    # Set GT Tx analog front-end swing and pre/post-emphasis:
    # TODO: Fine-tune the following configuration. In general, this achieves alignment,
    # 24 dB SNR and seems to stay aligned for a couple of days
    # These values are now set by default in the GPIO
    #gt_gpio.txprecursor = 6
    #gt_gpio.txmaincursor = 52
    #gt_gpio.txpostcursor = 6

    if args.verbose > 0:
        print(f'{dcmac._base_offset=:#x}')
        print(f'{gt_gpio._base_offset=:#x}')
        print(f'{monitor._base_offset=:#x}')
        print(f'{gtdatapath._base_offset=:#x}')
        print(f'{monitor.dual_dcmac=}')
        print(f'{gt_gpio.txprecursor=}')
        print(f'{gt_gpio.txmaincursor=}')
        print(f'{gt_gpio.txpostcursor=}')

    if args.loopback is not None:
        if args.loopback != args.loopback:
            gt_gpio.loopback = args.loopback
            time.sleep(0.1)
            print(f'Loopback mode set to: {gt_gpio.loopback}')
            args.init = True

    if args.keep_alive:
        print('Keep ALIVE path')
        iters = 0
        init_time = time.time()
        prev_link_up = dcmac.link_up
        while True:
            iters += 1
            if dcmac.link_up:
                if iters % 100 == 0:
                    print(f"\rDCMAC {args.dcmac} link still up after {time.time() - init_time:.1f} s", end="", flush=True)
            else:
                if prev_link_up:
                    print(f" | Link Down |")
                dcmac_reset_procedure(not dcmac.tx_aligned, args.dcmac)
                if dcmac.link_up:
                    print(" | Link up again")
                    init_time = time.time()
            prev_link_up = dcmac.link_up
            time.sleep(0.05)

    # TODO, we need an independent reset TX code
    if args.init or args.align_rx:
        print(f'INIT or ALIGN RX. {args.init=} {args.align_rx=}')
        if args.verbose > 1:
            dcmac.print_config(False)
            print(f"{gt_gpio.loopback=}")
            print(f"{gt_gpio.txprecursor=}")
            print(f"{gt_gpio.txmaincursor=}")
            print(f"{gt_gpio.txpostcursor=}")
            print('\nResetting GT -> DCMAC Tx -> DCMAC Rx')

        NUM_OF_RETRIES = 10
        reset_tx = args.init
        # Iterate through reset routine until MAC is ready or we run out of retries
        for retry_id in range(NUM_OF_RETRIES):
            tx_rst_success, rx_rst_success = dcmac_reset_procedure(reset_tx, args.dcmac)
            tx_rst_success = tx_rst_success if reset_tx else True
            reset_tx = tx_rst_success
            if tx_rst_success and rx_rst_success:
                print(f"DCMAC initialization successful after {retry_id} retries")
                break
        else:
            print(f"DCMAC initialization failed after {NUM_OF_RETRIES} retries. DCMAC state:")
            dcmac.print_status(only_modified_fields=True)
            print(f"Exiting...")
            exit(1)

    if args.verbose > 0:
        dcmac.print_status(only_modified_fields=args.verbose < 2)

        dcmac.tx_stats(0, True, verbose=args.verbose)
        dcmac.rx_stats(0, True, verbose=args.verbose)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-i', '--init', action='store_true',
                        help='Initialize system')
    parser.add_argument('-a', '--align_rx', action='store_true',help='Align RX')
    parser.add_argument('-k', '--keep_alive', action='store_true',help='Keep link alive')
    parser.add_argument('-l', '--loopback', type=hex_or_int,
                        help="Set GT Loopback", default=None)
    parser.add_argument('-t', '--traffic_test', action='store_true',
                        help='Run traffic test')
    # default only status
    parser.add_argument('-p', '--print', action='store_true',
                        help='Print stats')
    parser = add_common_args(parser, verbose=True)

    args = parser.parse_args()
    dcmac_logic_init(args)
