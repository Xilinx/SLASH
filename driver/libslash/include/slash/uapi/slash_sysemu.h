/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/**
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * This file is dual-licensed: you may select either the GNU General Public
 * License version 2 (GPL-2.0-only) or the MIT License.  See the LICENSE
 * files in the repository root for the full text of each license.
 */

/**
 * @file slash_sysemu.h
 *
 * Shared ABI for the slash system-emulation daemon socket protocol.
 *
 * This header defines the wire structures and constants used by both the
 * slash_sysemu daemon and libslash when communicating over AF_UNIX
 * SOCK_SEQPACKET sockets (the emulated equivalents of the slash character
 * devices).
 *
 * Every datagram — both request (client -> daemon) and response
 * (daemon -> client) — begins with a fixed-size struct
 * slash_sysemu_socket_header, followed immediately by the IOCTL argument
 * struct bytes.
 *
 * This header is designed to be included from both C (C90 and later) and
 * C++ translation units.
 */

#ifndef SLASH_SYSEMU_UAPI_H
#define SLASH_SYSEMU_UAPI_H

#include <linux/types.h>

/**
 * @brief Wire header for every datagram exchanged over the emulation sockets.
 *
 * Both request (client->daemon) and response (daemon->client) datagrams begin
 * with this fixed-size header, followed immediately by the IOCTL argument
 * struct bytes.
 *
 * Field layout matches the architecture specification exactly; types use the
 * fixed-width kernel __u32 aliases so the struct is the same size in every
 * build (C or C++, 32-bit or 64-bit).
 */
struct slash_sysemu_socket_header {
    __u32 ioctl_op;     /**< The IOCTL operation to emulate. */
    __u32 sequence_id;  /**< A monotonically increasing sequence number. */
    /**
     * Signed errno encoded as uint32 two's-complement.
     * In requests this field carries the return value of a prior ioctl and
     * is not interpreted by the daemon.  In responses the daemon sets it to
     * the ioctl's errno (0 on success, a negative errno on failure).
     * Callers MUST cast this field to int32_t to recover the sign.
     */
    __u32 return_value;
    __u32 pad;          /**< Padding to reach 16 bytes; must be zero. */
};

/*
 * Verify the header is exactly 16 bytes.
 *
 * C89/C90 portable: a negative-size array is a compile-time error.
 * The C++ path uses static_assert for a cleaner diagnostic.
 */
#ifdef __cplusplus
static_assert(sizeof(struct slash_sysemu_socket_header) == 16,
              "slash_sysemu_socket_header must be exactly 16 bytes");
#else
typedef char slash_sysemu_hdr_size_check[
    (sizeof(struct slash_sysemu_socket_header) == 16) ? 1 : -1];
#endif

#endif /* SLASH_SYSEMU_UAPI_H */
