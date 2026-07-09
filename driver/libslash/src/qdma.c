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
 * @file qdma.c
 *
 * Implementation of the slash QDMA userspace wrapper.
 *
 * Each public function validates its arguments, then issues a single
 * ioctl against the QDMA character device. No mock path exists yet.
 *
 * The ioctl structs use a size field for kernel-side version
 * negotiation: userspace sets size = sizeof(struct), and the kernel
 * can handle older/newer struct layouts accordingly.
 */

#define _GNU_SOURCE

#include <slash/qdma.h>

#include "qdma_mock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* Bounce-copy chunk used by the @mock transfer fallback. */
#define QDMA_XFER_BOUNCE_CHUNK (1u << 20)

/*
 * mmap a buffer fd (kernel buffer or @mock memfd) for CPU access.  Always
 * MAP_SHARED so writes are visible to the kernel/device and to pread/pwrite on
 * the same fd.
 */
static int qdma_buffer_mmap(struct slash_qdma_buffer *buf)
{
    void *addr = mmap(NULL, (size_t)buf->length, PROT_READ | PROT_WRITE,
                      MAP_SHARED, buf->fd, 0);

    if (addr == MAP_FAILED) {
        return -1;
    }
    buf->addr = addr;
    return 0;
}

/*
 * @mock / fallback buffer: a memfd sized to @length and mmapped shared.  Used
 * when the BUF_CREATE ioctl is unavailable (the memfd-backed @mock path).
 */
static int qdma_buffer_create_memfd(uint64_t length,
                                    struct slash_qdma_buffer *buf_out)
{
    int fd;
    int saved_errno;

    fd = memfd_create("slash_qdma_buf", MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)length) != 0) {
        saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }

    buf_out->fd = fd;
    buf_out->length = length;
    buf_out->granule = 4096;
    buf_out->transfer_hint = SLASH_QDMA_TRANSFER_HINT_V80;
    buf_out->addr = NULL;

    if (qdma_buffer_mmap(buf_out) != 0) {
        saved_errno = errno;
        (void)close(fd);
        buf_out->fd = -1;
        errno = saved_errno;
        return -1;
    }

    return 0;
}

/*
 * Create a kernel buffer via the BUF_CREATE ioctl on @ioctl_fd (control fd or
 * queue-pair fd), then mmap it.  Falls back to a memfd buffer when the ioctl is
 * not implemented (ENOTTY: the @mock path).
 */
static int qdma_buffer_create_on_fd(int ioctl_fd, uint64_t length,
                                    struct slash_qdma_buffer *buf_out)
{
    struct slash_qdma_buf_create req;
    int fd;
    int saved_errno;

    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    req.flags = O_CLOEXEC;
    req.length = length;

    fd = ioctl(ioctl_fd, SLASH_QDMA_IOCTL_BUF_CREATE, &req);
    if (fd < 0) {
        if (errno == ENOTTY) {
            return qdma_buffer_create_memfd(length, buf_out);
        }
        return -1;
    }

    buf_out->fd = fd;
    buf_out->length = length;
    buf_out->granule = req.granule ? req.granule : 4096;
    buf_out->transfer_hint = (enum slash_qdma_transfer_hint)req.transfer_hint;
    buf_out->addr = NULL;

    if (qdma_buffer_mmap(buf_out) != 0) {
        saved_errno = errno;
        (void)close(fd);
        buf_out->fd = -1;
        errno = saved_errno;
        return -1;
    }

    return 0;
}

/*
 * @mock transfer fallback: bounce a single sub-transfer between the host buffer
 * fd and the queue-pair memfd that stands in for device memory.  Only used when
 * the transfer ioctl returns ENOTTY.
 */
static ssize_t qdma_fallback_subxfer(int qpair_fd,
                                     const struct slash_qdma_subxfer *x)
{
    uint8_t *tmp;
    uint64_t done = 0;

    if (x->buf_fd < 0 ||
        (x->direction != SLASH_QDMA_XFER_H2C &&
         x->direction != SLASH_QDMA_XFER_C2H)) {
        errno = EINVAL;
        return -1;
    }

    /*
     * For C2H, make sure the device memfd is large enough that reads of
     * never-written regions return zeros instead of a short read.  Only ever
     * grow the file: shrinking would discard data a prior H2C wrote.
     */
    if (x->direction == SLASH_QDMA_XFER_C2H) {
        struct stat st;
        off_t want = (off_t)(x->dev_addr + x->length);

        if (fstat(qpair_fd, &st) == 0 && st.st_size < want) {
            (void)ftruncate(qpair_fd, want);
        }
    }

    tmp = (uint8_t *)malloc(QDMA_XFER_BOUNCE_CHUNK);
    if (tmp == NULL) {
        return -1;
    }

    while (done < x->length) {
        uint64_t remaining = x->length - done;
        size_t chunk = remaining < QDMA_XFER_BOUNCE_CHUNK
                           ? (size_t)remaining : QDMA_XFER_BOUNCE_CHUNK;
        ssize_t r;
        ssize_t w;

        if (x->direction == SLASH_QDMA_XFER_H2C) {
            r = pread(x->buf_fd, tmp, chunk, (off_t)(x->buf_offset + done));
            if (r <= 0) {
                free(tmp);
                return -1;
            }
            w = pwrite(qpair_fd, tmp, (size_t)r, (off_t)(x->dev_addr + done));
        } else {
            r = pread(qpair_fd, tmp, chunk, (off_t)(x->dev_addr + done));
            if (r <= 0) {
                free(tmp);
                return -1;
            }
            w = pwrite(x->buf_fd, tmp, (size_t)r, (off_t)(x->buf_offset + done));
        }

        if (w != r) {
            free(tmp);
            return -1;
        }
        done += (uint64_t)r;
    }

    free(tmp);
    return (ssize_t)done;
}

struct slash_qdma *slash_qdma_open(const char *path)
{
    struct slash_qdma *qdma;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (strcmp(path, "@mock") == 0) {
        return slash_qdma_mock_open();
    }

    qdma = calloc(1, sizeof(*qdma));
    if (qdma == NULL) {
        return NULL;
    }

    qdma->fd = open(path, O_RDWR);
    if (qdma->fd < 0) {
        free(qdma);
        return NULL;
    }

    return qdma;
}

int slash_qdma_close(struct slash_qdma *qdma)
{
    int ret;

    if (qdma == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (qdma->priv) {
        return slash_qdma_mock_close(qdma);
    }

    ret = 0;
    if (qdma->fd >= 0 && close(qdma->fd) != 0) {
        ret = -1;
    }

    /* Free unconditionally — handle is invalid after this call. */
    free(qdma);

    return ret;
}

int slash_qdma_info_read(struct slash_qdma *qdma, struct slash_qdma_info *info)
{
    struct slash_qdma_info tmp;
    int ret;

    if (qdma == NULL || info == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (qdma->priv) {
        return slash_qdma_mock_info_read(qdma, info);
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.size = sizeof(tmp);

    ret = ioctl(qdma->fd, SLASH_QDMA_IOCTL_INFO, &tmp);
    if (ret < 0) {
        return -1;
    }

    /* Copy the kernel-filled result back to the caller. */
    *info = tmp;

    return 0;
}

/**
 * slash_qdma_qpair_add() — Create a new queue pair.
 *
 * Copies caller-provided configuration into a zeroed temporary to
 * ensure no stale fields leak to the kernel, then copies the full
 * kernel response (including assigned qid) back into @req.
 */
int slash_qdma_qpair_add(struct slash_qdma *qdma,
                         struct slash_qdma_qpair_add *req)
{
    struct slash_qdma_qpair_add tmp;
    int ret;

    if (qdma == NULL || req == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (qdma->priv) {
        return slash_qdma_mock_qpair_add(qdma, req);
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.size        = sizeof(tmp);
    tmp.mode        = req->mode;
    tmp.dir_mask    = req->dir_mask;
    tmp.mm_channel  = req->mm_channel;
    tmp.h2c_ring_sz = req->h2c_ring_sz;
    tmp.c2h_ring_sz = req->c2h_ring_sz;
    tmp.cmpt_ring_sz = req->cmpt_ring_sz;
    tmp.aperture_size = req->aperture_size;

    ret = ioctl(qdma->fd, SLASH_QDMA_IOCTL_QPAIR_ADD, &tmp);
    if (ret < 0) {
        return -1;
    }

    /* Write back — kernel will have filled in qid and other fields. */
    *req = tmp;

    return 0;
}

/**
 * slash_qdma_qpair_op() — Issue a queue pair lifecycle operation.
 *
 * Internal helper shared by start/stop/del. The @op parameter selects
 * which operation the kernel performs.
 */
static int slash_qdma_qpair_op(struct slash_qdma *qdma,
                               uint32_t qid,
                               uint32_t op)
{
    struct slash_qdma_qpair_op req;
    int ret;

    if (qdma == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (qdma->priv) {
        switch (op) {
        case SLASH_QDMA_QUEUE_OP_START:
            return slash_qdma_mock_qpair_start(qdma, qid);
        case SLASH_QDMA_QUEUE_OP_STOP:
            return slash_qdma_mock_qpair_stop(qdma, qid);
        case SLASH_QDMA_QUEUE_OP_DEL:
            return slash_qdma_mock_qpair_del(qdma, qid);
        default:
            errno = EINVAL;
            return -1;
        }
    }

    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    req.qid  = qid;
    req.op   = op;

    ret = ioctl(qdma->fd, SLASH_QDMA_IOCTL_Q_OP, &req);
    if (ret < 0) {
        return -1;
    }

    return 0;
}

int slash_qdma_qpair_start(struct slash_qdma *qdma, uint32_t qid)
{
    return slash_qdma_qpair_op(qdma, qid, SLASH_QDMA_QUEUE_OP_START);
}

int slash_qdma_qpair_stop(struct slash_qdma *qdma, uint32_t qid)
{
    return slash_qdma_qpair_op(qdma, qid, SLASH_QDMA_QUEUE_OP_STOP);
}

int slash_qdma_qpair_del(struct slash_qdma *qdma, uint32_t qid)
{
    return slash_qdma_qpair_op(qdma, qid, SLASH_QDMA_QUEUE_OP_DEL);
}

int slash_qdma_qpair_get_fd(struct slash_qdma *qdma, uint32_t qid, int flags)
{
    struct slash_qdma_qpair_fd_request req;
    int fd;

    if (qdma == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (qdma->priv) {
        return slash_qdma_mock_qpair_get_fd(qdma, qid, flags);
    }

    memset(&req, 0, sizeof(req));
    req.size  = sizeof(req);
    req.qid   = qid;
    req.flags = flags;

    fd = ioctl(qdma->fd, SLASH_QDMA_IOCTL_QPAIR_GET_FD, &req);
    if (fd < 0) {
        return -1;
    }

    return fd;
}

int slash_qdma_qpair_get_fd_multi(struct slash_qdma *qdma, const uint32_t *qids,
                                  uint32_t qpair_count, int flags)
{
    struct slash_qdma_qpair_fd_request req;
    uint32_t i;
    int fd;

    if (qdma == NULL || qids == NULL ||
        qpair_count == 0 || qpair_count > SLASH_QDMA_FD_MAX_QPAIRS) {
        errno = EINVAL;
        return -1;
    }

    if (qdma->priv) {
        return slash_qdma_mock_qpair_get_fd_multi(qdma, qids, qpair_count,
                                                  flags);
    }

    memset(&req, 0, sizeof(req));
    req.size        = sizeof(req);
    req.flags       = flags;
    req.qid         = qids[0];
    req.qpair_count = qpair_count;
    for (i = 0; i < qpair_count; ++i) {
        req.qpair_ids[i] = qids[i];
    }

    fd = ioctl(qdma->fd, SLASH_QDMA_IOCTL_QPAIR_GET_FD, &req);
    if (fd < 0) {
        return -1;
    }

    return fd;
}

int slash_qdma_buffer_create(struct slash_qdma *qdma, uint64_t length,
                             struct slash_qdma_buffer *buf_out)
{
    if (qdma == NULL || buf_out == NULL || length == 0) {
        errno = EINVAL;
        return -1;
    }

    /* @mock has no character device: back the buffer with a memfd directly. */
    if (qdma->priv) {
        return qdma_buffer_create_memfd(length, buf_out);
    }

    return qdma_buffer_create_on_fd(qdma->fd, length, buf_out);
}

int slash_qdma_qpair_buffer_create(int qpair_fd, uint64_t length,
                                   struct slash_qdma_buffer *buf_out)
{
    if (qpair_fd < 0 || buf_out == NULL || length == 0) {
        errno = EINVAL;
        return -1;
    }

    return qdma_buffer_create_on_fd(qpair_fd, length, buf_out);
}

int slash_qdma_buffer_destroy(struct slash_qdma_buffer *buf)
{
    int ret = 0;

    if (buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (buf->addr != NULL && buf->addr != MAP_FAILED && buf->length != 0) {
        if (munmap(buf->addr, (size_t)buf->length) != 0) {
            ret = -1;
        }
    }
    buf->addr = NULL;

    if (buf->fd >= 0) {
        if (close(buf->fd) != 0) {
            ret = -1;
        }
        buf->fd = -1;
    }

    return ret;
}

ssize_t slash_qdma_qpair_transfer_batch(int qpair_fd,
                                        const struct slash_qdma_subxfer *xfers,
                                        uint32_t count)
{
    struct slash_qdma_transfer req;
    uint32_t i;
    int ret;

    if (qpair_fd < 0 || xfers == NULL ||
        count == 0 || count > SLASH_QDMA_FD_MAX_QPAIRS) {
        errno = EINVAL;
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.size  = sizeof(req);
    req.count = count;
    for (i = 0; i < count; ++i) {
        if (xfers[i].direction != SLASH_QDMA_XFER_H2C &&
            xfers[i].direction != SLASH_QDMA_XFER_C2H) {
            errno = EINVAL;
            return -1;
        }
        req.xfers[i] = xfers[i];
    }

    ret = ioctl(qpair_fd, SLASH_QDMA_QPAIR_IOCTL_TRANSFER, &req);
    if (ret < 0) {
        if (errno == ENOTTY) {
            /* @mock path: bounce each sub-transfer through the memfds. */
            uint64_t total = 0;

            for (i = 0; i < count; ++i) {
                ssize_t n = qdma_fallback_subxfer(qpair_fd, &xfers[i]);

                if (n < 0) {
                    return -1;
                }
                total += (uint64_t)n;
            }
            return (ssize_t)total;
        }
        return -1;
    }

    return (ssize_t)ret;
}

ssize_t slash_qdma_qpair_transfer(int qpair_fd, int buf_fd,
                                  uint64_t buf_offset, uint64_t dev_addr,
                                  uint64_t length, uint32_t direction)
{
    struct slash_qdma_subxfer xfer;

    memset(&xfer, 0, sizeof(xfer));
    xfer.qpair_index = 0;
    xfer.direction   = direction;
    xfer.buf_fd      = buf_fd;
    xfer.buf_offset  = buf_offset;
    xfer.dev_addr    = dev_addr;
    xfer.length      = length;

    return slash_qdma_qpair_transfer_batch(qpair_fd, &xfer, 1);
}

