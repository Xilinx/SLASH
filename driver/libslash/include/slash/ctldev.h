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
 * @file ctldev.h
 *
 * Userspace API for the slash control device — device info, BAR info,
 * and memory-mapped BAR access.
 *
 * A slash control device is a misc character device created for each
 * FPGA PCI function (specifically PF2).  Device nodes appear at
 * /dev/slash_ctl0, /dev/slash_ctl1, etc.
 *
 * Three groups of functionality:
 *   1. Device info — PCI identity (slash_device_info_read)
 *   2. BAR info    — BAR properties (slash_bar_info_read)
 *   3. BAR file    — mmap'd BAR access via dma-buf (slash_bar_file_open)
 *
 * BAR file access uses the kernel dma-buf framework. Callers must
 * bracket MMIO accesses with the start/end sync helpers for cache
 * coherency.
 *
 * Mock mode: passing "\@mock" as the path to slash_ctldev_open() creates
 * a device backed by files on disk for testing without hardware.
 *
 * All functions follow POSIX conventions: pointer-returning functions
 * return NULL on failure; int-returning functions return -1. errno is
 * set in both cases.
 */

#ifndef LIBSLASH_CTLDEV_H
#define LIBSLASH_CTLDEV_H

#include "uapi/slash_interface.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <linux/dma-buf.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Handle to an open slash control device.
 */
struct slash_ctldev {
    int fd;    /**< File descriptor for the control character device. */
    bool mock; /**< True if this is a mock device (no real hardware). */
};

/**
 * @brief A memory-mapped BAR region.
 *
 * Obtained via slash_bar_file_open(). Callers access MMIO registers
 * through \@map, bracketing accesses with the start/end sync helpers.
 */
struct slash_bar_file {
    void *map;    /**< Pointer to the mmap'd BAR region. */
    size_t len;   /**< Size of the mapping in bytes. */
    int fd;       /**< The dma-buf file descriptor backing the mapping. */
    bool mock;    /**< True if backed by a mock file instead of real hardware. */
    /**
     * Path to the backing file (mock mode only); NULL otherwise.
     * Allocated by slash_bar_file_open() (mock path) and freed
     * by slash_bar_file_close().  NULL in non-mock mode.
     */
    char *mock_path;
};

/**
 * @brief Open a slash control device.
 *
 * @param path Path to the character device node, or "\@mock" for mock mode.
 *
 * @return A heap-allocated handle on success, NULL on failure.
 */
struct slash_ctldev *slash_ctldev_open(const char *path);

/**
 * @brief Close the control device and free the handle.
 *
 * @param ctldev Handle from slash_ctldev_open(). NULL returns -1 / EINVAL.
 *               Must not be used after this call.
 *
 * @return 0 on success, -1 on failure.
 */
int slash_ctldev_close(struct slash_ctldev *ctldev);


/**
 * @brief Read PCI identity information.
 *
 * @param ctldev Open control device handle.
 *
 * @return A heap-allocated slash_ioctl_device_info on success (caller
 *         frees with slash_device_info_free()), or NULL on failure.
 */
struct slash_ioctl_device_info *slash_device_info_read(struct slash_ctldev *ctldev);

/** @brief Free a device info struct returned by slash_device_info_read(). */
void slash_device_info_free(struct slash_ioctl_device_info *info);

/**
 * @brief Read BAR information for a specific BAR.
 *
 * @param ctldev     Open control device handle.
 * @param bar_number Which BAR to query (0–5).
 *
 * @return A heap-allocated slash_ioctl_bar_info on success (caller
 *         frees with slash_bar_info_free()), or NULL on failure.
 */
struct slash_ioctl_bar_info *slash_bar_info_read(struct slash_ctldev *ctldev, int bar_number);

/** @brief Free a BAR info struct returned by slash_bar_info_read(). */
void slash_bar_info_free(struct slash_ioctl_bar_info *ctldev);

/**
 * @brief Open and mmap a BAR region.
 *
 * @param ctldev     Open control device handle.
 * @param bar_number Which BAR to map (0–5).
 * @param flags      Only O_CLOEXEC is accepted.
 *
 * On success returns a handle whose \@map field points to the BAR
 * (PROT_READ|PROT_WRITE, MAP_SHARED). The underlying fd is a dma-buf;
 * callers must use the sync helpers to bracket accesses.
 *
 * @return NULL on failure.
 */
struct slash_bar_file *slash_bar_file_open(struct slash_ctldev *ctldev, int bar_number, int flags);

/**
 * @brief Unmap and close a BAR file.
 *
 * @param bar_file Handle from slash_bar_file_open(). NULL returns -1 / EINVAL.
 *
 * @return 0 on success, -1 if munmap or close fails.
 *         The handle is freed regardless.
 */
int slash_bar_file_close(struct slash_bar_file *bar_file);

/**
 * @brief Issue a DMA_BUF_IOCTL_SYNC on the BAR fd.
 *
 * @param bar_file Open BAR file handle.
 * @param flags    DMA_BUF_SYNC_* flags (START/END combined with READ/WRITE).
 *
 * Must be called to bracket MMIO accesses for cache coherency.
 * No-op in mock mode.
 *
 * @return 0 on success, -1 on failure.
 */
static __inline__ int slash_bar_file_sync(struct slash_bar_file *bar_file, unsigned int flags)
{
    struct dma_buf_sync sync = { .flags = flags };

    if (bar_file->mock) {
        return 0;
    }

    int ret = ioctl(bar_file->fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret == -1) {
        fprintf(stderr, "slash_bar_file_sync: DMA_BUF_IOCTL_SYNC failed (flags=0x%x, fd=%d, errno=%d)\n",
                flags, bar_file->fd, errno);
    }
    return ret;
}

/** Acquire write access to the BAR mapping. Equivalent to slash_bar_file_sync(bar_file, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE). */
static __inline__ int slash_bar_file_start_write(struct slash_bar_file *bar_file)
{
    return slash_bar_file_sync(bar_file, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
}

/** Release write access to the BAR mapping. Equivalent to slash_bar_file_sync(bar_file, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE). */
static __inline__ int slash_bar_file_end_write(struct slash_bar_file *bar_file)
{
    return slash_bar_file_sync(bar_file, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

/** Acquire read access to the BAR mapping. Equivalent to slash_bar_file_sync(bar_file, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ). */
static __inline__ int slash_bar_file_start_read(struct slash_bar_file *bar_file)
{
    return slash_bar_file_sync(bar_file, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}

/** Release read access to the BAR mapping. Equivalent to slash_bar_file_sync(bar_file, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ). */
static __inline__ int slash_bar_file_end_read(struct slash_bar_file *bar_file)
{
    return slash_bar_file_sync(bar_file, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* LIBSLASH_CTLDEV_H */
