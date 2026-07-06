/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/// @file meta_ram.hpp
/// @brief Layout of the static-shell per-RM metadata readback RAMs, and helpers
///        to encode/decode their contents.
///
/// Two 8 KiB writable RAMs live in the static shell (see
/// `linker/.../scripts/top.tcl`): one for the user (slash) RM and one for the
/// service RM. The runtime writes a small header plus the gzip-compressed
/// metadata XML describing what was programmed into each region; SMI (or the
/// runtime) reads it back to ground inspection in hardware rather than trusting
/// the host-side vbin. Because the RAMs are static-shell resident, partial
/// reconfiguration of an RM does not erase them.

#ifndef VRT_META_RAM_HPP
#define VRT_META_RAM_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace vrt {

/// BAR index carrying the static-shell peripherals (shared with the clock
/// wizards and build-ID GPIO). Must match the address assigned in the shell
/// block design.
constexpr uint8_t META_RAM_BAR = 4;

/// BAR4 maps the 0x0204_0000_0000 aperture; register offsets are the assigned
/// BD address minus this base (matches shell_build_id.hpp).
constexpr uint64_t META_RAM_APERTURE_BASE = 0x020400000000ULL;

/// BD address of the user-RM metadata RAM (top.tcl assign_bd_address).
constexpr uint64_t USER_META_RAM_BD_ADDR = 0x020400030000ULL;

/// BD address of the service-RM metadata RAM (top.tcl assign_bd_address).
constexpr uint64_t SERVICE_META_RAM_BD_ADDR = 0x020400040000ULL;

/// Offset of the user-RM metadata RAM within BAR4.
constexpr uint64_t USER_META_RAM_OFFSET = USER_META_RAM_BD_ADDR - META_RAM_APERTURE_BASE;

/// Offset of the service-RM metadata RAM within BAR4.
constexpr uint64_t SERVICE_META_RAM_OFFSET = SERVICE_META_RAM_BD_ADDR - META_RAM_APERTURE_BASE;

/// Usable size of each metadata RAM in bytes (8 KiB).
constexpr uint32_t META_RAM_SIZE = 8 * 1024;

/// Magic in the header word[0] high 16 bits identifying a valid metadata blob.
constexpr uint32_t META_RAM_MAGIC = 0x534D;  // 'SM' (System Map)

/// Header layout (little-endian 32-bit words at the start of each RAM):
///   word[0]: bits[31:16] = magic (META_RAM_MAGIC), bits[7:0] = version,
///            bit[8]      = gzip flag
///   word[1]: payload length in bytes (compressed)
///   word[2]: CRC32 of the compressed payload
/// Payload (compressed XML) starts at META_RAM_HEADER_BYTES.
constexpr uint32_t META_RAM_VERSION = 1;
constexpr uint32_t META_RAM_FLAG_GZIP = 0x100;
constexpr uint32_t META_RAM_HEADER_BYTES = 16;

/// Maximum compressed payload that fits in a RAM.
constexpr uint32_t META_RAM_MAX_PAYLOAD = META_RAM_SIZE - META_RAM_HEADER_BYTES;

/// @brief Encode an XML metadata string into the RAM image (header + gzip
///        payload), zero-padded to a whole number of 32-bit words.
/// @param xml The metadata XML.
/// @return The byte image ready to be written into the RAM, size a multiple of 4.
/// @throws std::runtime_error if the compressed payload exceeds META_RAM_MAX_PAYLOAD.
std::vector<uint8_t> encodeMetaRam(const std::string& xml);

/// @brief Decode a RAM image (as read back from hardware) into the XML string.
/// @param image Bytes read from the RAM (at least META_RAM_HEADER_BYTES).
/// @return The decompressed metadata XML.
/// @throws std::runtime_error on bad magic, length, CRC mismatch, or inflate error.
std::string decodeMetaRam(const std::vector<uint8_t>& image);

}  // namespace vrt

#endif  // VRT_META_RAM_HPP
