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
 * @file qdma_mock.c
 * @brief Mock QDMA implementation backed by memfd files.
 *
 * Each queue pair's I/O fd is a memfd_create() anonymous file.  The kernel
 * supports pread()/pwrite() at arbitrary offsets on memfds (tmpfs), so the
 * test's DDR_BASE_ADDRESS offset is handled transparently via sparse pages.
 *
 * Queue state is tracked in a fixed-size table (QDMA_MOCK_MAX_QUEUES slots)
 * stored in the slash_qdma_mock struct pointed to by qdma->priv.
 */

#define _GNU_SOURCE

#include "qdma_mock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#define QDMA_MOCK_MAX_QUEUES 64

struct slash_qdma_mock_qpair {
    bool in_use;
    bool started;
    int  fd; /* backing memfd; -1 when slot is free */
};

struct slash_qdma_mock {
    struct slash_qdma_mock_qpair queues[QDMA_MOCK_MAX_QUEUES];
};

static struct slash_qdma_mock *mock_ctx(struct slash_qdma *qdma)
{
    return (struct slash_qdma_mock *) qdma->priv;
}

struct slash_qdma *slash_qdma_mock_open(void)
{
    struct slash_qdma *qdma;
    struct slash_qdma_mock *ctx;
    size_t i;

    qdma = calloc(1, sizeof(*qdma));
    if (qdma == NULL) {
        return NULL;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        free(qdma);
        return NULL;
    }

    for (i = 0; i < QDMA_MOCK_MAX_QUEUES; ++i) {
        ctx->queues[i].fd = -1;
    }

    qdma->fd   = -1;
    qdma->priv = ctx;

    return qdma;
}

int slash_qdma_mock_close(struct slash_qdma *qdma)
{
    struct slash_qdma_mock *ctx;
    size_t i;

    if (qdma == NULL) {
        errno = EINVAL;
        return -1;
    }

    ctx = mock_ctx(qdma);

    for (i = 0; i < QDMA_MOCK_MAX_QUEUES; ++i) {
        if (ctx->queues[i].in_use && ctx->queues[i].fd >= 0) {
            (void) close(ctx->queues[i].fd);
        }
    }

    free(ctx);
    free(qdma);

    return 0;
}

int slash_qdma_mock_info_read(struct slash_qdma *qdma, struct slash_qdma_info *info)
{
    if (qdma == NULL || info == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(info, 0, sizeof(*info));
    info->size      = sizeof(*info);
    info->qsets_max = QDMA_MOCK_MAX_QUEUES;
    info->msix_qvecs = 1;

    return 0;
}

int slash_qdma_mock_qpair_add(struct slash_qdma *qdma, struct slash_qdma_qpair_add *req)
{
    struct slash_qdma_mock *ctx;
    size_t i;
    int fd;

    if (qdma == NULL || req == NULL) {
        errno = EINVAL;
        return -1;
    }

    ctx = mock_ctx(qdma);

    for (i = 0; i < QDMA_MOCK_MAX_QUEUES; ++i) {
        if (!ctx->queues[i].in_use) {
            break;
        }
    }

    if (i == QDMA_MOCK_MAX_QUEUES) {
        errno = ENOSPC;
        return -1;
    }

    fd = memfd_create("slash_qdma_mock", MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    ctx->queues[i].in_use  = true;
    ctx->queues[i].started = false;
    ctx->queues[i].fd      = fd;

    req->qid = (uint32_t) i;

    return 0;
}

static int mock_qpair_op(struct slash_qdma *qdma, uint32_t qid, bool start)
{
    struct slash_qdma_mock *ctx;

    if (qdma == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (qid >= QDMA_MOCK_MAX_QUEUES) {
        errno = EINVAL;
        return -1;
    }

    ctx = mock_ctx(qdma);

    if (!ctx->queues[qid].in_use) {
        errno = EINVAL;
        return -1;
    }

    ctx->queues[qid].started = start;

    return 0;
}

int slash_qdma_mock_qpair_start(struct slash_qdma *qdma, uint32_t qid)
{
    return mock_qpair_op(qdma, qid, true);
}

int slash_qdma_mock_qpair_stop(struct slash_qdma *qdma, uint32_t qid)
{
    return mock_qpair_op(qdma, qid, false);
}

int slash_qdma_mock_qpair_del(struct slash_qdma *qdma, uint32_t qid)
{
    struct slash_qdma_mock *ctx;

    if (qdma == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (qid >= QDMA_MOCK_MAX_QUEUES) {
        errno = EINVAL;
        return -1;
    }

    ctx = mock_ctx(qdma);

    if (!ctx->queues[qid].in_use) {
        errno = EINVAL;
        return -1;
    }

    if (ctx->queues[qid].fd >= 0) {
        (void) close(ctx->queues[qid].fd);
    }

    memset(&ctx->queues[qid], 0, sizeof(ctx->queues[qid]));
    ctx->queues[qid].fd = -1;

    return 0;
}

int slash_qdma_mock_qpair_get_fd(struct slash_qdma *qdma, uint32_t qid, int flags)
{
    struct slash_qdma_mock *ctx;
    int new_fd;
    (void) flags; /* O_CLOEXEC already set on the memfd */

    if (qdma == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (qid >= QDMA_MOCK_MAX_QUEUES) {
        errno = EINVAL;
        return -1;
    }

    ctx = mock_ctx(qdma);

    if (!ctx->queues[qid].in_use || !ctx->queues[qid].started) {
        errno = EINVAL;
        return -1;
    }

    /* dup so the caller owns a separate fd they can close independently */
    new_fd = dup(ctx->queues[qid].fd);
    if (new_fd < 0) {
        return -1;
    }

    return new_fd;
}
