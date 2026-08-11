/**
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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
 * @file slash_config.h
 *
 * Build-time constants for the SLASH kernel module.
 *
 * Defines PCI identity, physical-function assignments, character-device
 * naming conventions, and the kernel log prefix used throughout the
 * driver.
 *
 * The SLASH design exposes two PCI physical functions per card:
 *
 *   - **PF1** (device 0x50C1) — QDMA function.  Hosts the Xilinx QDMA
 *     IP used for high-throughput DMA transfers between host memory and
 *     the FPGA fabric.
 *
 *   - **PF2** (device 0x50C2) — Control function.  Exposes PCI BARs
 *     that the host can mmap for register-level MMIO access to the
 *     FPGA design.
 *
 * Both functions share vendor ID 0x10EE (AMD/Xilinx).
 *
 * For backward compatibility the driver also binds the legacy device IDs
 * used by pre-compute-platform designs (PF1 0x50B5, PF2 0x50B6) and the
 * AVED/V80P QDMA function (0x50BD).  New designs should use 0x50C1/0x50C2.
 */

#ifndef SLASH_CONFIG_H
#define SLASH_CONFIG_H

/* --- PCI identity for the control function (PF2) --- */

/** AMD/Xilinx PCI vendor ID. */
#define SLASH_PCIE_VENDOR_ID 0x10EE
/** PCI device ID for the V80 SLASH control function. */
#define SLASH_PCIE_DEVICE_ID 0x50C2
/** Legacy PCI device ID for the control function (pre-compute-platform designs). */
#define SLASH_PCIE_DEVICE_ID_LEGACY 0x50B6
/** Physical function number for the control/BAR-access interface. */
#define SLASH_PCIE_PF 2

/* --- PCI identity for the QDMA function (PF1) --- */

/** AMD/Xilinx PCI vendor ID (same as control function). */
#define SLASH_QDMA_PCI_VENDOR_ID 0x10EE
/** PCI device ID for the V80 SLASH QDMA function. */
#define SLASH_QDMA_PCI_DEVICE_ID 0x50C1
/** Legacy PCI device ID for the QDMA function (pre-compute-platform designs). */
#define SLASH_QDMA_PCI_DEVICE_ID_LEGACY 0x50B5
/** PCI device ID for the V80P/AVED QDMA function. */
#define SLASH_AVED_QDMA_PCI_DEVICE_ID 0x50BD
/** Physical function number for the QDMA DMA engine. */
#define SLASH_QDMA_PF 1

/* --- Driver name and character-device naming --- */

/** Short driver name used in log prefixes and sysfs entries. */
#define SLASH_NAME "slash"
/** PCI driver name for the PF2 control function. */
#define SLASH_PCIE_DRV_NAME SLASH_NAME "_ctl"
/** PCI driver name for the PF1 QDMA function. */
#define SLASH_QDMA_DRV_NAME SLASH_NAME "_qdma"

/** Maximum number of distinct board BDFs tracked until module unload. */
#define SLASH_MAX_CARDS 16
/** Shared character-device range: hotplug plus CTL and QDMA per card. */
#define SLASH_CHRDEV_MINORS (1 + 2 * SLASH_MAX_CARDS)

#define SLASH_HOTPLUG_MINOR 0
#define SLASH_CTLDEV_MINOR(card) (2 * (card) + 1)
#define SLASH_QDMA_MINOR(card) (2 * (card) + 2)
/** Recover the board slot from any non-hotplug CTL or QDMA minor. */
#define SLASH_CARD_FROM_MINOR(minor) (((minor) - 1) / 2)

/**
 * Sysfs name format for control character devices.
 * Uses pci_name() (e.g. "0000:03:00.2") — appears in /sys/class/slash.
 */
#define SLASH_CTLDEV_NAME_FMT "slash_ctl_%s"
/**
 * Node name format for control character devices.
 * Uses the board's stable slot — appears as /dev/slash_ctl0, etc.
 */
#define SLASH_CTLDEV_NODENAME_FMT "slash_ctl%d"

/** Sysfs name format for QDMA control devices (/sys/class/slash). */
#define SLASH_QDMA_CTLDEV_NAME_FMT "slash_qdma_ctl_%s"
/** Stable board-slot node name format for QDMA control devices (/dev/). */
#define SLASH_QDMA_CTLDEV_NODENAME_FMT "slash_qdma_ctl%d"

/*
 * Default /dev node permissions (owner read/write only).
 * For production, prefer a udev rule to set permissions instead of
 * changing these constants.
 */
#define SLASH_CHRDEV_MODE 0600

/*
 * Override the kernel's pr_fmt to prefix every pr_info/pr_err/pr_dbg
 * message with "slash:<function_name>: ".
 */
#undef pr_fmt
#define pr_fmt(fmt) "%s:%s: " fmt, SLASH_NAME, __func__

#endif /* SLASH_CONFIG_H */
