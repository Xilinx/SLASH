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
 * @file slash_main.c
 *
 * Module entry point for the SLASH kernel driver.
 *
 * SLASH is a partial-reconfiguration design for AMD Alveo V80 FPGAs.
 * This module manages two PCI physical functions exposed by the design:
 *
 *   - PF1 (QDMA)    — queue-based DMA for bulk data transfer.
 *   - PF2 (Control) — BAR access for register-level MMIO.
 *
 * It also provides a hotplug subsystem for removing, resetting (SBR),
 * and rescanning PCI devices — needed when the FPGA bitstream changes
 * and the device must be re-enumerated.
 *
 * Initialization order matters:
 *
 *   1. **Character devices** — reserve the shared major/minor range and
 *      create the slash class before any endpoint is exposed.
 *
 *   2. **Hotplug** — /dev/slash_hotplug must exist before either PCI
 *      driver is registered.
 *
 *   3. **QDMA / PCIe** — register PF1, then PF2.  Registering either
 *      driver may immediately probe devices already present on the bus.
 *
 * Teardown is the strict reverse order.
 */

#include "slash.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/version.h>

#include "slash_chrdev.h"
#include "slash_pcie.h"
#include "slash_hotplug_driver.h"
#include "slash_qdma.h"

#ifndef SLASH_VERSION_STR
#define SLASH_VERSION_STR "unknown"
#endif

/** Number of worker threads for libqdma's internal processing. */
static unsigned int qdma_num_threads = 8;
/** Optional debugfs mount path for libqdma diagnostics (unused). */
static char *qdma_debugfs_path = NULL;

/**
 * slash_init() - Module initialization.
 *
 * Brings up the three subsystems in dependency order.
 * On failure, tears down any subsystems that were already initialized.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int __init slash_init(void)
{
    int err;

    pr_info("slash: module init\n");

    /* Reserve all stable minors and create the shared class first. */
    err = slash_chrdev_init();
    if (err) {
        pr_err("slash: character-device init failed: %d\n", err);
        return err;
    }

    /* The hotplug endpoint must exist before either PCI probe can run. */
    err = slash_hotplug_init();
    if (err) {
        pr_err("slash: hotplug init failed: %d\n", err);
        goto err_chrdev;
    }

    /* libqdma initialization precedes PF1 PCI-driver registration. */
    err = slash_qdma_init(qdma_num_threads, NULL);
    if (err) {
        pr_err("slash: libqdma init failed: %d\n", err);
        goto err_hotplug;
    }

    /* Register PF2 last; both PCI drivers can now create shared-class nodes. */
    err = slash_pcie_init();
    if (err) {
        pr_err("slash: PCIe init failed: %d\n", err);
        goto err_qdma;
    }

    pr_info("slash: module init complete\n");
    return 0;

err_qdma:
    slash_qdma_exit();
err_hotplug:
    slash_hotplug_exit();
err_chrdev:
    slash_chrdev_exit();
    return err;
}

/**
 * slash_exit() - Module cleanup.
 *
 * Tears down subsystems in reverse initialization order.
 */
static void __exit slash_exit(void)
{
    pr_info("slash: module exit\n");
    slash_pcie_exit();
    slash_qdma_exit();
    slash_hotplug_exit();
    slash_chrdev_exit();
    pr_info("slash: module exit complete\n");
}

module_init(slash_init);
module_exit(slash_exit);

module_param(qdma_num_threads, uint, 0644);
MODULE_PARM_DESC(qdma_num_threads, "Number of libqdma worker threads (default: 8)");
module_param(qdma_debugfs_path, charp, 0644);
MODULE_PARM_DESC(qdma_debugfs_path, "debugfs mount path for libqdma (default: disabled)");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AMD Inc.");
MODULE_DESCRIPTION("SLASH/VRT module");
MODULE_VERSION(SLASH_VERSION_STR);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
