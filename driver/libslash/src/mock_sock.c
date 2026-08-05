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
#include "mock_sock_qdma.h"

#include <slash/uapi/slash_sysemu.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RECV_BUFFER_SIZE (sizeof(struct slash_sysemu_socket_header) + 65536)
#define MAX_FDS 64
#define CMSG_SIZE (CMSG_SPACE(sizeof(int) * MAX_FDS))

__u32 slash_checked_copy_from_user(void *dst, size_t dst_size, void *arg,
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
        msg.msg_control = cmsg_buffer;
        msg.msg_controllen = CMSG_LEN(n_fds * sizeof(int));
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = msg.msg_controllen;
        memcpy(CMSG_DATA(cmsg), fds, n_fds * sizeof(int));
    } else {
        cmsg_buffer = NULL;
    }

    sent_size = sendmsg(fd, &msg, MSG_CMSG_CLOEXEC);
    free(cmsg_buffer);
    return sent_size;
}

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
    void *endpoint_state;
};

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

        switch (state->endpoint) {
        case SLASH_MOCK_SOCK_ENDPOINT_QDMA:
            msg_hdr->return_value = slash_mock_sock_qdma_dispatch(
                state->endpoint_state, msg_hdr->ioctl_op, msg_arg, msg_arg_size,
                input_fds, n_input_fds, &output_fds, &n_output_fds);
            break;

        case SLASH_MOCK_SOCK_ENDPOINT_QPAIR:
            msg_hdr->return_value = slash_mock_sock_qpair_dispatch(
                state->endpoint_state, msg_hdr->ioctl_op, msg_arg, msg_arg_size,
                input_fds, n_input_fds, &output_fds, &n_output_fds);
            break;

        default:
            /* This endpoint is not implemented yet */
            msg_hdr->return_value = -ENOSYS;
            output_fds = NULL;
            n_output_fds = 0;
            break;
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
    switch (state->endpoint) {
    case SLASH_MOCK_SOCK_ENDPOINT_QDMA:
        slash_mock_sock_qdma_put_state(state->endpoint_state);
        break;
    case SLASH_MOCK_SOCK_ENDPOINT_QPAIR:
        slash_mock_sock_qpair_release_state(state->endpoint_state);
        free(state->endpoint_state);
    default:
    }
    free(state);
    return NULL;
}

int slash_mock_sock_create(enum slash_mock_sock_endpoint endpoint) {
    int rv, creation_errno;
    void *state;

    switch (endpoint) {
    case SLASH_MOCK_SOCK_ENDPOINT_QDMA:
        state = malloc(sizeof(struct slash_mock_sock_qdma_state));
        if (state == NULL) {
            errno = ENOMEM;
            return -1;
        }
        slash_mock_sock_qdma_init_state(state);
        break;
    default:
        /* TODO: Implement other kinds of mock socks */
        errno = ENOTSUP;
        return -1;
    }

    rv = slash_mock_sock_create_with_state(endpoint, state);
    if (rv >= 0) {
        return rv;
    }
    creation_errno = errno;

    /* Endpoint creation failed, clean up the state. */
    switch (endpoint) {
    case SLASH_MOCK_SOCK_ENDPOINT_QDMA:
        slash_mock_sock_qdma_put_state(state);
        errno = creation_errno;
        return -1;
    default:
        errno = ENOTSUP;
        return -1;
    }
}

int slash_mock_sock_create_with_state(enum slash_mock_sock_endpoint endpoint,
                                      void *endpoint_state) {
    int rv;
    struct server_state *state;
    int sockets[2];
    pthread_t thread;

    state = calloc(1, sizeof(struct server_state));
    if (state == NULL) {
        errno = ENOMEM;
        return -1;
    }

    rv = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets);
    if (rv == -1) {
        free(state);
        /* Returning the error from socketpair */
        return -1;
    }
    state->server_fd = sockets[1];
    state->endpoint = endpoint;
    state->endpoint_state = endpoint_state;

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
