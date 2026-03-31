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
 * @file ctldev_mock.h
 * @brief Mock control-device implementation for testing without hardware.
 */

#ifndef LIBSLASH_CTLDEV_MOCK_H
#define LIBSLASH_CTLDEV_MOCK_H

#include <slash/ctldev.h>

/**
 * @brief Open a mock control device (no hardware required).
 * @return Allocated mock handle, or NULL on failure.
 */
struct slash_ctldev *slash_ctldev_mock_open(void);

/**
 * @brief Close a mock control device and free its handle.
 * @param ctldev  Handle returned by slash_ctldev_mock_open().
 * @return 0 on success, -1 on failure.
 */
int slash_ctldev_mock_close(struct slash_ctldev *ctldev);

/**
 * @brief Return mock device info with BDF 0000:00:00.0.
 * @param ctldev  Mock control device handle.
 * @return Allocated device info, or NULL on failure.  Free with slash_device_info_free().
 */
struct slash_ioctl_device_info *slash_device_info_mock_read(struct slash_ctldev *ctldev);

/**
 * @brief Return mock BAR info.  BAR 0 is usable (64 MB); others are unusable.
 * @param ctldev      Mock control device handle.
 * @param bar_number  BAR index (0-5).
 * @return Allocated BAR info, or NULL on failure.  Free with slash_bar_info_mock_free().
 */
struct slash_ioctl_bar_info *slash_bar_info_mock_read(struct slash_ctldev *ctldev, int bar_number);

/**
 * @brief Free a BAR info struct returned by slash_bar_info_mock_read().
 * @param ctldev  BAR info to free.
 */
void slash_bar_info_mock_free(struct slash_ioctl_bar_info *ctldev);

/**
 * @brief Open mock BAR 0 backed by a temporary file (64 MB mmap).
 * @param ctldev      Mock control device handle.
 * @param bar_number  BAR index (only 0 is supported).
 * @param flags       Open flags (e.g. O_CLOEXEC).
 * @return Allocated BAR file with mmap, or NULL on failure.
 */
struct slash_bar_file *slash_bar_file_mock_open(struct slash_ctldev *ctldev, int bar_number, int flags);

/**
 * @brief Close mock BAR file, unmap memory, and unlink the backing file.
 * @param bar_file  BAR file returned by slash_bar_file_mock_open().
 * @return 0 on success, -1 on failure.
 */
int slash_bar_file_mock_close(struct slash_bar_file *bar_file);

#endif /* LIBSLASH_CTLDEV_MOCK_H */
