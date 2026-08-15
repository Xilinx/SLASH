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

/// @file shell_build_id.hpp
/// @brief Reads the shell build-ID register (git commit + dirty flag) from
///        hardware over PCIe.

#ifndef SMI_SHELL_BUILD_ID_HPP
#define SMI_SHELL_BUILD_ID_HPP

#include <cstdint>
#include <string>

/// BAR index carrying the static-region build-ID GPIO (shared with the clock
/// wizards). Must match the address assigned in the shell block design.
constexpr uint8_t BUILD_ID_BAR = 4;

/// Offset of the build-ID AXI GPIO within BAR4. Its BD address is
/// 0x0204_0002_0000, and BAR4 maps the 0x0204_0000_0000 aperture.
constexpr uint64_t BUILD_ID_OFFSET = 0x20000;

/// AXI GPIO channel-1 data register (low 32 bits of the commit prefix).
constexpr uint64_t BUILD_ID_REG_LO = BUILD_ID_OFFSET + 0x0;

/// AXI GPIO channel-2 data register: bits[27:0] hash, bit[28] shell type,
/// bits[30:29] reserved, bit[31] dirty.
constexpr uint64_t BUILD_ID_REG_HI = BUILD_ID_OFFSET + 0x8;

/// Mask of the high hash bits (bits[27:0]) within the channel-2 register.
constexpr uint32_t BUILD_ID_HI_HASH_MASK = 0x0FFFFFFFu;

/// Mask of the shell-type flag (bit[28]) within the channel-2 register.
constexpr uint32_t BUILD_ID_HI_SHELL_MASK = 0x10000000u;

/// Mask of the still-unassigned reserved bits (bits[30:29]).
constexpr uint32_t BUILD_ID_HI_RESERVED_MASK = 0x60000000u;

/// Mask of the dirty-tree flag (bit[31]) within the channel-2 register.
constexpr uint32_t BUILD_ID_HI_DIRTY_MASK = 0x80000000u;

/// @brief Shell variant encoded in bit[28] of the build-ID high word.
enum class ShellVariant : uint8_t {
    Service = 0,  ///< Service shell.
    Compute = 1,  ///< Compute shell.
};

/// @brief Shell build-ID decoded from the hardware register.
struct BuildId {
    uint32_t lo{};        ///< Low 32 bits of the 60-bit commit prefix.
    uint32_t hiHash{};    ///< High 28 bits of the commit prefix (bits[27:0]).
    bool     dirty{};     ///< True if the shell was built from a dirty tree.

    /// Which shell variant is loaded, as reported by the hardware itself.
    ShellVariant shell{ShellVariant::Service};

    /// Returns the 60-bit commit prefix as a "0x"-prefixed hex string.
    std::string commitHex() const;

    /// Returns the shell variant as a lowercase name ("service" / "compute").
    const char* shellName() const;
};

/// @brief Reads the build-ID register from the device at @p bdf over PCIe.
/// @throws std::exception on any vrtd/BAR access failure.
BuildId readBuildId(const std::string& bdf);

#endif // SMI_SHELL_BUILD_ID_HPP
