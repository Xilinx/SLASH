/**
 * Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/**
 * @file ctldev.c
 *
 * Implementation of the slash control device wrapper.
 *
 * Each public function either issues a single ioctl/syscall against the real
 * character device or executes a @ref slash_sock_request against the system
 * emulation socket. No caching or retry logic.
 *
 * Error handling follows POSIX conventions: -1 or NULL on failure
 * with errno set.
 */

#define _GNU_SOURCE

#include <slash/ctldev.h>

#include "mock_sock.h"
#include "sock_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdlib.h>
#include <unistd.h>

#include <stdint.h>
#include <string.h>

#include <stdio.h>
#include <sys/mman.h>

struct slash_ctldev *slash_ctldev_open(const char *path) {
    struct slash_ctldev *ctldev;
    int is_mock, is_sock;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    ctldev = calloc(1, sizeof(*ctldev));
    if (ctldev == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    is_mock = strcmp(path, "@mock") == 0;
    if (!is_mock) {
        is_sock = slash_path_is_socket(path);
        if (is_sock < 0) {
            free(ctldev);
            /* With the error from slash_path_is_socket. */
            return NULL;
        }
    }

    if (is_mock) {
        ctldev->fd = slash_mock_sock_create(SLASH_MOCK_SOCK_ENDPOINT_CTLDEV);
        ctldev->transport = SLASH_TRANSPORT_SOCKET;
    } else if (is_sock) {
        ctldev->fd = slash_sock_connect(path);
        ctldev->transport = SLASH_TRANSPORT_SOCKET;
    } else {
        ctldev->fd = open(path, O_RDWR);
        ctldev->transport = SLASH_TRANSPORT_IOCTL;
    }
    ctldev->seq = 0;

    if (ctldev->fd < 0) {
        /* With the errno from open/slash_mock_sock_create/slash_sock_connect */
        goto err_free_ctldev;
    }

    return ctldev;

err_free_ctldev:
    free(ctldev);

    return NULL;
}

int slash_ctldev_close(struct slash_ctldev *ctldev) {
    int ret;

    if (ctldev == NULL) {
        errno = EINVAL;
        return -1;
    }

    ret = 0;
    if (ctldev->fd >= 0 && close(ctldev->fd) != 0) {
        /* With the errno from close */
        ret = -1;
    }

    /* Free unconditionally — the handle is invalid after this call. */
    free(ctldev);

    return ret;
}

struct slash_ioctl_device_info *
slash_device_info_read(struct slash_ctldev *ctldev) {
    int ret;
    int32_t rv;
    struct slash_ioctl_device_info *info;

    if (ctldev == NULL) {
        errno = EINVAL;
        return NULL;
    }

    info = calloc(1, sizeof(*info));
    if (info == NULL) {
        return NULL;
    }

    info->size = sizeof(*info);

    if (ctldev->transport == SLASH_TRANSPORT_SOCKET) {
        rv = slash_sock_request(
            ctldev->fd, (uint32_t)SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO, info,
            sizeof(*info), NULL, 0, NULL, 0, NULL, &ctldev->seq);
    } else if (ctldev->transport == SLASH_TRANSPORT_IOCTL) {
        rv = ioctl(ctldev->fd, SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO, info);
    } else {
        rv = -EINVAL;
    }

    if (rv < 0) {
        errno = -rv;
        goto err_free_info;
    }

    return info;

err_free_info:
    free(info);

    return NULL;
}

void slash_device_info_free(struct slash_ioctl_device_info *info) {
    free(info);
}

struct slash_ioctl_bar_info *slash_bar_info_read(struct slash_ctldev *ctldev,
                                                 int bar_number) {
    int ret;
    int32_t rv;
    struct slash_ioctl_bar_info *bar_info;

    if (ctldev == NULL) {
        errno = EINVAL;
        return NULL;
    }

    bar_info = calloc(1, sizeof(*bar_info));
    if (bar_info == NULL) {
        return NULL;
    }

    bar_info->size = sizeof(*bar_info);
    bar_info->bar_number = (uint8_t)bar_number;

    if (ctldev->transport == SLASH_TRANSPORT_SOCKET) {
        rv = slash_sock_request(
            ctldev->fd, (uint32_t)SLASH_CTLDEV_IOCTL_GET_BAR_INFO, bar_info,
            sizeof(*bar_info), NULL, 0, NULL, 0, NULL, &ctldev->seq);
    } else if (ctldev->transport == SLASH_TRANSPORT_IOCTL) {
        rv = ioctl(ctldev->fd, SLASH_CTLDEV_IOCTL_GET_BAR_INFO, bar_info);
    } else {
        rv = -EINVAL;
    }

    if (rv < 0) {
        errno = (int)-rv;
        goto err_free_bar_info;
    }
    return bar_info;

err_free_bar_info:
    free(bar_info);

    return NULL;
}

struct slash_bar_file *slash_bar_file_open(struct slash_ctldev *ctldev,
                                           int bar_number, int flags) {
    struct slash_ioctl_bar_fd_request req;
    struct slash_bar_file *bar_file;
    size_t n_recv;
    int32_t rv;

    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    req.bar_number = (uint8_t)bar_number;
    req.flags = (uint32_t)flags;

    if (ctldev == NULL) {
        errno = EINVAL;
        return NULL;
    }

    bar_file = calloc(1, sizeof(*bar_file));
    if (bar_file == NULL) {
        return NULL;
    }

    if (ctldev->transport == SLASH_TRANSPORT_SOCKET) {
        n_recv = 0;
        rv = slash_sock_request(
            ctldev->fd, (uint32_t)SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req,
            sizeof(req), NULL, 0, &bar_file->fd, 1, &n_recv, &ctldev->seq);
        if (rv < 0) {
            errno = -rv;
            goto err_free_bar_file;
        }
        if (n_recv < 1 || bar_file->fd < 0) {
            /* Daemon didn't send us an fd — treat as transport error */
            errno = ENODEV;
            goto err_free_bar_file;
        }
    } else if (ctldev->transport == SLASH_TRANSPORT_IOCTL) {
        bar_file->fd = ioctl(ctldev->fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
        if (bar_file->fd < 0) {
            goto err_free_bar_file;
        }
    } else {
        errno = EINVAL;
        goto err_free_bar_file;
    }

    /* After this point, bar_file->fd is a valid FD */

    /* req.length was filled in by the server. */
    bar_file->len = (size_t)req.length;
    bar_file->transport = ctldev->transport;

    bar_file->map = mmap(NULL, bar_file->len, PROT_READ | PROT_WRITE,
                         MAP_SHARED, bar_file->fd, 0);
    if (bar_file->map == MAP_FAILED) {
        close(bar_file->fd);
        goto err_close_fd;
    }
    return bar_file;

err_close_fd:
    if (bar_file->fd >= 0) {
        (void)close(bar_file->fd);
    }

err_free_bar_file:
    free(bar_file);

    return NULL;
}

int slash_bar_file_close(struct slash_bar_file *bar_file) {
    int ret = 0;

    if (bar_file == NULL) {
        errno = EINVAL;
        return -1;
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

void slash_bar_info_free(struct slash_ioctl_bar_info *bar_info) {
    free(bar_info);
}
