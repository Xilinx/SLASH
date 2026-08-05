/**
 * Copyright (C) 2026 Paderborn Center for Parallel Computing, Paderborn
 * University
 *
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

#define _GNU_SOURCE

#define BAR_LENGTH (1ull << 17)

#include "mock_sock_ctldev.h"
#include "mock_sock.h"

#include <slash/uapi/slash_interface.h>

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

int slash_mock_sock_ctldev_init_state(
    struct slash_mock_sock_ctldev_state *state) {
    int i_bar, rv;
    int bar_fd[3] = {-1, -1, -1};

    if (state == NULL) {
        return -EINVAL;
    }
    memset(state, 0, sizeof(*state));

    for (i_bar = 0; i_bar < 3; i_bar++) {
        bar_fd[i_bar] = memfd_create("slash_mock_sock_buf", MFD_CLOEXEC);
        if (bar_fd[i_bar] == -1) {
            rv = errno;
            goto cleanup;
        }
        if (ftruncate(bar_fd[i_bar], BAR_LENGTH) == -1) {
            rv = errno;
            goto cleanup;
        }
    }

    memcpy(state->bar_fd, bar_fd, sizeof(bar_fd));
    return 0;

cleanup:
    for (i_bar = 0; i_bar < 3; i_bar++) {
        if (bar_fd[i_bar] >= 0) {
            close(bar_fd[i_bar]);
        }
    }
    return rv;
}

int slash_mock_sock_ctldev_release_state(
    struct slash_mock_sock_ctldev_state *state) {
    int i_bar, successful = 1;

    if (state == NULL) {
        return -EINVAL;
    }

    for (i_bar = 0; i_bar < 3; i_bar++) {
        successful &= close(state->bar_fd[i_bar]) == 0;
    }

    if (successful) {
        return 0;
    } else {
        return -errno;
    }
}

static int ctldev_ioctl_get_bar_info(void *arg, size_t arg_size) {
    struct slash_ioctl_bar_info bar_info;

    if (arg == NULL) {
        return -EINVAL;
    }

    bar_info.size =
        slash_checked_copy_from_user(&bar_info, sizeof(bar_info), arg, arg_size,
                                     SLASH_IOCTL_BAR_INFO_MIN_SIZE);
    if (bar_info.size == -1 || bar_info.bar_number >= 6) {
        return -EINVAL;
    }

    bar_info.usable = (bar_info.bar_number % 2) == 0;
    bar_info.in_use = 0;
    bar_info.start_address = 0;
    bar_info.length = BAR_LENGTH;

    memcpy(arg, &bar_info, bar_info.size);
    return 0;
}

static int ctldev_ioctl_get_device_info(void *arg, size_t arg_size) {
    struct slash_ioctl_device_info dev_info;

    if (arg == NULL) {
        return -EINVAL;
    }

    dev_info.size =
        slash_checked_copy_from_user(&dev_info, sizeof(dev_info), arg, arg_size,
                                     SLASH_IOCTL_DEVICE_INFO_MIN_SIZE);
    if (dev_info.size == -1) {
        return -EINVAL;
    }

    strcpy(dev_info.bdf, LIBSLASH_MOCK_SOCK_CTLDEV_BDF);
    dev_info.vendor_id = 0x10EE; /* AMD/Xilinx */
    dev_info.device_id = 0x50B6;
    dev_info.subsystem_vendor_id = 0x10EE;
    dev_info.subsystem_device_id = 0x000e;

    memcpy(arg, &dev_info, dev_info.size);
    return 0;
}

static int
ctldev_ioctl_bar_fd_request(struct slash_mock_sock_ctldev_state *state,
                            void *arg, size_t arg_size, int **output_fds,
                            size_t *n_output_fds) {
    struct slash_ioctl_bar_fd_request fd_request;
    int rv;

    if (arg == 0 || output_fds == 0 || n_output_fds == 0) {
        return -EINVAL;
    }

    fd_request.size =
        slash_checked_copy_from_user(&fd_request, sizeof(fd_request), arg,
                                     arg_size, SLASH_IOCTL_BAR_FD_MIN_SIZE);

    if (fd_request.size == -1 || fd_request.bar_number > 6 || (fd_request.bar_number % 2) != 0) {
        return -EINVAL;
    }

    *output_fds = malloc(sizeof(int));
    if (*output_fds == NULL) {
        return -ENOMEM;
    }

    **output_fds = dup(state->bar_fd[fd_request.bar_number >> 1]);
    if (**output_fds == -1) {
        rv = -errno;
        goto cleanup;
    }
    *n_output_fds = 1;

    fd_request.length = BAR_LENGTH;
    memcpy(arg, &fd_request, fd_request.size);

    return 0;

cleanup:
    free(*output_fds);
    *n_output_fds = 0;
    return -rv;
}

int slash_mock_sock_ctldev_dispatch(struct slash_mock_sock_ctldev_state *state,
                                    int op, void *arg, size_t arg_size,
                                    int *input_fds, size_t n_input_fds,
                                    int **output_fds, size_t *n_output_fds) {
    *output_fds = NULL;
    *n_output_fds = 0;
    switch (op) {
    case SLASH_CTLDEV_IOCTL_GET_BAR_INFO:
        return ctldev_ioctl_get_bar_info(arg, arg_size);
    case SLASH_CTLDEV_IOCTL_GET_BAR_FD:
        return ctldev_ioctl_bar_fd_request(state, arg, arg_size, output_fds, n_output_fds);
    case SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO:
        return ctldev_ioctl_get_device_info(arg, arg_size);
    default:
        return -ENOTTY;
    }
}