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
 * @file ctldev.c
 *
 * Implementation of the slash control device wrapper.
 *
 * Each public function either delegates to the mock implementation
 * (ctldev_mock.h) or issues a single ioctl/syscall against the real
 * character device. No caching or retry logic.
 *
 * Error handling follows POSIX conventions: -1 or NULL on failure
 * with errno set.
 */


#define _GNU_SOURCE

#include <slash/ctldev.h>

#include "ctldev_mock.h"
#include "sock_transport.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <errno.h>

#include <stdint.h>
#include <string.h>

#include <stdio.h>
#include <sys/mman.h>

struct slash_ctldev *slash_ctldev_open(const char *path)
{
    struct slash_ctldev *ctldev;
    int is_sock;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (strcmp(path, "@mock") == 0) {
        return slash_ctldev_mock_open();
    }

    ctldev = calloc(1, sizeof(*ctldev));
    if (ctldev == NULL) {
        return NULL;
    }

    is_sock = slash_path_is_socket(path);
    if (is_sock < 0) {
        goto err_free_ctldev;
    }

    if (is_sock) {
        ctldev->fd = slash_sock_connect(path);
        if (ctldev->fd < 0) {
            goto err_free_ctldev;
        }
        ctldev->mock      = false;
        ctldev->transport = SLASH_TRANSPORT_SOCKET;
        ctldev->seq       = 0;
    } else {
        ctldev->fd = open(path, O_RDWR);
        if (ctldev->fd < 0) {
            goto err_free_ctldev;
        }
        ctldev->mock      = false;
        ctldev->transport = SLASH_TRANSPORT_IOCTL;
        ctldev->seq       = 0;
    }

    return ctldev;

err_free_ctldev:
    free(ctldev);

    return NULL;
}

int slash_ctldev_close(struct slash_ctldev *ctldev)
{
    int ret;

    if (ctldev == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (ctldev->mock) {
        return slash_ctldev_mock_close(ctldev);
    }

    ret = 0;
    if (ctldev->fd >= 0 && close(ctldev->fd) != 0) {
        ret = -1;
    }

    /* Free unconditionally — the handle is invalid after this call. */
    free(ctldev);

    return ret;
}

struct slash_ioctl_device_info *slash_device_info_read(struct slash_ctldev *ctldev)
{
    int ret;
    int32_t rv;
    struct slash_ioctl_device_info *info;

    if (ctldev == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (ctldev->mock) {
        return slash_device_info_mock_read(ctldev);
    }

    info = calloc(1, sizeof(*info));
    if (info == NULL) {
        return NULL;
    }

    info->size = sizeof(*info);

    if (ctldev->transport == SLASH_TRANSPORT_SOCKET) {
        rv = slash_sock_request(ctldev->fd,
                                (uint32_t)SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO,
                                info, sizeof(*info),
                                NULL, 0,
                                NULL, 0, NULL,
                                &ctldev->seq);
        if (rv == SLASH_SOCK_TRANSPORT_ERR) {
            /* errno already ENODEV */
            goto err_free_info;
        }
        if (rv < 0) {
            errno = (int)-rv;
            goto err_free_info;
        }
        return info;
    }

    /* IOCTL path */
    ret = ioctl(ctldev->fd, SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO, info);
    if (ret < 0) {
        goto err_free_info;
    }

    return info;

err_free_info:
    free(info);

    return NULL;
}

void slash_device_info_free(struct slash_ioctl_device_info *info)
{
    free(info);
}

struct slash_ioctl_bar_info *slash_bar_info_read(struct slash_ctldev *ctldev, int bar_number)
{
    int ret;
    int32_t rv;
    struct slash_ioctl_bar_info *bar_info;

    if (ctldev == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (ctldev->mock) {
        return slash_bar_info_mock_read(ctldev, bar_number);
    }

    bar_info = calloc(1, sizeof(*bar_info));
    if (bar_info == NULL) {
        return NULL;
    }

    bar_info->size       = sizeof(*bar_info);
    bar_info->bar_number = (uint8_t)bar_number;

    if (ctldev->transport == SLASH_TRANSPORT_SOCKET) {
        rv = slash_sock_request(ctldev->fd,
                                (uint32_t)SLASH_CTLDEV_IOCTL_GET_BAR_INFO,
                                bar_info, sizeof(*bar_info),
                                NULL, 0,
                                NULL, 0, NULL,
                                &ctldev->seq);
        if (rv == SLASH_SOCK_TRANSPORT_ERR) {
            goto err_free_bar_info;
        }
        if (rv < 0) {
            errno = (int)-rv;
            goto err_free_bar_info;
        }
        return bar_info;
    }

    /* IOCTL path */
    ret = ioctl(ctldev->fd, SLASH_CTLDEV_IOCTL_GET_BAR_INFO, bar_info);
    if (ret < 0) {
        goto err_free_bar_info;
    }

    return bar_info;

err_free_bar_info:
    free(bar_info);

    return NULL;
}

struct slash_bar_file *slash_bar_file_open(struct slash_ctldev *ctldev, int bar_number, int flags)
{
    struct slash_ioctl_bar_fd_request req;
    struct slash_bar_file *bar_file;
    int recv_fd;
    size_t n_recv;
    int32_t rv;

    memset(&req, 0, sizeof(req));
    req.size       = sizeof(req);
    req.bar_number = (uint8_t)bar_number;
    req.flags      = (uint32_t)flags;

    if (ctldev == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (ctldev->mock) {
        return slash_bar_file_mock_open(ctldev, bar_number, flags);
    }

    bar_file = calloc(1, sizeof(*bar_file));
    if (bar_file == NULL) {
        return NULL;
    }

    if (ctldev->transport == SLASH_TRANSPORT_SOCKET) {
        recv_fd  = -1;
        n_recv   = 0;
        rv = slash_sock_request(ctldev->fd,
                                (uint32_t)SLASH_CTLDEV_IOCTL_GET_BAR_FD,
                                &req, sizeof(req),
                                NULL, 0,
                                &recv_fd, 1, &n_recv,
                                &ctldev->seq);
        if (rv == SLASH_SOCK_TRANSPORT_ERR) {
            goto err_free_bar_file;
        }
        if (rv < 0) {
            errno = (int)-rv;
            if (recv_fd >= 0) { close(recv_fd); recv_fd = -1; }
            goto err_free_bar_file;
        }
        if (n_recv < 1 || recv_fd < 0) {
            /* Daemon didn't send us an fd — treat as transport error */
            errno = ENODEV;
            goto err_free_bar_file;
        }
        /* req.length was filled in by the daemon. */
        bar_file->fd        = recv_fd;
        bar_file->len       = (size_t)req.length;
        bar_file->mock      = false;
        bar_file->transport = SLASH_TRANSPORT_SOCKET;

        bar_file->map = mmap(NULL, bar_file->len, PROT_READ | PROT_WRITE,
                             MAP_SHARED, bar_file->fd, 0);
        if (bar_file->map == MAP_FAILED) {
            close(bar_file->fd);
            goto err_free_bar_file;
        }
        return bar_file;
    }

    /* IOCTL path: the ioctl returns the dma-buf fd directly as its return value. */
    bar_file->fd = ioctl(ctldev->fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
    if (bar_file->fd < 0) {
        goto err_free_bar_file;
    }

    /* The kernel filled in req.length with the BAR size. */
    bar_file->len = (size_t)req.length;

    /*
     * Map the entire BAR into our address space.  The dma-buf fd
     * backs this mapping — the kernel's slash_bar_dmabuf_mmap()
     * installs a fault handler that maps BAR pages via
     * vmf_insert_pfn() on first access.  Callers must bracket
     * accesses with the DMA_BUF_IOCTL_SYNC start/end helpers
     * (see the inline functions in ctldev.h).
     */
    bar_file->map = mmap(NULL, bar_file->len, PROT_READ | PROT_WRITE,
                         MAP_SHARED, bar_file->fd, 0);
    if (bar_file->map == MAP_FAILED) {
        goto err_close_fd;
    }

    bar_file->mock      = false;
    bar_file->transport = SLASH_TRANSPORT_IOCTL;

    return bar_file;

err_close_fd:
    (void)close(bar_file->fd);

err_free_bar_file:
    free(bar_file);

    return NULL;
}

int slash_bar_file_close(struct slash_bar_file *bar_file)
{
    int ret = 0;

    if (bar_file == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (bar_file->mock) {
        return slash_bar_file_mock_close(bar_file);
    }

    if (munmap(bar_file->map, bar_file->len) != 0) {
        ret = -1;
    }

    if (close(bar_file->fd) != 0) {
        ret = -1;
    }

    /* Free unconditionally — the handle is invalid after this call. */
    free(bar_file);

    return ret;
}

void slash_bar_info_free(struct slash_ioctl_bar_info *bar_info)
{
    free(bar_info);
}
