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

#include <cstdint>

namespace slash_sysemu {

/**
 * @brief Wire header for every datagram exchanged over the emulation sockets.
 *
 * Both request (user->daemon) and response (daemon->user) datagrams begin with
 * this fixed-size header, followed immediately by the IOCTL argument struct bytes.
 *
 * Field layout matches the architecture specification exactly; types use the
 * fixed-width stdint aliases so the struct is the same size in every build.
 */
struct slash_sysemu_socket_header {
    uint32_t ioctl_op;      /**< The IOCTL operation to emulate */
    uint32_t sequence_id;   /**< A monotonically increasing sequence number */
    uint32_t return_value;  /**< The return value of the IOCTL, can be set arbitrarily for requests */
    uint32_t pad;           /**< Padding */
};

static_assert(sizeof(slash_sysemu_socket_header) == 16,
              "slash_sysemu_socket_header must be exactly 16 bytes");

} // namespace slash_sysemu
