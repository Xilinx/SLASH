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
 * @file sock_transport.h
 *
 * Private C90 helper: client-side AF_UNIX/SOCK_SEQPACKET transport.
 *
 * This is the libslash mirror of the slash_sysemu daemon's transport.cpp.
 * It speaks the same wire protocol: every datagram begins with a
 * slash_sysemu_socket_header (16 bytes) followed immediately by the IOCTL
 * argument struct bytes.  File descriptors are passed via SCM_RIGHTS as
 * ancillary data; struct fields that hold fds store their index into the
 * ancillary list instead of the raw fd.
 *
 * All functions follow POSIX error conventions: -1 / failure return on error
 * with errno set.  On any transport-layer failure (send/recv error, peer
 * close, truncation, sequence or op mismatch) errno is set to ENODEV and
 * SLASH_SOCK_TRANSPORT_ERR is returned so callers can map to -ENODEV.
 *
 * This header is private to libslash; do not install it.
 */

#ifndef SLASH_SOCK_TRANSPORT_H
#define SLASH_SOCK_TRANSPORT_H

#include <slash/uapi/slash_sysemu.h>

#include <stddef.h>
#include <stdint.h>

/*
 * Maximum payload bytes (excluding the 16-byte header) accepted per datagram.
 * Mirrors kMaxPayloadBytes in the daemon's transport.h.
 */
#define SLASH_SOCK_MAX_PAYLOAD_BYTES ((size_t)65536)

/*
 * Maximum number of FDs that may be sent or received in one datagram.
 * Mirrors kMaxFdsPerMessage in the daemon's transport.h.
 */
#define SLASH_SOCK_MAX_FDS_PER_MSG ((size_t)64)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * slash_sock_connect - connect a SEQPACKET socket to the daemon.
 *
 * Creates an AF_UNIX SOCK_SEQPACKET socket, sets SO_RCVTIMEO and SO_SNDTIMEO
 * to ~10 s so a dead daemon cannot hang the caller indefinitely, then calls
 * connect(2).
 *
 * @param path  Absolute path of the daemon socket.
 * @return      New connected fd (>= 0), or -1 with errno set on failure.
 */
int slash_sock_connect(const char *path);

/**
 * slash_path_is_socket - test whether a filesystem path is a socket.
 *
 * Uses stat(2) and S_ISSOCK.
 *
 * @param path  Path to test.
 * @return      1 if path is a socket, 0 if it is not, -1 on stat(2) error
 *              (errno preserved).
 */
int slash_path_is_socket(const char *path);

/**
 * slash_fd_is_socket - test whether an open fd refers to a socket.
 *
 * Uses fstat(2) and S_ISSOCK.
 *
 * @param fd  Open file descriptor to test.
 * @return    1 if fd is a socket, 0 if it is not, -1 on fstat(2) error
 *            (errno preserved).
 */
int slash_fd_is_socket(int fd);

/**
 * Execute a control operation over a UNIX domain socket.
 *
 * Builds a datagram from the header and @arg bytes, sends it with one
 * sendmsg(2) (SCM_RIGHTS for @send_fds, MSG_NOSIGNAL), then receives the
 * response with one recvmsg(2) (MSG_CMSG_CLOEXEC).
 *
 * Validation after recv:
 *   - MSG_TRUNC or MSG_CTRUNC set => transport failure
 *   - received size < sizeof(header) => transport failure
 *   - response sequence_id != request sequence_id => transport failure
 *   - response ioctl_op    != request ioctl_op    => transport failure
 *
 * On transport success, the response arg bytes are copied back into @arg, any
 * received FDs are stored in @recv_fds[0..n-1] with *n_recv_fds set, and the
 * return value from the daemon is returned.
 *
 * If @ref arg_len, @ref n_send_fds, or @ref seq is invalid, -EINVAL is returned.
 * On ANY transport or protocol failure -ENODEV is returend.  Any FDs
 * received before the error was detected are closed; no fd leaks.
 *
 * @param fd           Connected SEQPACKET socket.
 * @param ioctl_op     IOCTL command number for this request.
 * @param arg          In/out pointer to the IOCTL argument struct bytes.
 * @param arg_len      Size of the arg struct in bytes (may be 0).
 * @param send_fds     Array of fds to send as SCM_RIGHTS (may be NULL).
 * @param n_send_fds   Number of entries in @send_fds.
 * @param recv_fds     Caller-supplied array to receive incoming fds (may be
 *                     NULL if recv_fd_cap == 0).
 * @param recv_fd_cap  Capacity of @recv_fds.
 * @param n_recv_fds   Out: number of fds written into @recv_fds.
 * @param seq          In/out sequence counter.  *seq is used as the request
 *                     sequence_id and incremented on success.
 * @return             Daemon's header.return_value cast to int32_t, may be a
 *                     negative errno if the daemon or this wrapper have
 *                     encountered an error.
 */
int32_t slash_sock_request(int fd, uint32_t ioctl_op, void *arg, size_t arg_len,
                           const int *send_fds, size_t n_send_fds,
                           int *recv_fds, size_t recv_fd_cap,
                           size_t *n_recv_fds, uint32_t *seq);

/**
 * slash_sock_rewrite_fd_index - client side of collect_fds_and_rewrite.
 *
 * Appends @fd to the fd accumulator array @fd_list[@fd_count] and overwrites
 * @field with the index @fd will occupy in the SCM_RIGHTS ancillary data.
 * *fd_count is incremented on success.
 *
 * Enforces SLASH_SOCK_MAX_FDS_PER_MSG: if *fd_count already equals the cap,
 * no modification is made, errno is set to EMSGSIZE, and -1 is returned.
 *
 * @param fd_list   Accumulator array of raw fds (caller provides storage of
 *                  at least SLASH_SOCK_MAX_FDS_PER_MSG entries).
 * @param fd_count  In/out: current number of entries in fd_list.
 * @param field     Pointer to the struct field to overwrite with the index.
 * @param fd        The actual fd to append.
 * @return          0 on success, -1 with errno=EMSGSIZE if the cap is reached.
 */
int slash_sock_rewrite_fd_index(int *fd_list, size_t *fd_count, int *field,
                                int fd);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SLASH_SOCK_TRANSPORT_H */
