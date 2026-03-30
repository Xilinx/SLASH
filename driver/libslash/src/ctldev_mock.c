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
 * @file ctldev_mock.c
 * @brief Mock control-device implementation backed by temporary files.
 */

#define _GNU_SOURCE

#include "ctldev_mock.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SLASH_MOCK_BAR_SIZE (64ULL * 1024ULL * 1024ULL)

/** @brief Generate a random 64-bit value, falling back to time/pid XOR. */
static uint64_t slash_mock_random(void)
{
    uint64_t value;
    ssize_t ret;
    ret = getrandom(&value, sizeof(value), 0);

    if (ret != (ssize_t) sizeof(value)) {
        struct timespec ts;

        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            value = ((uint64_t) ts.tv_sec << 32) ^ (uint64_t) ts.tv_nsec;
        } else {
            value = (uint64_t) time(NULL);
        }

        value ^= (uint64_t) (uintptr_t) &value;
        value ^= (uint64_t) getpid();
    }

    return value;
}

/** @brief Create a temporary file for mock BAR storage in XDG_RUNTIME_DIR or /tmp. */
static int slash_mock_create_backing_file(char **path_out)
{
    const char *env_dir = getenv("XDG_RUNTIME_DIR");
    const char *dir_path = {0};
    int last_errno = EIO;
    size_t i;
    int attempt;

    if (env_dir != NULL && env_dir[0] != '\0') {
        dir_path = env_dir;
    } else {
        dir_path = "/tmp";
    }

    for (attempt = 0; attempt < 32; ++attempt) {
        uint64_t rnd;
        int needed;
        size_t buf_len;
        char *path;
        int fd;

        rnd = slash_mock_random();
        needed = snprintf(NULL, 0, "%s/%s%llu", dir_path, "slash.mock.", (unsigned long long) rnd);
        if (needed < 0) {
            last_errno = EINVAL;
            continue;
        }

        if ((size_t) needed >= PATH_MAX) {
            last_errno = ENAMETOOLONG;
            continue;
        }

        buf_len = (size_t) needed + 1;
        path = malloc(buf_len);
        if (path == NULL) {
            last_errno = ENOMEM;
            errno = ENOMEM;
            return -1;
        }

        (void) snprintf(path, buf_len, "%s/slash.mock.%llu", dir_path, (unsigned long long) rnd);

        fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            *path_out = path;
            return fd;
        }

        last_errno = errno;
        free(path);

        if (errno == EEXIST) {
            continue;
        }

        if (errno == ENOENT || errno == EACCES) {
            break;
        }

        return -1;
    }

    errno = last_errno;
    return -1;
}

struct slash_ctldev *slash_ctldev_mock_open(void)
{
    struct slash_ctldev *ctldev;

    ctldev = calloc(1, sizeof(*ctldev));
    if (ctldev == NULL) {
        return NULL;
    }

    ctldev->fd = -1;
    ctldev->mock = true;

    return ctldev;
}

int slash_ctldev_mock_close(struct slash_ctldev *ctldev)
{
    if (ctldev == NULL) {
        errno = EINVAL;
        return -1;
    }

    free(ctldev);

    return 0;
}

struct slash_ioctl_device_info *slash_device_info_mock_read(struct slash_ctldev *ctldev)
{
    struct slash_ioctl_device_info *info;

    if (ctldev == NULL || !ctldev->mock) {
        errno = EINVAL;
        return NULL;
    }

    info = calloc(1, sizeof(*info));
    if (info == NULL) {
        return NULL;
    }

    info->size = sizeof(*info);
    (void) snprintf(info->bdf, sizeof(info->bdf), "0000:00:00.0");

    return info;
}

struct slash_ioctl_bar_info *slash_bar_info_mock_read(struct slash_ctldev *ctldev, int bar_number)
{
    struct slash_ioctl_bar_info *bar_info;

    if (ctldev == NULL || !ctldev->mock) {
        errno = EINVAL;
        return NULL;
    }

    bar_info = calloc(1, sizeof(*bar_info));
    if (bar_info == NULL) {
        return NULL;
    }

    bar_info->size = sizeof(*bar_info);
    bar_info->bar_number = (uint8_t) bar_number;

    if (bar_number == 0) {
        bar_info->usable = 1;
        bar_info->in_use = 0;
        bar_info->start_address = 0;
        bar_info->length = SLASH_MOCK_BAR_SIZE;
    } else {
        bar_info->usable = 0;
        bar_info->in_use = 0;
        bar_info->start_address = 0;
        bar_info->length = 0;
    }

    return bar_info;
}

void slash_bar_info_mock_free(struct slash_ioctl_bar_info *bar_info)
{
    free(bar_info);
}

struct slash_bar_file *slash_bar_file_mock_open(struct slash_ctldev *ctldev, int bar_number, int flags)
{
    (void) flags;

    struct slash_bar_file *bar_file;
    char *path;
    int fd;
    void *map;

    if (ctldev == NULL || !ctldev->mock) {
        errno = EINVAL;
        return NULL;
    }

    if (bar_number != 0) {
        errno = ENODEV;
        return NULL;
    }

    bar_file = calloc(1, sizeof(*bar_file));
    if (bar_file == NULL) {
        return NULL;
    }

    path = NULL;
    fd = slash_mock_create_backing_file(&path);
    if (fd < 0) {
        free(bar_file);
        return NULL;
    }

    if (ftruncate(fd, (off_t) SLASH_MOCK_BAR_SIZE) != 0) {
        (void) unlink(path);
        free(path);
        (void) close(fd);
        free(bar_file);
        return NULL;
    }

    map = mmap(NULL, (size_t) SLASH_MOCK_BAR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        (void) unlink(path);
        free(path);
        (void) close(fd);
        free(bar_file);
        return NULL;
    }

    bar_file->fd = fd;
    bar_file->len = (size_t) SLASH_MOCK_BAR_SIZE;
    bar_file->map = map;
    bar_file->mock = true;
    bar_file->mock_path = path;

    return bar_file;
}

int slash_bar_file_mock_close(struct slash_bar_file *bar_file)
{
    int ret = 0;

    if (bar_file == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (bar_file->map != NULL && bar_file->len != 0) {
        if (munmap(bar_file->map, bar_file->len) != 0) {
            ret = -1;
        }
    }

    if (bar_file->fd >= 0) {
        if (close(bar_file->fd) != 0) {
            ret = -1;
        }
    }

    if (bar_file->mock_path != NULL) {
        if (unlink(bar_file->mock_path) != 0 && errno != ENOENT) {
            ret = -1;
        }
        free(bar_file->mock_path);
    }

    free(bar_file);

    return ret;
}
