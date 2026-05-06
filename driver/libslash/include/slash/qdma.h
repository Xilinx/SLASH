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
 * @file qdma.h
 *
 * Userspace API for slash QDMA (Queue-based DMA) devices.
 *
 * A QDMA device is a separate misc character device created for PF1,
 * while the control device (ctldev) is created for PF2.  Each PCI
 * function gets at most one of each.  Device nodes appear at
 * /dev/slash_qdma_ctl0, /dev/slash_qdma_ctl1, etc.
 *
 * Queue pair lifecycle:
 *   1. slash_qdma_open()         — open the QDMA device
 *   2. slash_qdma_qpair_add()   — create a queue pair (returns assigned qid)
 *   3. slash_qdma_qpair_start() — activate for transfers
 *   4. slash_qdma_qpair_get_fd() — obtain fd for data transfer
 *   5. slash_qdma_qpair_stop()  — deactivate
 *   6. slash_qdma_qpair_del()   — destroy
 *   7. slash_qdma_close()       — close the device
 *
 * The fd from qpair_get_fd() supports read() for C2H (card-to-host)
 * and write() for H2C (host-to-card) DMA transfers.  Positional I/O
 * via lseek()/pread()/pwrite() is also supported.  splice(), mmap(),
 * and poll() are not available.
 *
 * Error conventions: int-returning functions return -1 with errno set.
 * Pointer-returning functions return NULL with errno set.
 */

#ifndef LIBSLASH_QDMA_H
#define LIBSLASH_QDMA_H

#include "uapi/slash_interface.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Handle to an open QDMA device.
 *
 * \@priv is NULL for real hardware handles.  When slash_qdma_open() is
 * called with "\@mock", it points to an internal slash_qdma_mock context;
 * callers should treat it as opaque.
 */
struct slash_qdma {
    int fd;     /**< File descriptor for the QDMA character device (-1 in mock mode). */
    void *priv; /**< Opaque mock context, or NULL for real hardware. */
};

/**
 * @brief Open a QDMA device.
 *
 * @param path Path to the character device node. NULL returns NULL/EINVAL.
 *
 * @return Heap-allocated handle on success, NULL on failure.
 */
struct slash_qdma *slash_qdma_open(const char *path);

/**
 * @brief Close a QDMA device and free the handle.
 *
 * @param qdma Handle from slash_qdma_open(), or NULL (returns -1/EINVAL).
 *
 * @return 0 on success, -1 on failure.
 */
int slash_qdma_close(struct slash_qdma *qdma);

/**
 * @brief Read QDMA device capabilities.
 *
 * @param qdma Open QDMA handle.
 * @param info Caller-allocated struct, filled in on success.
 *
 * @return 0 on success, -1 on failure.
 */
int slash_qdma_info_read(struct slash_qdma *qdma, struct slash_qdma_info *info);

/**
 * @brief Create a new queue pair.
 *
 * @param qdma Open QDMA handle.
 * @param req  In/out — caller sets configuration fields, kernel fills in
 *             the assigned queue id (and possibly other output fields).
 *
 * @return 0 on success, -1 on failure.
 */
int slash_qdma_qpair_add(struct slash_qdma *qdma,
                         struct slash_qdma_qpair_add *req);

/**
 * @brief Activate a queue pair for transfers.
 *
 * @param qdma Open QDMA handle.
 * @param qid  Queue pair id from slash_qdma_qpair_add().
 *
 * @return 0 on success, -1 on failure.
 */
int slash_qdma_qpair_start(struct slash_qdma *qdma, uint32_t qid);

/**
 * @brief Deactivate a queue pair.
 *
 * @param qdma Open QDMA handle.
 * @param qid  Queue pair id.
 *
 * @return 0 on success, -1 on failure.
 */
int slash_qdma_qpair_stop(struct slash_qdma *qdma, uint32_t qid);

/**
 * @brief Destroy a queue pair.
 *
 * @param qdma Open QDMA handle.
 * @param qid  Queue pair id.
 *
 * The kernel implicitly stops the queue if it is still running, so a
 * separate stop call is not required before del.
 *
 * @return 0 on success, -1 on failure.
 */
int slash_qdma_qpair_del(struct slash_qdma *qdma, uint32_t qid);

/**
 * @brief Obtain a file descriptor for data transfer.
 *
 * @param qdma  Open QDMA handle.
 * @param qid   Queue pair id (must be started).
 * @param flags Only O_CLOEXEC is accepted; the kernel returns -EINVAL for
 *              any other bits.
 *
 * The returned fd supports read() (C2H) and write() (H2C).  Positional
 * I/O via lseek()/pread()/pwrite() is also available.
 *
 * @return Non-negative fd on success, -1 on failure.
 */
int slash_qdma_qpair_get_fd(struct slash_qdma *qdma, uint32_t qid, int flags);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* LIBSLASH_QDMA_H */

