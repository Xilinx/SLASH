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
#include "qdma_mock_sock.h"

#include <slash/uapi/slash_sysemu.h>

#include <printf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define RECV_BUFFER_SIZE (sizeof(struct slash_sysemu_socket_header) + 65536)
#define MAX_FDS 64

struct mock_sock_server_state {
    int server_fd;
};

void *mock_sock_listener_task(void *arg) {
    struct mock_sock_server_state *state = (struct mock_sock_server_state *)arg;
    void *recv_buffer = NULL;
    size_t cmsg_size = CMSG_SPACE(sizeof(int) * MAX_FDS);
    void *cmsg_buffer = NULL;
    struct iovec iov;
    struct msghdr msg;
    int n;

    recv_buffer = calloc(1, RECV_BUFFER_SIZE);
    if (recv_buffer == NULL) {
        goto cleanup;
    }

    cmsg_buffer = calloc(1, cmsg_size);
    if (cmsg_buffer == NULL) {
        goto cleanup;
    }

    memset(&iov, 0, sizeof(iov));
    iov.iov_base = recv_buffer;
    iov.iov_len = RECV_BUFFER_SIZE;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buffer;
    msg.msg_controllen = cmsg_size;

    while (1) {
        n = recvmsg(state->server_fd, &msg, 0);

        if (n > 0) {
            printf((char *)recv_buffer);
        } else if (n == 0) {
            printf("Orderly shutdown\n");
            goto cleanup;
        } else {
            printf("Error receiving a messagen\n");
            goto cleanup;
        }
    }

cleanup:
    free(cmsg_buffer);
    free(recv_buffer);
    close(state->server_fd);
    free(state);
    return NULL;
}

int slash_qdma_mock_sock_create(struct slash_qdma_mock_sock_server *server) {
    int rv;
    int sockets[2];
    struct mock_sock_server_state *state;

    rv = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets);
    if (rv != 0) {
        return -1;
    }

    state = calloc(1, sizeof(struct mock_sock_server_state));
    if (state == NULL) {
        close(sockets[0]);
        close(sockets[1]);
        errno = ENOMEM;
        return -1;
    }

    state->server_fd = sockets[1];

    rv = pthread_create(&server->listener_thread, NULL,
                        &mock_sock_listener_task, state);
    if (rv != 0) {
        close(sockets[0]);
        close(sockets[1]);
        free(state);
        errno = rv;
        return -1;
    }

    server->client_fd = sockets[0];
    return 0;
}

int slash_qdma_mock_sock_destroy(struct slash_qdma_mock_sock_server *server) {
    int rv;

    rv = close(server->client_fd);
    if (rv != 0) {
        /*
        Closing the socket failed,
        waiting for the server to finish doesn't make sense.
        */
        return -1;
    }

    rv = pthread_join(server->listener_thread, NULL);
    if (rv != 0) {
        errno = rv;
        return -1;
    }

    return 0;
}