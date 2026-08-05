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
#ifndef LIBSLASH_MOCK_SOCK_QDMA_H
#define LIBSLASH_MOCK_SOCK_QDMA_H

#define LIBSLASH_MOCK_SOCK_MAX_QPAIRS 256

#include <pthread.h>

/**
 * @brief State specific to mock sock servers implementing the
 * /dev/slash_qdma_ctl endpoint.
 */
struct slash_mock_sock_qdma_state {
    /**
     * @brief Mutex guarding all following fields
     */
    pthread_mutex_t mutex;
    /**
     * @brief The number of active co-owned references to the state.
     *
     * Once @ref slash_mock_sock_qdma_put_state decreases this value to zero,
     * the state will be torn down and freed.
     */
    int refcount;
    /** @brief Qpair state bitsets.
     *
     * Bit 0 (0b01): Qpair used.
     * Bit 1 (0b10): Qpair started.
     */
    int qpairs[LIBSLASH_MOCK_SOCK_MAX_QPAIRS];
    /** @brief The configured aperture of the qpair.
     */
    int aperture_size[LIBSLASH_MOCK_SOCK_MAX_QPAIRS];
};

/**
 * @brief Initialize the state of a QDMA endpoint.
 *
 * This will setup all internal fields. In particular, it will set up the mutex
 * and set the reference counter to 1.
 *
 * @param state Non-owned pointer to the state struct to initialize
 * @return Zero if successfull, otherwise a negative error number.
 */
int slash_mock_sock_qdma_init_state(struct slash_mock_sock_qdma_state *state);

/**
 * @brief Increase the state reference counter
 *
 * This is needed to create a new co-owned reference to the state.
 *
 * @param state Non-owned pointer to the state to modify.
 * @return Zero if successfull, otherwise a negative error number.
 */
int slash_mock_sock_qdma_get_state(struct slash_mock_sock_qdma_state *state);

/**
 * @brief Decrease the state's reference counter, potentially freeing it.
 *
 * This function will look the state's mutex, decrease the reference counter,
 * and free it if the reference counter has reached zero.
 *
 * @param state Co-owned reference to the state to modify.
 * @return Zero if successful, otherwise a negative error number.
 */
int slash_mock_sock_qdma_put_state(struct slash_mock_sock_qdma_state *state);

/**
 * @brief Dispatch the IOCTL operation in the QDMA control file endpoint.
 *
 * @param state Non-owning reference to the QDMA endpoint state.
 * @param op Op-code of the IOCTL to dispatch.
 * @param arg Non-owning reference to the argument struct.
 * @param arg_size Apparent/maximal size of the argument struct.
 * @param input_fds Non-owned array of fds sent by the user, to be used in the
 * operation.
 * @param n_input_fds Size of the @ref input_fds array, <= MAX_FDS
 * @param output_fds Non-owned array to write output fds to, which are to be
 * sent to the user
 * @param n_output_fds Non-owned reference to the capacity/size of @ref
 * output_fds. The original value is the capacity of the array in number of FDs,
 * and this function sets it to the actual number of FDs emitted.
 */
int slash_mock_sock_qdma_dispatch(struct slash_mock_sock_qdma_state *state,
                                  int op, void *arg, size_t arg_size,
                                  int *input_fds, size_t n_input_fds,
                                  int **output_fds, size_t *n_output_fds);

#endif