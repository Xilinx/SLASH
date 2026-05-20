/**
 * Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if
 * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * @file slash_qdma.h
 *
 * QDMA subsystem init/exit interface for the SLASH kernel module.
 *
 * The QDMA subsystem manages the Xilinx QDMA IP on PF1, providing
 * queue-pair-based DMA transfers between host memory and the FPGA
 * fabric.  It wraps the libqdma library from the Xilinx reference
 * driver (submodules/qdma_drv).
 *
 * Initialization is split from PCI probe: slash_qdma_init() sets up
 * the libqdma library and registers a separate PCI driver for PF1,
 * while the per-device work happens in the PCI probe callback
 * (internal to slash_qdma.c).
 */

#ifndef SLASH_QDMA_H
#define SLASH_QDMA_H

#include <linux/module.h>

/**
 * slash_qdma_init() - Initialize the QDMA subsystem.
 * @num_threads: Number of worker threads for libqdma's internal
 *               processing (passed to libqdma_init()).
 * @debugfs:     Optional debugfs mount path for libqdma diagnostics.
 *               Pass NULL to disable debugfs integration.
 *
 * Initializes the libqdma library and registers a PCI driver that
 * will probe QDMA-capable functions (PF1) on SLASH devices.
 *
 * Must be called before slash_pcie_init(), because PCI probe for PF2
 * may trigger activity that depends on the QDMA subsystem being ready.
 *
 * Return: 0 on success, negative errno on failure.
 */
int __init slash_qdma_init(unsigned int num_threads, char *debugfs);

/**
 * slash_qdma_exit() - Tear down the QDMA subsystem.
 *
 * Unregisters the QDMA PCI driver, closes all open QDMA devices,
 * removes all queue pairs, and shuts down the libqdma library.
 *
 * Must be called after slash_pcie_exit() to ensure the control
 * function is cleaned up before the QDMA function.
 */
void slash_qdma_exit(void);

#endif /* SLASH_QDMA_H */
