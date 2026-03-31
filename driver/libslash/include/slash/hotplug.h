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
 * @file hotplug.h
 *
 * Userspace API for managing PCIe hot-plug operations on slash devices.
 *
 * This module provides a thin wrapper around the slash hotplug character
 * device (/dev/slash_hotplug).  It handles opening/closing the device
 * node and issuing the four hotplug ioctls defined in the UAPI header:
 * rescan, remove, toggle SBR, and full hot-plug.
 *
 * All functions follow POSIX conventions: return 0 on success, -1 on
 * failure with errno set.  slash_hotplug_open() returns NULL on failure.
 */

#ifndef LIBSLASH_HOTPLUG_H
#define LIBSLASH_HOTPLUG_H

#include "uapi/slash_hotplug.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** Default path to the hotplug character device. */
#define SLASH_HOTPLUG_DEFAULT_PATH "/dev/" SLASH_HOTPLUG_DEVICE_NAME

/**
 * @brief Opaque handle to the hotplug control device.
 */
struct slash_hotplug {
    int fd; /**< File descriptor for the opened hotplug character device. */
};

/**
 * @brief Open the hotplug control device.
 *
 * @param path Path to the character device, or NULL to use
 *             SLASH_HOTPLUG_DEFAULT_PATH ("/dev/slash_hotplug").
 *
 * @return A heap-allocated handle on success, or NULL on failure
 *         (errno is set by open() or calloc()).
 */
struct slash_hotplug *slash_hotplug_open(const char *path); /* NULL means SLASH_HOTPLUG_DEFAULT_PATH */

/**
 * @brief Close the hotplug device and free the handle.
 *
 * @param hotplug Handle returned by slash_hotplug_open().  Must not be used
 *                after this call.  Passing NULL sets errno to EINVAL and
 *                returns -1.
 *
 * @return 0 on success, -1 if close() fails (errno is preserved).
 *         The handle is freed regardless of whether close() succeeds.
 */
int slash_hotplug_close(struct slash_hotplug *hotplug);

/**
 * @brief Trigger a PCI bus rescan.
 *
 * @param hotplug Open hotplug handle.
 *
 * No device BDF is required; the kernel rescans the entire bus.
 *
 * @return 0 on success, -1 on failure.
 */
int slash_hotplug_rescan(struct slash_hotplug *hotplug);

/**
 * @brief Remove a device from the PCI bus.
 *
 * @param hotplug Open hotplug handle.
 * @param bdf     PCI BDF string (e.g. "0000:03:00.0").  Required.
 *
 * @return 0 on success, -1 on failure.
 */
int slash_hotplug_remove(struct slash_hotplug *hotplug, const char *bdf);

/**
 * @brief Assert and deassert a Secondary Bus Reset.
 *
 * @param hotplug Open hotplug handle.
 * @param bdf     PCI BDF string identifying the device (or its former
 *                location if already removed).  Required.
 *
 * Toggles the SBR bit on the device's immediate upstream bridge
 * (assert, 2 ms hold, deassert) and returns.  The caller is
 * responsible for waiting for the device to re-initialize before
 * rescanning the bus.
 *
 * @return 0 on success, -1 on failure.
 */
int slash_hotplug_toggle_sbr(struct slash_hotplug *hotplug, const char *bdf);

/**
 * @brief Perform a full hot-plug cycle (remove + rescan).
 *
 * @param hotplug Open hotplug handle.
 * @param bdf     PCI BDF string.  Required.
 *
 * @return 0 on success, -1 on failure.
 */
int slash_hotplug_hotplug(struct slash_hotplug *hotplug, const char *bdf);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* LIBSLASH_HOTPLUG_H */
