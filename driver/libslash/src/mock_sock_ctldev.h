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

/**
 * @brief State specific to mock sock servers implementing the
 * /dev/slash_ctl endpoint.
 */
struct slash_mock_sock_ctldev_state
{
    /** @brief The three memfds for the three supported BARs. */
    int bar_fd[3];
};
#define LIBSLASH_MOCK_SOCK_BAR_SIZE (4ULL * 1024ULL * 1024ULL)

/**
 * @brief Initialize the state of a CTLDEV endpoint.
 *
 * This will set up all internal fields.
 *
 * @param state Non-owned pointer to the state struct to initialize.
 * @return Zero if successfull, otherwise a negative error number.
 */
int slash_mock_sock_ctldev_init_state(
    struct slash_mock_sock_ctldev_state *state);

/**
 * @brief Clean up the state of a CTLDEV endpoint.
 *
 * This will not free the state struct; This is left to the caller.
 *
 * @param state Non-owned pointer to the state to clean.
 * @return Zero if successfull, otherwise a negative erro number.
 */
int slash_mock_sock_ctldev_release_state(
    struct slash_mock_sock_ctldev_state *state);

/**
 * @brief Dispatch a control operation onto a CTLDEV endpoint.
 *
 * @param state Non-owning reference ot the state of the endpoint.
 * @param op The opcode of the control operation.
 * @param arg Non-owning reference to the control operation's argument struct.
 * @param input_fds Non-owning reference to an array of file descriptors passed
 * from the user to the endpoint.
 * @param n_input_fds The number of file descriptors in @ref input_fds, or zero
 * if @ref input_fds is NULL. Must be <= MAX_FDS.
 * @param output_fds Non-owned array to write output fds to, which are to be
 * sent to the user
 * @param n_output_fds Non-owned reference to the capacity/size of @ref
 * output_fds. The original value is the capacity of the array in number of FDs,
 * and this function sets it to the actual number of FDs emitted.
 * @return 0 on success, a negative errno on failure.
 */
int slash_mock_sock_ctldev_dispatch(struct slash_mock_sock_ctldev_state *state,
                                    int op, void *arg, size_t arg_size,
                                    int *input_fds, size_t n_input_fds,
                                    int **output_fds, size_t *n_output_fds);

#endif