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
 * @file hotplug.c
 *
 * Implementation of the libslash hotplug wrapper.
 *
 * This file provides the userspace side of the slash hotplug interface.
 * Each public function maps directly to a single ioctl on the hotplug
 * character device — there is no caching, batching, or retry logic.
 *
 * Error handling follows POSIX conventions throughout: functions return
 * -1 and set errno.  errno values originate either from this library
 * (EINVAL for NULL handles or oversized BDF strings) or from the
 * underlying syscalls (open, close, ioctl).
 */

#define _GNU_SOURCE

#include <slash/hotplug.h>

#include "sock_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

/**
 * slash_hotplug_ioctl() — Issue a hotplug ioctl that optionally carries a
 * slash_hotplug_device_request identifying a device by BDF.
 *
 * On the socket transport, the same request struct is sent as the payload.
 *
 * @hotplug:    Open hotplug handle.  Must not be NULL.
 * @op:         ioctl request number.
 * @attach_bdf: True iff a slash_hotplug_device_request, including @ref bdf,
 *              should be attached to the ioctl.
 * @bdf:        PCI BDF string, or NULL / empty string to let the kernel
 *              pick the only tracked device.  Must be shorter than
 *              SLASH_HOTPLUG_BDF_LEN bytes (including NUL); otherwise
 *              EINVAL is returned.
 *
 * Returns 0 on success, and -1 on failure with errno set.
 */
static int slash_hotplug_ioctl(struct slash_hotplug *hotplug, unsigned long op,
                               bool attach_bdf, const char *bdf) {
    struct slash_hotplug_device_request req;
    size_t len;
    void *arg;
    size_t arg_len;
    int32_t rv;

    if (hotplug == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (attach_bdf) {
        memset(&req, 0, sizeof(req));
        req.size = sizeof(req);

        if (bdf != NULL && bdf[0] != '\0') {
            len = strlen(bdf);
            if (len >= sizeof(req.bdf)) {
                errno = EINVAL;
                return -1;
            }

            memcpy(req.bdf, bdf, len + 1);
        }

        arg = &req;
        arg_len = sizeof(req);
    } else {
        arg = NULL;
        arg_len = 0;
    }

    if (hotplug->transport == SLASH_TRANSPORT_SOCKET) {
        rv = slash_sock_request(hotplug->fd, (uint32_t)op, arg, arg_len, NULL,
                                0, NULL, 0, NULL, &hotplug->seq);
    } else if (hotplug->transport == SLASH_TRANSPORT_IOCTL) {
        if (attach_bdf) {
            rv = ioctl(hotplug->fd, op, arg);
        } else {
            rv = ioctl(hotplug->fd, op);
        }
    } else {
        rv = -EINVAL;
    }

    if (rv < 0) {
        errno = -rv;
        return -1;
    }

    return 0;
}

struct slash_hotplug *slash_hotplug_open(const char *path) {
    const char *open_path;
    struct slash_hotplug *hotplug;
    int is_sock;

    open_path = path;
    if (open_path == NULL) {
        open_path = SLASH_HOTPLUG_DEFAULT_PATH;
    }

    hotplug = calloc(1, sizeof(*hotplug));
    if (hotplug == NULL) {
        return NULL;
    }
    hotplug->seq = 0;

    is_sock = slash_path_is_socket(open_path);
    if (is_sock < 0) {
        free(hotplug);
        return NULL;
    }

    if (is_sock) {
        hotplug->fd = slash_sock_connect(open_path);
        hotplug->transport = SLASH_TRANSPORT_SOCKET;
    } else {
        hotplug->fd = open(open_path, O_RDWR | O_CLOEXEC);
        hotplug->transport = SLASH_TRANSPORT_IOCTL;
    }

    if (hotplug->fd < 0) {
        free(hotplug);
        return NULL;
    }

    return hotplug;
}

int slash_hotplug_close(struct slash_hotplug *hotplug) {
    int ret;

    if (hotplug == NULL) {
        errno = EINVAL;
        return -1;
    }

    ret = 0;
    if (hotplug->fd >= 0 && close(hotplug->fd) != 0) {
        ret = -1;
    }

    /* Free unconditionally — the handle is invalid after this call
     * regardless of whether close() succeeded. */
    free(hotplug);

    return ret;
}

/* ─────────────────────────────────────────────────────────────────────
 * Public hotplug operations — each is a thin wrapper over an ioctl.
 * ───────────────────────────────────────────────────────────────────── */

int slash_hotplug_rescan(struct slash_hotplug *hotplug) {
    return slash_hotplug_ioctl(hotplug, SLASH_HOTPLUG_IOCTL_RESCAN, false,
                               NULL);
}

int slash_hotplug_remove(struct slash_hotplug *hotplug, const char *bdf) {
    return slash_hotplug_ioctl(hotplug, SLASH_HOTPLUG_IOCTL_REMOVE, true, bdf);
}

int slash_hotplug_toggle_sbr(struct slash_hotplug *hotplug, const char *bdf) {
    return slash_hotplug_ioctl(hotplug, SLASH_HOTPLUG_IOCTL_TOGGLE_SBR, true,
                               bdf);
}

int slash_hotplug_hotplug(struct slash_hotplug *hotplug, const char *bdf) {
    return slash_hotplug_ioctl(hotplug, SLASH_HOTPLUG_IOCTL_HOTPLUG, true, bdf);
}
