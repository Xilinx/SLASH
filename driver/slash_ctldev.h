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
 * @file slash_ctldev.h
 *
 * Control device interface for the SLASH kernel module.
 *
 * Each probed PF2 device gets a control misc device (/dev/slash_ctl<N>)
 * that exposes device identity, BAR properties, and dma-buf-backed BAR
 * mappings to userspace via ioctl.
 */

#ifndef SLASH_CTLDEV_H
#define SLASH_CTLDEV_H

#include <linux/dma-buf.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>

/**
 * struct slash_ctldev_bar - Per-BAR metadata cached during probe.
 * @active: 1 if the BAR is present (has a non-zero start address).
 * @mmio:   1 if the BAR is a memory-mapped I/O region (IORESOURCE_MEM).
 *          I/O-port BARs are tracked but not exported to userspace.
 * @start:  Physical/bus start address of the BAR.
 * @end:    Physical/bus end address of the BAR (inclusive).
 * @len:    Size of the BAR in bytes.
 * @dmabuf: dma-buf exporter for this BAR, or NULL if the BAR is not
 *          MMIO.  Created during probe, destroyed during remove.
 */
struct slash_ctldev_bar {
    unsigned int active : 1;
    unsigned int mmio : 1;
    resource_size_t start;
    resource_size_t end;
    resource_size_t len;

    struct dma_buf *dmabuf;
};

/**
 * struct slash_ctldev - Per-device control device state.
 * @pdev: Back-pointer to the PCI device this control device manages.
 * @misc: Kernel misc device registered as /dev/slash_ctl<N>.
 * @bars: Cached BAR metadata for all standard PCI BARs (0-5).
 *
 * Allocated during probe, stored via pci_set_drvdata(), and freed
 * during remove.
 */
struct slash_ctldev {
    struct pci_dev *pdev;
    struct miscdevice misc;
    struct slash_ctldev_bar bars[PCI_STD_NUM_BARS];
};

/**
 * slash_ctldev_create() - Create a control device for a PCI function.
 * @pdev: PCI device to create the control device for.
 *
 * Probes BARs, creates dma-buf exporters for MMIO BARs, and registers
 * a misc device.  The control device state is stored as PCI driver
 * data on @pdev.
 *
 * Return: 0 on success, negative errno on failure.
 */
int slash_ctldev_create(struct pci_dev *pdev);

/**
 * slash_ctldev_destroy() - Destroy a control device.
 * @pdev: PCI device whose control device should be torn down.
 *
 * Deregisters the misc device, destroys dma-buf exporters, and frees
 * the control device state.
 */
void slash_ctldev_destroy(struct pci_dev *pdev);

#endif /* SLASH_CTLDEV_H */
