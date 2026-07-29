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

/**
 * @brief The state of a mock sock server.
 */
struct mock_sock_server_state {
    /**
     * @brief The socket FD to communicate with a client.
     */
    int server_fd;
    /**
     * @brief The kind of endpoint to implement.
     */
    enum slash_mock_sock_endpoint endpoint;
};

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
    __u32 user_size;
    __u32 out_size = sizeof(struct slash_qdma_info);

    if (arg_size < sizeof(__u32)) {
        return -EINVAL;
    }
    user_size = *(__u32 *)arg;
    if (arg_size < (size_t)user_size) {
        return -EINVAL;
    }
    if (user_size < out_size) {
        out_size = user_size;
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

int slash_mock_sock_dispatch_ioctl(int op, void *arg, size_t arg_size) {
    switch (op) {
    case SLASH_QDMA_IOCTL_INFO:
        return slash_mock_sock_qdma_ioctl_info(arg, arg_size);
    default:
        return -EINVAL;
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
    struct mock_sock_server_state *state = (struct mock_sock_server_state *)arg;
    char recv_buffer[RECV_BUFFER_SIZE];
    char cmsg_buffer[CMSG_SPACE(sizeof(int) * MAX_FDS)];
    struct iovec iov;
    struct msghdr msg;
    ssize_t n;

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

        n = recvmsg(state->server_fd, &msg, 0);

        if (n <= 0) {
            break;
        }

        if ((size_t)n < sizeof(struct slash_sysemu_socket_header)) {
            /* Message too small */
            continue;
        }

        struct slash_sysemu_socket_header *header =
            (struct slash_sysemu_socket_header *)recv_buffer;

        header->return_value = slash_mock_sock_dispatch_ioctl(
            header->ioctl_op,
            recv_buffer + sizeof(struct slash_sysemu_socket_header),
            ((size_t)n) - sizeof(struct slash_sysemu_socket_header));

        sendmsg(state->server_fd, &msg, MSG_CMSG_CLOEXEC);
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
    struct mock_sock_server_state *state;

    /* TODO: Implement other kinds of mock socks */
    if (endpoint != SLASH_MOCK_SOCK_ENDPOINT_QDMA) {
        errno = ENOTSUP;
        return -1;
    }

    state = calloc(1, sizeof(struct mock_sock_server_state));
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
