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

#define MOCK_SOCK_QDMA_HBM_BASE 0x0000004000000000ull
#define MOCK_SOCK_QDMA_HBM_END 0x0000004800000000ull
#define MOCK_SOCK_QDMA_DDR_BASE 0x0000060000000000ull
#define MOCK_SOCK_QDMA_DDR_END 0x0000060800000000ull

#include "mock_sock_qdma.h"
#include "mock_sock.h"

#include <slash/uapi/slash_interface.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int slash_mock_sock_qdma_init_state(struct slash_mock_sock_qdma_state *state) {
    int rv;
    pthread_mutexattr_t attr;

    if (state == NULL) {
        return -EINVAL;
    }

    memset(state, 0, sizeof(*state));

    rv = pthread_mutexattr_init(&attr);
    if (rv != 0)
        return -rv;
    rv = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    if (rv != 0) {
        pthread_mutexattr_destroy(&attr);
        return -rv;
    }
    rv = pthread_mutex_init(&state->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rv != 0) {
        return -rv;
    }

    state->refcount = 1;

    state->hbm_memory = malloc(1ul << 35);
    if (state->hbm_memory == NULL) {
        rv = -ENOMEM;
        goto cleanup;
    }
    state->ddr_memory = malloc(1ul << 35);
    if (state->ddr_memory == NULL) {
        rv = -ENOMEM;
        goto cleanup;
    }

    return 0;

cleanup:
    free(state->ddr_memory);
    free(state->hbm_memory);
    pthread_mutex_destroy(&state->mutex);
    return rv;
}

int slash_mock_sock_qdma_get_state(struct slash_mock_sock_qdma_state *state) {
    int rv;

    if (state == NULL) {
        return -EINVAL;
    }

    rv = pthread_mutex_lock(&state->mutex);
    if (rv != 0) {
        return -rv;
    }

    /* Defensive check */
    if (state->refcount <= 0) {
        pthread_mutex_unlock(&state->mutex);
        return -EINVAL;
    }

    state->refcount += 1;

    rv = pthread_mutex_unlock(&state->mutex);
    if (rv != 0) {
        return -rv;
    }
    return 0;
}

int slash_mock_sock_qdma_put_state(struct slash_mock_sock_qdma_state *state) {
    int rv, do_free;

    if (state == NULL) {
        return -EINVAL;
    }

    rv = pthread_mutex_lock(&state->mutex);
    if (rv != 0) {
        return -rv;
    }

    if (state->refcount <= 0) {
        pthread_mutex_unlock(&state->mutex);
        return -EINVAL;
    }

    state->refcount -= 1;
    do_free = state->refcount == 0;

    rv = pthread_mutex_unlock(&state->mutex);
    if (rv != 0) {
        return -rv;
    }

    if (do_free) {
        pthread_mutex_destroy(&state->mutex);
        free(state->ddr_memory);
        free(state->hbm_memory);
        free(state);
    }
    return 0;
}

/**
 * @brief Execute the SLASH_QDMA_IOCTL_INFO operation
 *
 * This treats @ref arg as reference to a @ref slash_qdma_info struct and
 * populate it with data.
 *
 * @param arg Non-owning reference to the argument buffer to read and write.
 * @param arg_size Size of the argument buffer in bytes.
 */
static int qdma_ioctl_info(void *arg, size_t arg_size) {
    struct slash_qdma_info info;
    __u32 out_size;

    if (arg == NULL) {
        return -EINVAL;
    }

    out_size = slash_checked_copy_from_user(&info, sizeof(info), arg, arg_size,
                                            SLASH_QDMA_INFO_MIN_SIZE);
    if (out_size == -1) {
        return -EINVAL;
    }

    info.size = out_size;
    info.qsets_max = 0;
    info.msix_qvecs = 0;
    info.vf_max = 0;
    info.caps = 0;
    strcpy(info.bdf, LIBSLASH_MOCK_SOCK_QDMA_BDF);
    memcpy(arg, &info, out_size);

    return 0;
}

/**
 * @brief Execute the SLASH_QDMA_IOCL_QPAIR_ADD operation.
 *
 * This operation looks for the first qpair that is not in use and allocates it
 * for the user. It then returns the qid as an out-field of the argument struct.
 *
 * @param state Non-owning reference to the QDMA endpoint state
 * @param arg Non-owning reference to the operation argument. Interpreted as an
 * instance of @ref slash_qdma_qpair_add, following the ABI versioning rules.
 * @param arg_size Apparent size of the argument struct.
 * @return 0 on success, negative error number on failure.
 */
static int qdma_ioctl_qpair_add(struct slash_mock_sock_qdma_state *state,
                                void *arg, size_t arg_size) {
    struct slash_qdma_qpair_add qpair_add;
    __u32 out_size;
    __u32 qid;
    int rv;

    if (state == NULL || arg == NULL) {
        return -EINVAL;
    }

    out_size =
        slash_checked_copy_from_user(&qpair_add, sizeof(qpair_add), arg,
                                     arg_size, SLASH_QDMA_QPAIR_ADD_MIN_SIZE);
    if (out_size < 0) {
        return -EINVAL;
    }

    if ((rv = pthread_mutex_lock(&state->mutex)) != 0) {
        return -rv;
    }

    for (qid = 0; qid < LIBSLASH_MOCK_SOCK_MAX_QPAIRS; qid++) {
        if (!(state->qpairs[qid] & 0b01)) {
            state->qpairs[qid] = 0b01;
            state->aperture_size[qid] = qpair_add.aperture_size;
            break;
        }
    }

    if ((rv = pthread_mutex_unlock(&state->mutex)) != 0) {
        return -rv;
    }

    if (qid == LIBSLASH_MOCK_SOCK_MAX_QPAIRS) {
        /* No qpair free, no need to write back data, thus exiting early */
        return -EBUSY;
    }

    qpair_add.size = out_size;
    qpair_add.qid = qid;
    memcpy(arg, &qpair_add, out_size);
    return 0;
}

/**
 * @brief Execute the SLASH_QDMA_IOCL_Q_OP operation.
 *
 * If the referenced qpair has been created before, it either starts, stops, or
 * deletes the qpair.
 *
 * @param state Non-owning reference to the QDMA endpoint state
 * @param arg Non-owning reference to the operation argument. Interpreted as an
 * instance of @ref slash_qdma_qpair_op, following the ABI versioning rules.
 * @param arg_size Apparent/maximal size of the argument struct.
 * @return 0 on success, negative error number on failure.
 */
static int qdma_ioctl_qpair_op(struct slash_mock_sock_qdma_state *state,
                               void *arg, size_t arg_size) {
    struct slash_qdma_qpair_op qpair_op;
    __u32 out_size;
    int rv0, rv1;

    if (state == NULL || arg == NULL) {
        return -EINVAL;
    }

    out_size =
        slash_checked_copy_from_user(&qpair_op, sizeof(qpair_op), arg, arg_size,
                                     SLASH_QDMA_QPAIR_OP_MIN_SIZE);
    if (out_size == -1) {
        return -EINVAL;
    }

    if ((rv0 = pthread_mutex_lock(&state->mutex)) != 0) {
        return -rv0;
    }

    if (qpair_op.qid >= LIBSLASH_MOCK_SOCK_MAX_QPAIRS ||
        !(state->qpairs[qpair_op.qid] & 0b01)) {
        rv0 = -ENOENT;
    } else {
        switch (qpair_op.op) {
        case SLASH_QDMA_QUEUE_OP_START:
            state->qpairs[qpair_op.qid] |= 0b10;
            rv0 = 0;
            break;
        case SLASH_QDMA_QUEUE_OP_STOP:
            state->qpairs[qpair_op.qid] &= ~0b10;
            rv0 = 0;
            break;
        case SLASH_QDMA_QUEUE_OP_DEL:
            state->qpairs[qpair_op.qid] = 0;
            rv0 = 0;
            break;
        default:
            rv0 = -ENOTTY;
        }
    }

    if ((rv1 = pthread_mutex_unlock(&state->mutex)) != 0) {
        return -rv1;
    } else {
        return rv0;
    }
    /* No out fields, no need to write back*/
}

/**
 * @brief Execute the SLASH_QDMA_IOCTL_BUF_CREATE operation.
 *
 * This creates a "host buffer", which in this case is only a memfd of the
 * specified size.
 *
 * @param arg Non-owning reference to the operation argument. Interpreted as an
 * instance of @ref slash_qdma_buf_create, following the ABI versioning rules.
 * @param arg_size Apparent size of the argument struct.
 * @param output_fds Non-owned array to write the buffer FD to.
 * @param n_output_fds Non-owned reference to the size of the @ref output_fds
 * array.
 * @return 0 on success, negative error number on failure.
 */
static int qdma_ioctl_buf_create(void *arg, size_t arg_size, int **output_fds,
                                 size_t *n_output_fds) {

    struct slash_qdma_buf_create buf_create;
    __u32 out_size;
    int buf_fd, rv;
    unsigned int memfd_flags = 0;
    long page_size = sysconf(_SC_PAGESIZE);

    if (arg == NULL || output_fds == NULL || n_output_fds == NULL) {
        return -EINVAL;
    }

    *output_fds = NULL;
    *n_output_fds = 0;

    if (page_size < 0) {
        return -ENOMEM;
    }

    out_size =
        slash_checked_copy_from_user(&buf_create, sizeof(buf_create), arg,
                                     arg_size, SLASH_QDMA_BUF_CREATE_MIN_SIZE);
    if (out_size == -1) {
        return -EINVAL;
    }

    if ((buf_create.flags & O_CLOEXEC) != 0) {
        memfd_flags = MFD_CLOEXEC;
    }

    buf_fd = memfd_create("slash_mock_sock_buf", memfd_flags);
    if (buf_fd == -1) {
        return -ENOMEM;
    }

    rv = ftruncate(buf_fd, buf_create.length);
    if (rv == -1) {
        close(buf_fd);
        return -ENOMEM;
    }

    *output_fds = malloc(sizeof(int));
    **output_fds = buf_fd;
    *n_output_fds = 1;

    buf_create.size = out_size;
    buf_create.granule = page_size;
    buf_create.transfer_hint = SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR;
    memcpy(arg, &buf_create, out_size);

    /* The kernel would return the FD here, but with the socket transport, we
     * have to return the index of the FD in the list of output FDs.
     * Coincidentally, we emit only one FD, so we still return 0. */
    return 0;
}

/**
 * @brief Execute the SLASH_QDMA_IOCTL_QPAIR_GET_FD operation.
 *
 * This creates a new endpoint plus background thread, which "uses" the
 * referenced qpairs to offer memory transfer operations.
 *
 * @param state Non-owning reference to the QDMA endpoint state. However, the
 * caller must be a co-owner of the QDMA endpoint state, since the function will
 * `get` the state to make the new worker thread a co-owner.
 * @param arg Non-owning reference to the operation argument. Interpreted as an
 * instance of @ref slash_qdma_qpair_fd_request, following the ABI versioning
 * rules.
 * @param arg_size Apparent size of the argument struct.
 * @param output_fds Non-owned array to write the socket FD to.
 * @param n_output_fds Non-owned reference to the size of the @ref
 * output_fds array.
 * @return 0 on success, negative error number on failure.
 */
static int qdma_ioctl_qpair_fd_request(struct slash_mock_sock_qdma_state *state,
                                       void *arg, size_t arg_size,
                                       int **output_fds, size_t *n_output_fds) {
    struct slash_qdma_qpair_fd_request request;
    struct slash_mock_sock_qpair_state *qpair_state = NULL;
    __u32 out_size = 0;
    size_t i_qpair = 0;
    int rv = 0;

    if (arg == NULL || output_fds == NULL || n_output_fds == NULL) {
        return -EINVAL;
    }

    *output_fds = malloc(sizeof(int));
    if (*output_fds == NULL) {
        return -ENOMEM;
    }

    out_size =
        slash_checked_copy_from_user(&request, sizeof(request), arg, arg_size,
                                     SLASH_QDMA_QPAIR_FD_REQUEST_MIN_SIZE);
    if (out_size == -1) {
        rv = -EINVAL;
        goto cleanup;
    }

    qpair_state = calloc(1, sizeof(*qpair_state));
    if (qpair_state == NULL) {
        rv = -ENOMEM;
        goto cleanup;
    }

    if ((rv = slash_mock_sock_qdma_get_state(state)) != 0) {
        goto cleanup;
    }
    qpair_state->main_state = state;

    if (request.qpair_count == 0) {
        qpair_state->qpair_ids[0] = request.qid;
        qpair_state->n_qpairs = 1;
    } else if (request.qpair_count <= SLASH_QDMA_FD_MAX_QPAIRS) {
        qpair_state->n_qpairs = request.qpair_count;
        for (i_qpair = 0; i_qpair < qpair_state->n_qpairs; i_qpair++) {
            qpair_state->qpair_ids[i_qpair] = request.qpair_ids[i_qpair];
        }
    } else {
        rv = -EINVAL;
        goto cleanup;
    }

    /* Not checking whether the qpairs are existing and running. This would be
     * pointless anyways, since the state mutex needs to be released in order to
     * create a new co-owned reference to the qdma state. It could therefore be
     * possible that we check the qpair state, and then another IOCTL stops the
     * qpair before we can create the new endpoint. Instead, the qpair FD
     * endpoint checks the qpair state before every actual user operation.*/

    if ((rv = slash_mock_sock_create_with_state(SLASH_MOCK_SOCK_ENDPOINT_QPAIR,
                                                qpair_state)) < 0) {
        goto cleanup;
    }

    **output_fds = rv;
    *n_output_fds = 1;
    return 0;

cleanup:
    if (qpair_state != NULL && qpair_state->main_state != NULL) {
        slash_mock_sock_qdma_put_state(qpair_state->main_state);
    }
    free(qpair_state);
    free(*output_fds);
    *n_output_fds = 0;
    return rv;
}

int slash_mock_sock_qdma_dispatch(struct slash_mock_sock_qdma_state *state,
                                  int op, void *arg, size_t arg_size,
                                  int *input_fds, size_t n_input_fds,
                                  int **output_fds, size_t *n_output_fds) {
    *output_fds = NULL;
    *n_output_fds = 0;
    switch (op) {
    case SLASH_QDMA_IOCTL_INFO:
        return qdma_ioctl_info(arg, arg_size);
    case SLASH_QDMA_IOCTL_QPAIR_ADD:
        return qdma_ioctl_qpair_add(state, arg, arg_size);
    case SLASH_QDMA_IOCTL_Q_OP:
        return qdma_ioctl_qpair_op(state, arg, arg_size);
    case SLASH_QDMA_IOCTL_BUF_CREATE:
        return qdma_ioctl_buf_create(arg, arg_size, output_fds, n_output_fds);
    case SLASH_QDMA_IOCTL_QPAIR_GET_FD:
        return qdma_ioctl_qpair_fd_request(state, arg, arg_size, output_fds,
                                           n_output_fds);
    default:
        return -ENOTTY;
    }
}

int slash_mock_sock_qpair_release_state(
    struct slash_mock_sock_qpair_state *qpair_state) {
    return slash_mock_sock_qdma_put_state(qpair_state->main_state);
}

/**
 * @brief Execute the SLASH_QDMA_QPAIR_IOCTL_TRANSFER operation.
 *
 * This executes one or more transfer operations, either to the "card" memory or
 * from it. The host-side memory is represented by a file descriptor, from which
 * this function preads or pwrites.
 *
 * @param state Non-owning reference to the Qpair endpoint state.
 * @param arg Non-owning reference to the operation argument. Interpreted as an
 * instance of @ref slash_qdma_transfer, following the ABI versioning rules.
 * @param arg_size Apparent size of the argument struct.
 * @param input_fds Non-owned array of FDs to use in the transfer operations
 * @param n_input_fds Number of FDs is @ref input_fds.
 * @return 0 on success, negative error number on failure.
 */
static int qdma_qpair_ioctl_transfer(struct slash_mock_sock_qpair_state *state,
                                     void *arg, size_t arg_size, int *input_fds,
                                     size_t n_input_fds) {
    int rv = 0;
    struct slash_qdma_transfer request;
    int mutex_locked = 0;
    size_t i_subxfer;
    struct slash_qdma_subxfer *subxfer; /* Non-owned reference */
    char *dev_ptr;                      /* Non-owned reference */
    ssize_t n_transferred;
    int total_transferred = 0;

    if (arg == NULL || input_fds == NULL) {
        return -EINVAL;
    }

    /* Copy in the request from the user's argument. */
    if (slash_checked_copy_from_user(&request, sizeof(request), arg, arg_size,
                                     SLASH_QDMA_TRANSFER_MIN_SIZE) == -1) {
        rv = -EINVAL;
        goto cleanup;
    }

    if ((rv = pthread_mutex_lock(&state->main_state->mutex)) != 0) {
        rv = -rv;
        goto cleanup;
    }
    mutex_locked = 1;

    /* Is the number of subxfers valid? */
    if (request.count > SLASH_QDMA_FD_MAX_QPAIRS) {
        rv = -EINVAL;
        goto cleanup;
    }

    for (i_subxfer = 0; i_subxfer < request.count; i_subxfer++) {
        subxfer = &request.xfers[i_subxfer];

        /* Is the qpair index valid? */
        if (subxfer->qpair_index > state->n_qpairs) {
            rv = -EINVAL;
            goto cleanup;
        }
        /* Is the indexed qpair running? */
        if (state->main_state->qpairs[state->qpair_ids[subxfer->qpair_index]] !=
            0b11) {
            rv = -ENODEV;
            goto cleanup;
        }

        /* Is the buf FD index valid? */
        if (subxfer->buf_fd > n_input_fds) {
            rv = -EINVAL;
            goto cleanup;
        }
        /* We're not fstat-ing the FD here since the advertised properties may
         * be different from the true ones. The validity of the indexed buf FD
         * and buf_offset are thus only tested by the actual copy operation.*/

        /* Compute the endpoint-side device address. */
        if (subxfer->dev_addr >= MOCK_SOCK_QDMA_HBM_BASE &&
            (subxfer->dev_addr + subxfer->length) <= MOCK_SOCK_QDMA_HBM_END) {
            dev_ptr = state->main_state->hbm_memory +
                      (subxfer->dev_addr - MOCK_SOCK_QDMA_HBM_BASE);
        } else if (subxfer->dev_addr >= MOCK_SOCK_QDMA_DDR_BASE &&
                   (subxfer->dev_addr + subxfer->length) <=
                       MOCK_SOCK_QDMA_DDR_END) {
            dev_ptr = state->main_state->ddr_memory +
                      (subxfer->dev_addr - MOCK_SOCK_QDMA_DDR_BASE);
        } else {
            rv = -EINVAL;
            goto cleanup;
        }

        /* Iterate until the entire transfer is complete. We track the state in
         * the length and buf_offset fields of the subxfer struct as well as the
         * dev_ptr. */
        while (subxfer->length > 0) {
            /* Attempt to execute the read or write operation */
            if (subxfer->direction == SLASH_QDMA_XFER_H2C) {
                n_transferred = pread(input_fds[subxfer->buf_fd], dev_ptr,
                                      subxfer->length, subxfer->buf_offset);
            } else if (subxfer->direction == SLASH_QDMA_XFER_C2H) {
                n_transferred = pwrite(input_fds[subxfer->buf_fd], dev_ptr,
                                       subxfer->length, subxfer->buf_offset);
            } else {
                rv = -EINVAL;
                goto cleanup;
            }

            if (n_transferred == -1) {
                /* Error case, either abort for "big" errors or retry for
                 * interruptions */
                switch (errno) {
                case EAGAIN:
                case EINTR:
                    continue;
                default:
                    rv = -EFAULT;
                    goto cleanup;
                }
            } else if (n_transferred == 0) {
                /* We have reached the end of the file, but the user wants us to
                 * transfer more. This is an error! */
                rv = -EFAULT;
                goto cleanup;
            }

            /* The transfer was successful, but not necessarily complete. Update
             * the indices, so that we try again if the transfer was incomplete.
             */
            dev_ptr += n_transferred;
            subxfer->length -= n_transferred;
            subxfer->buf_offset += n_transferred;
            total_transferred += n_transferred;
        }
    }

    mutex_locked = 0;
    if ((rv = pthread_mutex_unlock(&state->main_state->mutex)) != 0) {
        rv = -rv;
        goto cleanup;
    }

    return total_transferred;

cleanup:
    if (mutex_locked) {
        pthread_mutex_unlock(&state->main_state->mutex);
    }
    return rv;
}

int slash_mock_sock_qpair_dispatch(struct slash_mock_sock_qpair_state *state,
                                   int op, void *arg, size_t arg_size,
                                   int *input_fds, size_t n_input_fds,
                                   int **output_fds, size_t *n_output_fds) {
    *output_fds = NULL;
    *n_output_fds = 0;
    switch (op) {
    case SLASH_QDMA_IOCTL_BUF_CREATE:
        return qdma_ioctl_buf_create(arg, arg_size, output_fds, n_output_fds);
    case SLASH_QDMA_QPAIR_IOCTL_TRANSFER:
        return qdma_qpair_ioctl_transfer(state, arg, arg_size, input_fds,
                                         n_input_fds);
    default:
        return -ENOTTY;
    }
}
