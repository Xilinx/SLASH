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
 * The fd from qpair_get_fd() is ioctl-only for data movement: create kernel
 * buffers with slash_qdma_buffer_create() (or slash_qdma_qpair_buffer_create()
 * through a queue-pair fd), then move them with slash_qdma_qpair_transfer() /
 * slash_qdma_qpair_transfer_batch().  read(), write(), and poll() are not
 * available for SLASH transfers.
 *
 * Kernel buffers:
 *   For high-throughput transfers, the kernel allocates a DMA buffer once
 *   (pages + SGL + DMA mapping built at creation), returns a mappable fd, and
 *   userspace mmaps it for CPU access.  Transfers reference the buffer by its
 *   fd instead of re-pinning per call.  Closing the buffer fd (and unmapping)
 *   releases it.
 *
 * Error conventions: int-returning functions return -1 with errno set.
 * Pointer-returning functions return NULL with errno set.
 */

#ifndef LIBSLASH_QDMA_H
#define LIBSLASH_QDMA_H

#include "uapi/slash_interface.h"

#include <stdint.h>
#include <sys/types.h>

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
 * The returned fd supports transfer and buffer-registration ioctls.  It does
 * not support read/write data movement; use slash_qdma_qpair_transfer().
 *
 * @return Non-negative fd on success, -1 on failure.
 */
int slash_qdma_qpair_get_fd(struct slash_qdma *qdma, uint32_t qid, int flags);

/**
 * @brief Obtain a transfer fd bound to one or more queue pairs.
 *
 * Like slash_qdma_qpair_get_fd(), but the returned fd is a collection of up to
 * SLASH_QDMA_FD_MAX_QPAIRS queue pairs.  A transfer issued on the fd selects a
 * bound queue pair by its index in @qids, so a single transfer can fan across
 * both AXI-MM/NoC channels.  Each bound queue pair keeps whatever per-qpair
 * settings (mm_channel, ring sizes, directions) it was given at add time.
 *
 * @param qdma        Open QDMA handle.
 * @param qids        Array of @qpair_count queue pair IDs (must be started).
 * @param qpair_count Number of entries in @qids (1..SLASH_QDMA_FD_MAX_QPAIRS).
 * @param flags       Only O_CLOEXEC is accepted.
 *
 * @return Non-negative fd on success, -1 on failure (errno set).
 */
int slash_qdma_qpair_get_fd_multi(struct slash_qdma *qdma, const uint32_t *qids,
                                  uint32_t qpair_count, int flags);

/**
 * @brief A kernel-owned DMA buffer and its CPU mapping.
 *
 * Created by slash_qdma_buffer_create() / slash_qdma_qpair_buffer_create() and
 * released by slash_qdma_buffer_destroy().  @addr is an mmap of the kernel
 * buffer fd; write/read it from the CPU and move it with the transfer helpers,
 * passing @fd as the sub-transfer's buf_fd.
 */
struct slash_qdma_buffer {
    int fd;                                 /**< Buffer fd (close via destroy). */
    void *addr;                             /**< CPU mapping of the buffer. */
    uint64_t length;                        /**< Buffer length in bytes. */
    uint32_t granule;                       /**< Bytes per DMA descriptor (page). */
    enum slash_qdma_transfer_hint transfer_hint; /**< Advisory channel policy. */
};

/**
 * @brief Create a kernel-owned DMA buffer and mmap it.
 *
 * Allocates @length bytes of kernel memory (DMA-mapped once), returns a buffer
 * fd, and mmaps it into @buf_out->addr for CPU access.  The buffer is bound to
 * @qdma's device; transfers must use a queue-pair fd of the same device.
 *
 * @param qdma    Open QDMA handle.
 * @param length  Buffer length in bytes (non-zero multiple of the page size).
 * @param buf_out [out] Receives the created buffer (fd, mapping, metadata).
 *
 * @return 0 on success, -1 on failure (errno set).
 */
int slash_qdma_buffer_create(struct slash_qdma *qdma, uint64_t length,
                             struct slash_qdma_buffer *buf_out);

/**
 * @brief Create a kernel-owned DMA buffer through a queue-pair fd.
 *
 * Same semantics as slash_qdma_buffer_create(), but issues the create ioctl on
 * @p qpair_fd.  This is the preferred form for clients that received only qpair
 * fds via SCM_RIGHTS (for example libvrtd clients).
 *
 * @return 0 on success, -1 on failure (errno set).
 */
int slash_qdma_qpair_buffer_create(int qpair_fd, uint64_t length,
                                   struct slash_qdma_buffer *buf_out);

/**
 * @brief Release a buffer created with slash_qdma_buffer_create() or
 *        slash_qdma_qpair_buffer_create().
 *
 * Unmaps @buf->addr and closes @buf->fd.  Safe to call on a zeroed/partial
 * buffer (fields are reset).
 *
 * @return 0 on success, -1 on failure (errno set).
 */
int slash_qdma_buffer_destroy(struct slash_qdma_buffer *buf);

/**
 * @brief Perform a DMA transfer using a single buffer fd.
 *
 * Convenience wrapper around slash_qdma_qpair_transfer_batch() for a single
 * sub-transfer on qpair_index 0.
 *
 * @param qpair_fd   Queue-pair I/O fd from slash_qdma_qpair_get_fd().
 * @param buf_fd     Buffer fd (from slash_qdma_buffer_create()).
 * @param buf_offset Byte offset within the buffer.
 * @param dev_addr   Device-side (endpoint) address.
 * @param length     Number of bytes to transfer.
 * @param direction  One of enum slash_qdma_transfer_dir (H2C or C2H).
 *
 * @return Number of bytes transferred (>= 0) on success, -1 on failure
 *         (errno set).
 */
ssize_t slash_qdma_qpair_transfer(int qpair_fd, int buf_fd,
                                  uint64_t buf_offset, uint64_t dev_addr,
                                  uint64_t length, uint32_t direction);

/**
 * @brief Perform a batch of buffer DMA sub-transfers in one call.
 *
 * Issues a single transfer ioctl carrying @count sub-transfers.  The kernel
 * runs sub-transfers that target distinct queue pairs concurrently, so one
 * call can drive both NoC channels in parallel.  Each sub-transfer names a
 * bound queue pair by index (see slash_qdma_qpair_get_fd_multi()) and a buffer
 * by its buf_fd.
 *
 * @param qpair_fd Transfer fd from slash_qdma_qpair_get_fd[_multi]().
 * @param xfers    Array of @count sub-transfer descriptors.
 * @param count    Number of sub-transfers (1..SLASH_QDMA_FD_MAX_QPAIRS).
 *
 * @return Total bytes transferred (>= 0) on success, -1 on failure (errno set).
 */
ssize_t slash_qdma_qpair_transfer_batch(int qpair_fd,
                                        const struct slash_qdma_subxfer *xfers,
                                        uint32_t count);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* LIBSLASH_QDMA_H */

