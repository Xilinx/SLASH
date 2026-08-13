/*
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file sock_transport.c
 *
 * Client-side AF_UNIX/SOCK_SEQPACKET transport for libslash.
 *
 * Wire protocol (mirrors the daemon's transport.cpp):
 *   datagram = slash_sysemu_socket_header (16 bytes)
 *            + ioctl-argument struct bytes (0..SLASH_SOCK_MAX_PAYLOAD_BYTES)
 *   fds transferred as SCM_RIGHTS ancillary data in the same sendmsg/recvmsg.
 *
 * C90 conformance:
 *   - declarations at top of block, before any statement
 *   - slash-slash comments are NOT used
 *   - _GNU_SOURCE is defined before any system header for SOCK_CLOEXEC,
 *     MSG_NOSIGNAL, MSG_CMSG_CLOEXEC, etc.
 */

#define _GNU_SOURCE

#include "sock_transport.h"

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * Size of the cmsg buffer large enough to hold SLASH_SOCK_MAX_FDS_PER_MSG fds.
 * CMSG_SPACE accounts for the cmsghdr and required alignment padding.
 */
#define CMSG_BUF_SIZE (CMSG_SPACE(sizeof(int) * SLASH_SOCK_MAX_FDS_PER_MSG))

/*
 * Total receive-buffer size: one header + maximum payload.
 */
#define RECV_BUF_SIZE (sizeof(struct slash_sysemu_socket_header) + SLASH_SOCK_MAX_PAYLOAD_BYTES)

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * close_fd_array - close every fd in an array, ignoring errors.
 * Used to ensure no fd leak on error paths after recvmsg.
 */
static void close_fd_array(int *fds, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
}


/* -------------------------------------------------------------------------
 * slash_sock_connect
 * ---------------------------------------------------------------------- */

int slash_sock_connect(const char *path)
{
    int fd;
    struct sockaddr_un addr;
    struct timeval tv;
    size_t path_len;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    path_len = strlen(path);
    if (path_len == 0 || path_len >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    /* Set ~10 s send and receive timeouts so a dead daemon cannot hang. */
    memset(&tv, 0, sizeof(tv));
    tv.tv_sec = 10;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1); /* include NUL */

    if (connect(fd, (struct sockaddr *)&addr,
                (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* -------------------------------------------------------------------------
 * slash_path_is_socket
 * ---------------------------------------------------------------------- */

int slash_path_is_socket(const char *path)
{
    struct stat st;

    if (stat(path, &st) < 0) {
        return -1;
    }
    return S_ISSOCK(st.st_mode) ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * slash_fd_is_socket
 * ---------------------------------------------------------------------- */

int slash_fd_is_socket(int fd)
{
    struct stat st;

    if (fstat(fd, &st) < 0) {
        return -1;
    }
    return S_ISSOCK(st.st_mode) ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * slash_sock_request
 * ---------------------------------------------------------------------- */

int32_t slash_sock_request(int fd,
                           uint32_t ioctl_op,
                           void *arg,
                           size_t arg_len,
                           const int *send_fds,
                           size_t n_send_fds,
                           int *recv_fds,
                           size_t recv_fd_cap,
                           size_t *n_recv_fds,
                           uint32_t *seq)
{
    struct slash_sysemu_socket_header req_hdr;
    struct slash_sysemu_socket_header resp_hdr;

    struct iovec send_iov[2];
    struct msghdr send_msg;
    char send_cmsg_buf[CMSG_BUF_SIZE];
    struct cmsghdr *cmsg;
    ssize_t sent;

    char recv_data_buf[RECV_BUF_SIZE];
    struct iovec recv_iov;
    struct msghdr recv_msg;
    char recv_cmsg_buf[CMSG_BUF_SIZE];
    ssize_t n;
    size_t payload_len;
    size_t n_fds_received;
    int recv_fd_tmp[SLASH_SOCK_MAX_FDS_PER_MSG];
    size_t i;
    size_t fd_bytes;
    const int *fd_ptr;
    size_t fd_count;

    /* Initialise outputs. */
    if (n_recv_fds != NULL) {
        *n_recv_fds = 0;
    }

    /* Basic argument validation. */
    if (arg_len > SLASH_SOCK_MAX_PAYLOAD_BYTES) {
        return -EINVAL;
    }
    if (n_send_fds > SLASH_SOCK_MAX_FDS_PER_MSG) {
        return -EINVAL;
    }
    if (seq == NULL) {
        return -EINVAL;
    }

    /* -----------------------------------------------------------------
     * Build and send the request datagram.
     * ----------------------------------------------------------------- */

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.ioctl_op    = ioctl_op;
    req_hdr.sequence_id = *seq;
    req_hdr.return_value = 0;
    req_hdr.pad          = 0;

    memset(send_iov, 0, sizeof(send_iov));
    send_iov[0].iov_base = &req_hdr;
    send_iov[0].iov_len  = sizeof(req_hdr);
    send_iov[1].iov_base = arg;           /* may be NULL when arg_len == 0 */
    send_iov[1].iov_len  = arg_len;

    memset(&send_msg, 0, sizeof(send_msg));
    send_msg.msg_iov    = send_iov;
    send_msg.msg_iovlen = (arg_len > 0) ? 2 : 1;

    if (n_send_fds > 0 && send_fds != NULL) {
        fd_bytes = sizeof(int) * n_send_fds;
        memset(send_cmsg_buf, 0, sizeof(send_cmsg_buf));
        send_msg.msg_control    = send_cmsg_buf;
        send_msg.msg_controllen = (socklen_t)CMSG_SPACE(fd_bytes);

        cmsg             = CMSG_FIRSTHDR(&send_msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = (socklen_t)CMSG_LEN(fd_bytes);
        memcpy(CMSG_DATA(cmsg), send_fds, fd_bytes);
    }

    sent = sendmsg(fd, &send_msg, MSG_NOSIGNAL);
    if (sent < 0) {
        return -ENODEV;
    }

    /* -----------------------------------------------------------------
     * Receive the response datagram.
     * ----------------------------------------------------------------- */

    memset(recv_fd_tmp, -1, sizeof(recv_fd_tmp));
    n_fds_received = 0;

    memset(&recv_iov, 0, sizeof(recv_iov));
    recv_iov.iov_base = recv_data_buf;
    recv_iov.iov_len  = sizeof(recv_data_buf);

    memset(recv_cmsg_buf, 0, sizeof(recv_cmsg_buf));

    memset(&recv_msg, 0, sizeof(recv_msg));
    recv_msg.msg_iov        = &recv_iov;
    recv_msg.msg_iovlen     = 1;
    recv_msg.msg_control    = recv_cmsg_buf;
    recv_msg.msg_controllen = (socklen_t)sizeof(recv_cmsg_buf);

    n = recvmsg(fd, &recv_msg, MSG_CMSG_CLOEXEC);

    /* Extract any received FDs from the control message BEFORE checking errors
     * so we can close them cleanly on any subsequent failure path. */
    for (cmsg = CMSG_FIRSTHDR(&recv_msg);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(&recv_msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        fd_bytes  = cmsg->cmsg_len - CMSG_LEN(0);
        fd_count  = fd_bytes / sizeof(int);
        fd_ptr    = (const int *)CMSG_DATA(cmsg);
        for (i = 0; i < fd_count && n_fds_received < SLASH_SOCK_MAX_FDS_PER_MSG; ++i) {
            recv_fd_tmp[n_fds_received++] = fd_ptr[i];
        }
        /* Close any fds beyond our cap (shouldn't happen, but be safe). */
        for (; i < fd_count; ++i) {
            close(fd_ptr[i]);
        }
    }

    /* Now check recv errors: close any received fds before returning. */
    if (n < 0) {
        close_fd_array(recv_fd_tmp, n_fds_received);
        return -ENODEV;
    }
    if (n == 0) {
        /* Peer closed. */
        close_fd_array(recv_fd_tmp, n_fds_received);
        return -ENODEV;
    }
    if (recv_msg.msg_flags & MSG_TRUNC) {
        close_fd_array(recv_fd_tmp, n_fds_received);
        return -ENODEV;
    }
    if (recv_msg.msg_flags & MSG_CTRUNC) {
        close_fd_array(recv_fd_tmp, n_fds_received);
        return -ENODEV;
    }
    if ((size_t)n < sizeof(struct slash_sysemu_socket_header)) {
        close_fd_array(recv_fd_tmp, n_fds_received);
        return -ENODEV;
    }

    /* Deserialise the response header. */
    memcpy(&resp_hdr, recv_data_buf, sizeof(resp_hdr));

    /* Validate sequence_id and ioctl_op match the request. */
    if (resp_hdr.sequence_id != req_hdr.sequence_id ||
        resp_hdr.ioctl_op    != req_hdr.ioctl_op) {
        close_fd_array(recv_fd_tmp, n_fds_received);
        return -ENODEV;
    }

    /* Copy the response arg bytes back into the caller's buffer. */
    payload_len = (size_t)n - sizeof(struct slash_sysemu_socket_header);
    if (arg != NULL && payload_len > 0) {
        size_t copy_len = (payload_len < arg_len) ? payload_len : arg_len;
        memcpy(arg, recv_data_buf + sizeof(struct slash_sysemu_socket_header), copy_len);
    }

    /* Hand received FDs to the caller, closing any that overflow the cap. */
    if (recv_fds != NULL && n_recv_fds != NULL) {
        size_t copy_count = (n_fds_received < recv_fd_cap) ? n_fds_received : recv_fd_cap;
        for (i = 0; i < copy_count; ++i) {
            recv_fds[i] = recv_fd_tmp[i];
            recv_fd_tmp[i] = -1; /* ownership transferred */
        }
        *n_recv_fds = copy_count;
    }
    /* Close any remaining unreferenced received fds. */
    close_fd_array(recv_fd_tmp, n_fds_received);

    /* Advance the sequence counter. */
    (*seq)++;

    /* return_value carries a signed errno encoded as uint32 two's-complement;
     * cast to int32_t to recover the sign. */
    return (int32_t)resp_hdr.return_value;
}

/* -------------------------------------------------------------------------
 * slash_sock_rewrite_fd_index
 * ---------------------------------------------------------------------- */

int slash_sock_rewrite_fd_index(int *fd_list,
                                size_t *fd_count,
                                int *field,
                                int fd)
{
    if (*fd_count >= SLASH_SOCK_MAX_FDS_PER_MSG) {
        errno = EMSGSIZE;
        return -1;
    }

    fd_list[*fd_count] = fd;
    *field = (int)(*fd_count);
    (*fd_count)++;

    return 0;
}
