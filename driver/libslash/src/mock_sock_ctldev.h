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
#ifndef LIBSLASH_MOCK_SOCK_CTLDEV_H
#define LIBSLASH_MOCK_SOCK_CTLDEV_H

#include <stddef.h>

struct slash_mock_sock_ctldev_state {
    int bar_fd[3];
};
#define LIBSLASH_MOCK_SOCK_BAR_SIZE (4ULL * 1024ULL * 1024ULL)

int slash_mock_sock_ctldev_init_state(
    struct slash_mock_sock_ctldev_state *state);
int slash_mock_sock_ctldev_release_state(
    struct slash_mock_sock_ctldev_state *state);
int slash_mock_sock_ctldev_dispatch(struct slash_mock_sock_ctldev_state *state,
                                    int op, void *arg, size_t arg_size,
                                    int *input_fds, size_t n_input_fds,
                                    int **output_fds, size_t *n_output_fds);

#endif