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
#include "mock_sock.h"

#include <slash/uapi/slash_sysemu.h>

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

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
struct slash_mock_sock_qdma_endpoint_state {
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
struct slash_mock_sock_server_state {
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
        struct slash_mock_sock_qdma_endpoint_state qdma;
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
__u32 checked_copy_from_user(void *dst, size_t dst_size, void *arg,
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
int slash_mock_sock_qdma_ioctl_info(void *arg, size_t arg_size) {
    struct slash_qdma_info info;
    __u32 out_size;

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
int slash_mock_sock_qdma_ioctl_qpair_add(
    struct slash_mock_sock_qdma_endpoint_state *state, void *arg,
    size_t arg_size) {
    struct slash_qdma_qpair_add qpair_add;
    __u32 out_size;
    __u32 qid;
    int rv;

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
int slash_mock_sock_qdma_ioctl_qpair_op(
    struct slash_mock_sock_qdma_endpoint_state *state, void *arg,
    size_t arg_size) {
    struct slash_qdma_qpair_op qpair_op;
    __u32 out_size;

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
 * @brief Dispatch the IOCTL operation in the QDMA control file endpoint.
 *
 * @param state Non-owning reference to the QDMA endpoint state.
 * @param op Op-code of the IOCTL to dispatch.
 * @param arg Non-owning reference to the argument struct.
 * @param arg_size Apparent/maximal size of the argument struct.
 */
int slash_mock_sock_dispatch_qdma_ioctl(
    struct slash_mock_sock_qdma_endpoint_state *state, int op, void *arg,
    size_t arg_size) {
    switch (op) {
    case SLASH_QDMA_IOCTL_INFO:
        return slash_mock_sock_qdma_ioctl_info(arg, arg_size);
    case SLASH_QDMA_IOCTL_QPAIR_ADD:
        return slash_mock_sock_qdma_ioctl_qpair_add(state, arg, arg_size);
    case SLASH_QDMA_IOCTL_Q_OP:
        return slash_mock_sock_qdma_ioctl_qpair_op(state, arg, arg_size);
    default:
        return -ENOTTY;
    }
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
void *mock_sock_server_main(void *arg) {
    struct slash_mock_sock_server_state *server_state;

    char recv_buffer[RECV_BUFFER_SIZE];
    char cmsg_buffer[CMSG_SPACE(sizeof(int) * MAX_FDS)];
    struct iovec iov;
    struct msghdr msg;
    ssize_t n;

    struct slash_sysemu_socket_header *message_header;
    int ioctl_op;
    void *message_body;
    size_t message_body_size;

    server_state = (struct slash_mock_sock_server_state *)arg;

    while (1) {
        memset(&iov, 0, sizeof(iov));
        memset(recv_buffer, 0, sizeof(recv_buffer));
        iov.iov_base = recv_buffer;
        iov.iov_len = sizeof(recv_buffer);

        memset(&msg, 0, sizeof(msg));
        memset(cmsg_buffer, 0, sizeof(cmsg_buffer));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buffer;
        msg.msg_controllen = sizeof(cmsg_buffer);

        n = recvmsg(server_state->server_fd, &msg, 0);

        if (n <= 0) {
            /* Receive failed or connection is closed, halting */
            break;
        }

        if ((size_t)n < sizeof(struct slash_sysemu_socket_header)) {
            /* Message too small, ignoring */
            continue;
        }

        message_header = (struct slash_sysemu_socket_header *)recv_buffer;
        ioctl_op = message_header->ioctl_op;
        message_body = recv_buffer + sizeof(struct slash_sysemu_socket_header);
        message_body_size =
            ((size_t)n) - sizeof(struct slash_sysemu_socket_header);

        if (server_state->endpoint == SLASH_MOCK_SOCK_ENDPOINT_QDMA) {
            message_header->return_value = slash_mock_sock_dispatch_qdma_ioctl(
                &(server_state->endpoint_state.qdma), ioctl_op, message_body,
                message_body_size);
        } else {
            /* This endpoint is not implemented yet */
            message_header->return_value = -ENOSYS;
        }

        n = sendmsg(server_state->server_fd, &msg, MSG_CMSG_CLOEXEC);
        if (n <= 0) {
            /* Sending failed, halting */
            break;
        }
    }

cleanup:
    close(server_state->server_fd);
    free(server_state);
    return NULL;
}

int slash_mock_sock_create(enum slash_mock_sock_endpoint endpoint) {
    int rv;
    int sockets[2];
    pthread_t thread;
    struct slash_mock_sock_server_state *state;

    /* TODO: Implement other kinds of mock socks */
    if (endpoint != SLASH_MOCK_SOCK_ENDPOINT_QDMA) {
        errno = ENOTSUP;
        return -1;
    }

    state = calloc(1, sizeof(struct slash_mock_sock_server_state));
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
