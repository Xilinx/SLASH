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

#include "mock_sock_qdma.h"
#include "mock_sock.h"

#include <slash/uapi/slash_interface.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
    return 0;
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
 * @param n_output_fds Non-owned reference to the capacity/size of the @ref
 * output_fds array. The original value is the capacity of the array, and this
 * function sets it to the number of emitted FDs.
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

    /* The kernel would return the FD here, but with the socket transport, we
     * have to return the index of the FD in the list of output FDs.
     * Coincidentally, we emit only one FD, so we still return 0. */
    return 0;
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
    default:
        return -ENOTTY;
    }
}
