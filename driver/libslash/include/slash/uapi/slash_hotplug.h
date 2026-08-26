/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/**
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * This file is dual-licensed: you may select either the GNU General Public
 * License version 2 (GPL-2.0-only) or the MIT License.  See the LICENSE
 * files in the repository root for the full text of each license.
 */

/**
 * @file slash_hotplug.h
 *
 * User-kernel ABI for the slash hotplug control device.
 *
 * The slash hotplug subsystem manages the PCIe-level lifecycle of FPGA
 * devices: removing them from the PCI bus, triggering Secondary Bus
 * Resets (SBR), rescanning, and performing full hot-plug sequences.
 *
 * This is essential for FPGA reconfiguration workflows where the device
 * identity or BAR layout may change after loading a new bitstream, and
 * the kernel must re-enumerate the device.
 *
 * All communication goes through a dedicated character device named
 * SLASH_HOTPLUG_DEVICE_NAME ("/dev/slash_hotplug").
 *
 * A typical FPGA reconfiguration flow uses these operations in order:
 *
 *   1. REMOVE all PCI functions (PF0, PF1, PF2 …) from the bus.
 *   2. TOGGLE_SBR on the upstream bridge to reset the device and wait
 *      for PDI reload and stable link training.
 *   3. RESCAN the PCI bus to discover the new configuration.
 *
 * For a simple device teardown/re-add (no reset or bitstream change),
 * REMOVE → RESCAN is sufficient.
 */

#ifndef SLASH_HOTPLUG_UAPI_H
#define SLASH_HOTPLUG_UAPI_H

#include <linux/types.h>

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif /* __KERNEL__ */

/** Name of the hotplug control character device (appears under /dev/). */
#define SLASH_HOTPLUG_DEVICE_NAME "slash_hotplug"

/** Maximum length (including NUL) of a PCI BDF string in hotplug requests. */
#define SLASH_HOTPLUG_BDF_LEN 32

/**
 * @brief Identify a device for a hotplug operation.
 *
 * If \@bdf is empty and multiple devices are tracked, the kernel
 * returns -EOPNOTSUPP; the caller must specify the BDF explicitly.
 * -ENODEV is returned if no devices are tracked at all.
 */
struct slash_hotplug_device_request {
    __u32 size; /**< Struct size for ABI versioning. */
    /**
     * PCI Bus/Device/Function string (e.g. "0000:03:00.0"), NUL-terminated.
     * If the string is empty, the kernel targets the only currently
     * tracked device (convenient for single-device systems).
     */
    char bdf[SLASH_HOTPLUG_BDF_LEN];
};

/** ioctl magic number for hotplug commands (uses 'w', distinct from 'v'). */
#define SLASH_HOTPLUG_IOCTL_MAGIC 'w'

/**
 * Rescan the PCI bus to discover new or reconfigured devices.
 * Takes no per-device argument.
 */
#define SLASH_HOTPLUG_IOCTL_RESCAN     _IO(SLASH_HOTPLUG_IOCTL_MAGIC, 0x30)

/**
 * Remove a device from the PCI bus.
 * The device is identified by the \@bdf in the request struct.
 */
#define SLASH_HOTPLUG_IOCTL_REMOVE     _IOW(SLASH_HOTPLUG_IOCTL_MAGIC, 0x31, struct slash_hotplug_device_request)

/**
 * Toggle a Secondary Bus Reset (SBR) on the device's upstream port.
 *
 * A single ioctl call performs the full SBR sequence on the upstream
 * bridge through pci_bridge_secondary_bus_reset().  On hotplug-capable
 * ports, expected link-change events are suppressed so pciehp does not
 * remove or re-enumerate the slot during the V80's full PDI reload.  The
 * secondary bus must be empty before the call; otherwise it returns -EBUSY.
 * The ioctl blocks for at least five seconds and, when supported by the port,
 * until DLL Link Active remains asserted for 100 ms.  It can return
 * -ETIMEDOUT after an additional 25 seconds if the link does not recover.
 */
#define SLASH_HOTPLUG_IOCTL_TOGGLE_SBR _IOW(SLASH_HOTPLUG_IOCTL_MAGIC, 0x32, struct slash_hotplug_device_request)

/**
 * Perform a full hot-plug cycle on the device.
 *
 * A "full hot-plug" performs REMOVE then RESCAN in one atomic step:
 * the kernel calls pci_stop_and_remove_bus_device() followed by
 * pci_rescan_bus() on the root-port's subordinate bus.  It does
 * **not** include a Secondary Bus Reset; use TOGGLE_SBR separately
 * before HOTPLUG if a reset is required.
 */
#define SLASH_HOTPLUG_IOCTL_HOTPLUG    _IOW(SLASH_HOTPLUG_IOCTL_MAGIC, 0x33, struct slash_hotplug_device_request)

#endif /* SLASH_HOTPLUG_UAPI_H */
