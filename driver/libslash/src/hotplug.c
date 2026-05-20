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

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

/**
 * slash_hotplug_ioctl_without_request() — Issue an ioctl that takes no
 * argument struct (used by RESCAN).
 *
 * @hotplug: Open hotplug handle.  Must not be NULL.
 * @op:      ioctl request number.
 *
 * Returns 0 on success, -1 on failure (errno set by ioctl or EINVAL).
 */
static int slash_hotplug_ioctl_without_request(struct slash_hotplug *hotplug, unsigned long op)
{
    int ret;

    if (hotplug == NULL) {
        errno = EINVAL;
        return -1;
    }

    ret = ioctl(hotplug->fd, op);
    if (ret < 0) {
        return -1;
    }

    return 0;
}

/**
 * slash_hotplug_ioctl_with_request() — Issue an ioctl that carries a
 * slash_hotplug_device_request identifying a device by BDF.
 *
 * @hotplug: Open hotplug handle.  Must not be NULL.
 * @op:      ioctl request number.
 * @bdf:     PCI BDF string, or NULL / empty string to let the kernel
 *           pick the only tracked device.  Must be shorter than
 *           SLASH_HOTPLUG_BDF_LEN bytes (including NUL); otherwise
 *           EINVAL is returned.
 *
 * Returns 0 on success, -1 on failure.
 */
static int slash_hotplug_ioctl_with_request(
    struct slash_hotplug *hotplug,
    unsigned long op,
    const char *bdf
)
{
    struct slash_hotplug_device_request req;
    size_t len;
    int ret;

    if (hotplug == NULL) {
        errno = EINVAL;
        return -1;
    }

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

    ret = ioctl(hotplug->fd, op, &req);
    if (ret < 0) {
        return -1;
    }

    return 0;
}

struct slash_hotplug *slash_hotplug_open(const char *path)
{
    const char *open_path;
    struct slash_hotplug *hotplug;

    open_path = path;
    if (open_path == NULL) {
        open_path = SLASH_HOTPLUG_DEFAULT_PATH;
    }

    hotplug = calloc(1, sizeof(*hotplug));
    if (hotplug == NULL) {
        return NULL;
    }

    hotplug->fd = open(open_path, O_RDWR | O_CLOEXEC);
    if (hotplug->fd < 0) {
        free(hotplug);
        return NULL;
    }

    return hotplug;
}

int slash_hotplug_close(struct slash_hotplug *hotplug)
{
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

int slash_hotplug_rescan(struct slash_hotplug *hotplug)
{
    return slash_hotplug_ioctl_without_request(hotplug, SLASH_HOTPLUG_IOCTL_RESCAN);
}

int slash_hotplug_remove(struct slash_hotplug *hotplug, const char *bdf)
{
    return slash_hotplug_ioctl_with_request(hotplug, SLASH_HOTPLUG_IOCTL_REMOVE, bdf);
}

int slash_hotplug_toggle_sbr(struct slash_hotplug *hotplug, const char *bdf)
{
    return slash_hotplug_ioctl_with_request(hotplug, SLASH_HOTPLUG_IOCTL_TOGGLE_SBR, bdf);
}

int slash_hotplug_hotplug(struct slash_hotplug *hotplug, const char *bdf)
{
    return slash_hotplug_ioctl_with_request(hotplug, SLASH_HOTPLUG_IOCTL_HOTPLUG, bdf);
}
