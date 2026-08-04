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

#include "mock_sock.h"

#include <slash/uapi/slash_sysemu.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define RECV_BUFFER_SIZE (sizeof(struct slash_sysemu_socket_header) + 65536)
#define MAX_FDS 64
#define CMSG_SIZE (CMSG_SPACE(sizeof(int) * MAX_FDS))
#define MOCK_SOCK_SERVER_BOARD_BDF "0000:61:00"
#define MOCK_SOCK_SERVER_QDMA_BDF "0000:61:00.1"
#define MOCK_SOCK_SERVER_CTLDEV_BDF "0000:61:00.2"

#define MOCK_SOCK_MAX_QPAIRS 256

/**
 * @brief State specific to mock sock servers implementing the
 * /dev/slash_qdma_ctl endpoint.
 */
struct qdma_endpoint_state {
    /** @brief Qpair state bitsets.
     *
     * Bit 0 (0b01): Qpair used.
     * Bit 1 (0b10): Qpair started.
     */
    int qpairs[MOCK_SOCK_MAX_QPAIRS];
    /** @brief The configured aperture of the qpair.
     */
    int aperture_size[MOCK_SOCK_MAX_QPAIRS];
};

/**
 * @brief The state of a mock sock server.
 */
struct server_state {
    /**
     * @brief The socket FD to communicate with a client.
     */
    int server_fd;
    /**
     * @brief The kind of endpoint to implement.
     */
    enum slash_mock_sock_endpoint endpoint;
    /**
     * @brief Required state to mock-up the endpoint.
     */
    union {
        struct qdma_endpoint_state qdma;
    } endpoint_state;
};

/**
 * @brief Copy the the user's data into the destination, following the ABI
 * versioning protocol.
 *
 * The ABI versioning protocol demands that first of all, the user must provide
 * the size of the argument struct *as they know it* in the first field of the
 * IOCTL argument. If this size is equal to or above a certain minimally
 * supported size, the kernel/server must assume that fields that are not
 * included in this size are not known to the client and thus ignore them.
 *
 * In the case of the mock sock server, we get an additional hint in the form of
 * the received datagram size. This function therefore checks that:
 *
 * 1. The received message body is big enough to contain at least the size field
 * 2. The user's size is at least as big as the minimum argument size.
 * 3. The received message body is big enough to contain the alleged struct
 * size.
 *
 * If this is all the case, it computes the minimum of the struct size known to
 * the server and the user's struct size, zeroes the entire destination buffer,
 * and copies the corresponding number of bytes to the destination.
 *
 * @param dst The destination buffer to write to.
 * @param dst_size The size of the argument struct as it is known to the
 * server.
 * @param arg The argument buffer provided by the user.
 * @param arg_size The size of the argument buffer, in bytes.
 * @param min_size The minimum allowed argument struct size.
 * @return The common size (i.e. min(user_size, server_size)) on success, -1 on
 * failure.
 */
static __u32 checked_copy_from_user(void *dst, size_t dst_size, void *arg,
                                    size_t arg_size, size_t min_size) {
    __u32 user_size;
    __u32 common_size;

    /* Fetching the user's size field in the argument. */
    if (arg_size < sizeof(user_size)) {
        return -1;
    }
    user_size = *(__u32 *)arg;

    /* Checking that the user's size field is valid and the actually transferred
     * body size agrees to it. */
    if (user_size < min_size || user_size > arg_size) {
        return -1;
    }

    /* Deriving the common size that both we and the user can agree on. */
    common_size = (dst_size < user_size) ? dst_size : user_size;

    /* Finally, zero the destination and copy the data. */
    memset(dst, 0, dst_size);
    memcpy(dst, arg, common_size);

    /* Return the derived common size */
    return common_size;
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

    out_size = checked_copy_from_user(&info, sizeof(info), arg, arg_size,
                                      SLASH_QDMA_INFO_MIN_SIZE);
    if (out_size == -1) {
        return -EINVAL;
    }

    info.size = out_size;
    info.qsets_max = 0;
    info.msix_qvecs = 0;
    info.vf_max = 0;
    info.caps = 0;
    strcpy(info.bdf, MOCK_SOCK_SERVER_QDMA_BDF);
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
static int qdma_ioctl_qpair_add(struct qdma_endpoint_state *state, void *arg,
                                size_t arg_size) {
    struct slash_qdma_qpair_add qpair_add;
    __u32 out_size;
    __u32 qid;
    int rv;

    if (state == NULL || arg == NULL) {
        return -EINVAL;
    }

    out_size = checked_copy_from_user(&qpair_add, sizeof(qpair_add), arg,
                                      arg_size, SLASH_QDMA_QPAIR_ADD_MIN_SIZE);
    if (out_size < 0) {
        return -EINVAL;
    }

    for (qid = 0; qid < MOCK_SOCK_MAX_QPAIRS; qid++) {
        if (!(state->qpairs[qid] & 0b01)) {
            state->qpairs[qid] = 0b01;
            state->aperture_size[qid] = qpair_add.aperture_size;
            break;
        }
    }
    if (qid == MOCK_SOCK_MAX_QPAIRS) {
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
static int qdma_ioctl_qpair_op(struct qdma_endpoint_state *state, void *arg,
                               size_t arg_size) {
    struct slash_qdma_qpair_op qpair_op;
    __u32 out_size;

    if (state == NULL || arg == NULL) {
        return -EINVAL;
    }

    out_size = checked_copy_from_user(&qpair_op, sizeof(qpair_op), arg,
                                      arg_size, SLASH_QDMA_QPAIR_OP_MIN_SIZE);
    if (out_size == -1) {
        return -EINVAL;
    }

    if (qpair_op.qid >= MOCK_SOCK_MAX_QPAIRS ||
        !(state->qpairs[qpair_op.qid] & 0b01)) {
        return -ENOENT;
    }

    switch (qpair_op.op) {
    case SLASH_QDMA_QUEUE_OP_START:
        state->qpairs[qpair_op.qid] |= 0b10;
        return 0;
    case SLASH_QDMA_QUEUE_OP_STOP:
        state->qpairs[qpair_op.qid] &= ~0b10;
        return 0;
    case SLASH_QDMA_QUEUE_OP_DEL:
        state->qpairs[qpair_op.qid] = 0;
        return 0;
    default:
        return -ENOTTY;
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

    out_size = checked_copy_from_user(&buf_create, sizeof(buf_create), arg,
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
static int dispatch_qdma_ioctl(struct qdma_endpoint_state *state, int op,
                               void *arg, size_t arg_size, int *input_fds,
                               size_t n_input_fds, int **output_fds,
                               size_t *n_output_fds) {
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

/**
 * @brief Receive a message over a socket, potentially including file
 * descriptors.
 *
 * If the message includes a list of file descriptors, this function will
 * allocate a buffer for these file descriptors, assign it to *out_fds, and set
 * *out_n_fds accordingly. Ownership is transferred to the caller. If the
 * message include no file descriptors, *out_fds will be set to NULL and
 * *out_n_fds to 0.
 *
 * @param fd The file descriptor of the UNIX domain socket.
 * @param msg_buffer Non-owning reference to the message buffer.
 * @param msg_buffer_size The size/capacity of the @ref msg_buffer in bytes.
 * @param out_fds Non-owning reference to a pointer to store the file
 * descriptors at.
 * @param n_fds The number of file descriptors stored in *out_fds, or 0 if
 * *out_fds is NULL.
 * @return The number of bytes received if successfull, 0 if the other side has
 * closed their socket, or -1 in an error case with errno set.
 */
static ssize_t recv_sock_message(int fd, void *msg_buffer,
                                 size_t msg_buffer_size, int **out_fds,
                                 size_t *out_n_fds) {
    union {
        char buf[CMSG_SPACE(sizeof(int) * MAX_FDS)];
        struct cmsghdr align;
    } cmsg_buffer;
    struct iovec iov;
    struct msghdr msg;
    ssize_t msg_size;
    struct cmsghdr *cmsg;

    if (msg_buffer == NULL || msg_buffer_size == 0 || out_fds == NULL ||
        out_n_fds == NULL) {
        errno = EINVAL;
        return 0;
    }

    memset(&cmsg_buffer, 0, sizeof(cmsg_buffer));
    memset(&iov, 0, sizeof(iov));
    memset(&msg, 0, sizeof(msg));
    *out_fds = NULL;
    *out_n_fds = 0;

    iov.iov_base = msg_buffer;
    iov.iov_len = msg_buffer_size;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buffer.buf;
    msg.msg_controllen = sizeof(cmsg_buffer);

    msg_size = recvmsg(fd, &msg, 0);

    if (msg_size <= 0) {
        if (msg_size < 0) {
            errno = EIO;
            return -1;
        } else {
            return 0;
        }
    }

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        *out_n_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        *out_fds = malloc(*out_n_fds * sizeof(int));
        if (*out_fds == NULL) {
            errno = ENOMEM;
            *out_n_fds = 0;
            return -1;
        }
        memcpy(*out_fds, CMSG_DATA(cmsg), *out_n_fds * sizeof(int));
        break;
    }

    return msg_size;
}

/**
 * @brief Send a message over the socket, including some file descriptors.
 *
 * @param fd The file descriptor of the UNIX domain socket.
 * @param msg_buffer Non-owning reference to the message to send.
 * @param msg_buffer_size The size of the @ref msg_buffer in bytes.
 * @param fds Non-owning reference to the file descriptors to send.
 * @param n_fds The length of @ref fds, in number of file descriptors
 * @return The number of bytes sent if successfull, 0 if the other side has
 * closed their socket, or -1 in an error case with errno set.
 */
static ssize_t send_sock_message(int fd, void *msg_buffer,
                                 size_t msg_buffer_size, int *fds,
                                 size_t n_fds) {
    struct iovec iov;
    struct msghdr msg;
    void *cmsg_buffer;    /* Owned */
    struct cmsghdr *cmsg; /* Non-owned reference */
    ssize_t sent_size;

    memset(&iov, 0, sizeof(iov));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = msg_buffer;
    iov.iov_len = msg_buffer_size;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (n_fds > 0 && fds != NULL) {
        cmsg_buffer = calloc(1, CMSG_SPACE(n_fds * sizeof(int)));
        if (cmsg_buffer == NULL) {
            errno = ENOMEM;
            return -1;
        }
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(n_fds * sizeof(int));
        memcpy(CMSG_DATA(cmsg), fds, n_fds * sizeof(int));
        msg.msg_control = cmsg_buffer;
        msg.msg_controllen = cmsg->cmsg_len;
    } else {
        cmsg_buffer = NULL;
    }

    sent_size = sendmsg(fd, &msg, MSG_CMSG_CLOEXEC);
    free(cmsg_buffer);
    return sent_size;
}

/**
 * @brief Main function of the mock sock server
 *
 * The mock sock server listens on a UNIX domain socket and implements the
 * operations for the configured endpoint. It halts once the peer closes the
 * connection.
 *
 * @param arg Owning pointer to the server state. The server will free the state
 * on shutdown
 * @return NULL
 */
static void *mock_sock_server_main(void *arg) {
    struct server_state *state =
        (struct server_state *)arg; /* Owned reference */

    while (1) {
        char msg_buf[RECV_BUFFER_SIZE];
        ssize_t msg_size;

        int *input_fds;  /* Owned reference, may be NULL */
        int *output_fds; /* Owned reference, may be NULL */
        size_t n_input_fds, n_output_fds, i_fd;

        struct slash_sysemu_socket_header *msg_hdr; /* Non-owned reference */
        void *msg_arg;                              /* Non-owned refernce */
        size_t msg_arg_size;

        msg_size = recv_sock_message(state->server_fd, msg_buf, sizeof(msg_buf),
                                     &input_fds, &n_input_fds);

        if (msg_size <= 0) {
            /* Normal connection shutdown or receiving failure, shutting down */
            for (i_fd = 0; i_fd < n_input_fds; i_fd++) {
                close(input_fds[i_fd]);
            }
            free(input_fds);
            break;
        }

        if ((size_t)msg_size < sizeof(struct slash_sysemu_socket_header)) {
            /* Message too small, ignoring the message */
            for (i_fd = 0; i_fd < n_input_fds; i_fd++) {
                close(input_fds[i_fd]);
            }
            free(input_fds);
            continue;
        }

        msg_hdr = (struct slash_sysemu_socket_header *)msg_buf;
        msg_arg = msg_buf + sizeof(struct slash_sysemu_socket_header);
        msg_arg_size =
            ((size_t)msg_size) - sizeof(struct slash_sysemu_socket_header);

        if (state->endpoint == SLASH_MOCK_SOCK_ENDPOINT_QDMA) {
            msg_hdr->return_value = dispatch_qdma_ioctl(
                &(state->endpoint_state.qdma), msg_hdr->ioctl_op, msg_arg,
                msg_arg_size, input_fds, n_input_fds, &output_fds,
                &n_output_fds);
        } else {
            /* This endpoint is not implemented yet */
            msg_hdr->return_value = -ENOSYS;
            output_fds = NULL;
            n_output_fds = 0;
        }

        msg_size = send_sock_message(state->server_fd, msg_buf, msg_size,
                                     output_fds, n_output_fds);

        /* Closing the transmitted FDs, we don't need them anymore. */
        for (i_fd = 0; i_fd < n_input_fds; i_fd++) {
            close(input_fds[i_fd]);
        }
        free(input_fds);
        for (i_fd = 0; i_fd < n_output_fds; i_fd++) {
            close(output_fds[i_fd]);
        }
        free(output_fds);

        if (msg_size <= 0) {
            /* Sending failed, halting */
            break;
        }
    }

cleanup:
    close(state->server_fd);
    free(state);
    return NULL;
}

int slash_mock_sock_create(enum slash_mock_sock_endpoint endpoint) {
    int rv;
    int sockets[2];
    pthread_t thread;
    struct server_state *state;

    /* TODO: Implement other kinds of mock socks */
    if (endpoint != SLASH_MOCK_SOCK_ENDPOINT_QDMA) {
        errno = ENOTSUP;
        return -1;
    }

    state = calloc(1, sizeof(struct server_state));
    if (state == NULL) {
        errno = ENOMEM;
        return -1;
    }

    rv = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets);
    if (rv == -1) {
        free(state);
        return -1;
    }

    state->server_fd = sockets[1];
    state->endpoint = endpoint;

    rv = pthread_create(&thread, NULL, &mock_sock_server_main, state);
    if (rv != 0) {
        close(sockets[0]);
        close(sockets[1]);
        free(state);
        errno = rv;
        return -1;
    }

    return sockets[0];
}
