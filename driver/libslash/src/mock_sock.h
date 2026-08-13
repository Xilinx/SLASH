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
#ifndef LIBSLASH_MOCK_SOCK_H
#define LIBSLASH_MOCK_SOCK_H

#include <slash/uapi/slash_interface.h>

#define LIBSLASH_MOCK_SOCK_BOARD_BDF "0000:61:00"
#define LIBSLASH_MOCK_SOCK_QDMA_BDF "0000:61:00.1"
#define LIBSLASH_MOCK_SOCK_CTLDEV_BDF "0000:61:00.2"

enum slash_mock_sock_endpoint {
    SLASH_MOCK_SOCK_ENDPOINT_QDMA,
    SLASH_MOCK_SOCK_ENDPOINT_CTLDEV,
    /* Endpoints that can not be directly created by users */
    SLASH_MOCK_SOCK_ENDPOINT_QPAIR,
};

/**
 * @brief Create a mock-up for a SLASH endpoint, using the socket transport.
 *
 * This function launches a background thread that supports most of the
 * functionality needed to test libSLASH users. User can thus mock-up a
 * connection to the QDMA, ctldev (BAR), and hotplugging subsystems, without
 * needing a running slash kernel module or the sysemu daemon.
 *
 * The only argument to this function is the endpoint type. However, out of the
 * offered endpoints, only @ref SLASH_MOCK_SOCK_ENDPOINT_QDMA, @ref
 * SLASH_MOCK_SOCK_ENDPOINT_CTLDEV, and @ref SLASH_MOCK_SOCK_ENDPOINT_HOTPLUG
 * are supported, since the other endpoint types are sub-endpoints that depend
 * on one of the previous endpoints.
 *
 * If successful, the return value of this function is a file descriptor to a
 * UNIX domain socket. Requests sent to this socket are serviced by a background
 * thread. This background thread stays alive until either an error occurrs or
 * the returned file descriptor is closed. Closing the returned file descriptor
 * is therefore enough to clean up the mock sock endpoint.
 *
 * Possible errors:
 * * ENOMEM: Not enough memory.
 * * ENOTSUP: The requested endpoint is not supported by this function.
 * * All errors from `socketpair` and `pthread_create`.
 *
 * @param endpoint The kind of endpoint to mock-up
 * @returns A file descriptor (>=0) on success, -1 on failure with errno set.
 */
int slash_mock_sock_create(enum slash_mock_sock_endpoint endpoint);

/**
 * @brief Create a mock-up for a SLASH endpoint, using the socket transport and
 * the referenced state object.
 *
 * Most users of the mock-sock system should use @ref slash_mock_sock_create,
 * which also creates the runtime state for the endpoint in question. However,
 * since this function does not need to create the state object itself, it can
 * launch all endpoint types defined in @ref slash_mock_sock_endpoint.
 *
 * If successful, the return value of this function is a file descriptor to a
 * UNIX domain socket. Requests sent to this socket are serviced by a background
 * thread. This background thread stays alive until either an error occurrs or
 * the returned file descriptor is closed. Closing the returned file descriptor
 * is therefore enough to clean up the mock sock endpoint.
 *
 * The background thread will take (co-)ownership of the state struct if it is
 * launched successfully. Please take the required types and ownership models
 * from the following table. However, if this function fails, the (co-)ownership
 * remains with the caller, who then has to clean up the state struct themselves.
 *
 * | Endpoint                       | State type                  | Ownership |
 * |--------------------------------|-----------------------------|-----------|
 * | SLASH_MOCK_SOCK_ENDPOINT_QDMA  | slash_mock_sock_qdma_state  | Co-owned  |
 * | SLASH_MOCK_SOCK_ENDPOINT_QPAIR | slash_mock_sock_qpair_state | Owned     |
 *
 * Possible errors:
 * * ENOMEM: Not enough memory available.
 * * All errors from `socketpair` and `pthread_create`
 *
 * @param endpoint The kind of endpoint to mock-up
 * @param endpoint_state Owned pointer to the necessary context for BAR and
 * QPAIR endpoints, or NULL for the QDMA, CTLDEV, and HOTPLUG endpoints.
 * @returns 0 on success, -1 on failure with errno set.
 */
int slash_mock_sock_create_with_state(enum slash_mock_sock_endpoint endpoint,
                                      void *endpoint_state);

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
__u32 slash_checked_copy_from_user(void *dst, size_t dst_size, void *arg,
                                   size_t arg_size, size_t min_size);

#endif /* LIBSLASH_MOCK_SOCK_H */
