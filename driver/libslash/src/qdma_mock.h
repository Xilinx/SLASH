/**
 * Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if
 * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * @file qdma_mock.h
 * @brief Mock QDMA implementation for testing without hardware.
 */

#ifndef LIBSLASH_QDMA_MOCK_H
#define LIBSLASH_QDMA_MOCK_H

#include <slash/qdma.h>
#include <slash/uapi/slash_interface.h>

#include <stdint.h>

struct slash_qdma *slash_qdma_mock_open(void);
int slash_qdma_mock_close(struct slash_qdma *qdma);
int slash_qdma_mock_info_read(struct slash_qdma *qdma, struct slash_qdma_info *info);
int slash_qdma_mock_qpair_add(struct slash_qdma *qdma, struct slash_qdma_qpair_add *req);
int slash_qdma_mock_qpair_start(struct slash_qdma *qdma, uint32_t qid);
int slash_qdma_mock_qpair_stop(struct slash_qdma *qdma, uint32_t qid);
int slash_qdma_mock_qpair_del(struct slash_qdma *qdma, uint32_t qid);
int slash_qdma_mock_qpair_get_fd(struct slash_qdma *qdma, uint32_t qid, int flags);

#endif /* LIBSLASH_QDMA_MOCK_H */
