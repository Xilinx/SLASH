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
 * @file slash_hotplug_driver.h
 *
 * Kernel-internal interface for the SLASH hotplug subsystem.
 *
 * The hotplug subsystem manages the PCIe-level lifecycle of SLASH FPGA
 * devices: removing them from the bus, performing Secondary Bus Resets,
 * and rescanning.  This is essential for FPGA reconfiguration workflows
 * where loading a new bitstream requires re-enumerating the device.
 *
 * A single misc device (/dev/slash_hotplug) handles ioctls.  All
 * operations that target a specific device require an explicit BDF.
 */

#ifndef SLASH_HOTPLUG_DRIVER_H
#define SLASH_HOTPLUG_DRIVER_H

/**
 * slash_hotplug_init() - Register the hotplug misc device.
 *
 * Creates /dev/slash_hotplug.
 *
 * Return: 0 on success, negative errno on failure.
 */
int slash_hotplug_init(void);

/**
 * slash_hotplug_exit() - Unregister the hotplug misc device.
 */
void slash_hotplug_exit(void);

#endif /* SLASH_HOTPLUG_DRIVER_H */
