// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// QDMA (PF1) IOCTL ABI
// ─────────────────────────────────────────────────────────────────────────────
//
// Like ctl_ioctls.h, slash_sysemu consumes the OFFICIAL libslash UAPI header for the
// QDMA structs and command numbers rather than mirroring them, so the daemon's
// socket wire format stays byte-for-byte identical with what libslash and the
// real driver use.
//
// The header provides:
//   * struct slash_qdma_info            (INFO payload; now carries the [out] bdf)
//   * struct slash_qdma_qpair_add       (QPAIR_ADD payload)
//   * struct slash_qdma_qpair_op        (Q_OP payload)  + SLASH_QDMA_QUEUE_OP_*
//   * struct slash_qdma_qpair_fd_request(QPAIR_GET_FD payload)
//   * struct slash_qdma_buf_create      (BUF_CREATE payload)
//   * struct slash_qdma_subxfer / slash_qdma_transfer (TRANSFER payload)
//   * the direction/hint/mode enums, SLASH_QDMA_FD_MAX_QPAIRS, SLASH_PCI_BDF_LEN
//   * SLASH_QDMA_IOCTL_INFO / _QPAIR_ADD / _Q_OP / _QPAIR_GET_FD / _BUF_CREATE
//     and SLASH_QDMA_QPAIR_IOCTL_TRANSFER
//
// This wrapper adds only convenient uint32_t aliases for the command numbers (the
// raw _IOWR macros are unsigned long; the socket header carries a uint32_t
// ioctl_op) plus the QDMA-mode constants the ABI doc references by name.

#include <slash/uapi/slash_interface.h>

#include <cstdint>

namespace slash_sysemu {

// Command numbers as uint32_t, matching slash_sysemu_socket_header::ioctl_op.  The
// _IOWR macro yields the same 32-bit encoding libslash uses to dispatch ioctls.
inline constexpr uint32_t kSlashQdmaIoctlInfo =
    static_cast<uint32_t>(SLASH_QDMA_IOCTL_INFO);
inline constexpr uint32_t kSlashQdmaIoctlQpairAdd =
    static_cast<uint32_t>(SLASH_QDMA_IOCTL_QPAIR_ADD);
inline constexpr uint32_t kSlashQdmaIoctlQOp =
    static_cast<uint32_t>(SLASH_QDMA_IOCTL_Q_OP);
inline constexpr uint32_t kSlashQdmaIoctlQpairGetFd =
    static_cast<uint32_t>(SLASH_QDMA_IOCTL_QPAIR_GET_FD);
inline constexpr uint32_t kSlashQdmaIoctlBufCreate =
    static_cast<uint32_t>(SLASH_QDMA_IOCTL_BUF_CREATE);
inline constexpr uint32_t kSlashQdmaQpairIoctlTransfer =
    static_cast<uint32_t>(SLASH_QDMA_QPAIR_IOCTL_TRANSFER);

// QDMA queue operating modes (struct slash_qdma_qpair_add::mode).  Only MM is
// supported; ST returns -EOPNOTSUPP (mirrors the real driver and the ABI doc).
inline constexpr uint32_t kQdmaQModeMm = 0; // AXI Memory Mapped
inline constexpr uint32_t kQdmaQModeSt = 1; // AXI Streaming (unsupported)

} // namespace slash_sysemu
