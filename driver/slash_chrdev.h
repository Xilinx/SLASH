/**
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
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
 * @file slash_chrdev.h
 *
 * Shared character-device range, class, and stable board-slot allocator.
 */

#ifndef SLASH_CHRDEV_H
#define SLASH_CHRDEV_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/pci.h>

#include "slash_config.h"

/** Reserve the shared device-number range and create /sys/class/slash. */
int __init slash_chrdev_init(void);
/** Destroy the shared class, range, and module-lifetime board map. */
void slash_chrdev_exit(void);

/**
 * slash_chrdev_add() - Add one cdev and its BDF-specific class device.
 *
 * Return: Class device pointer, or ERR_PTR on failure.
 */
struct device *slash_chrdev_add(struct cdev *cdev,
                                const struct file_operations *fops,
                                unsigned int minor,
                                struct device *parent,
                                void *drvdata,
                                const char *name);
/** Remove a class device and its cdev. */
void slash_chrdev_del(struct cdev *cdev, struct device *device);

/**
 * slash_chrdev_board_get() - Get the stable card number for a PCI endpoint.
 *
 * PF1 and PF2 at the same domain:bus:slot share a number.  The mapping remains
 * allocated until module exit.
 *
 * Return: Card number in [0, SLASH_MAX_CARDS), or negative errno.
 */
int slash_chrdev_board_get(struct pci_dev *pdev);
/** Mark one endpoint inactive without discarding its stable mapping. */
void slash_chrdev_board_put(struct pci_dev *pdev);

#endif /* SLASH_CHRDEV_H */
