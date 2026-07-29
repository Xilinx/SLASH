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

#include <pthread.h>

#include <slash/qdma.h>
#include <slash/uapi/slash_interface.h>

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

#endif /* LIBSLASH_MOCK_SOCK_H */
