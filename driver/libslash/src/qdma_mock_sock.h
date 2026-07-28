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
#ifndef LIBSLASH_QDMA_MOCK_SOCK_H
#define LIBSLASH_QDMA_MOCK_SOCK_H

#include <pthread.h>

#include <slash/qdma.h>
#include <slash/uapi/slash_interface.h>

struct slash_qdma_mock_sock_server {
    pthread_t listener_thread;
    int client_fd;
};

int slash_qdma_mock_sock_create(struct slash_qdma_mock_sock_server *server);
int slash_qdma_mock_sock_destroy(struct slash_qdma_mock_sock_server *server);

#endif
