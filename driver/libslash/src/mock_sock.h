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
    SLASH_MOCK_SOCK_ENDPOINT_HOTPLUG,
    /* Endpoints that can not be directly created by users */
    SLASH_MOCK_SOCK_ENDPOINT_QPAIR,
};

/**
 * @brief Create a mock-up for a SLASH endpoint, using the socket transport.
 *
 * This function launches a background thread that supports most of the
 * functionality needed to test libSLASH users. TODO: Describe which
 * functionality is mocked and which is not.
 *
 * Possible errors:
 * * ENOTSUP: Unsupported endpoint endpoint
 * * ENOMEM: No memory available
 * As well as all errors from @ref socketpair and @ref pthread_create.
 *
 * @param endpoint The kind of endpoint to mock-up
 * @returns 0 on success, -1 on failure with errno set.
 */
int slash_mock_sock_create(enum slash_mock_sock_endpoint endpoint);

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
