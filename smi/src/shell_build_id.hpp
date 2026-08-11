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

/// AXI GPIO channel-1 data register (low 32 bits of the SHA-1).
constexpr uint64_t BUILD_ID_REG_LO = BUILD_ID_OFFSET + 0x0;

/// AXI GPIO channel-2 data register (bits[30:0] hash, bit[31] dirty).
constexpr uint64_t BUILD_ID_REG_HI = BUILD_ID_OFFSET + 0x8;

/// @brief Shell build-ID decoded from the hardware register.
struct BuildId {
    uint32_t lo{};        ///< Low 32 bits of the git SHA-1.
    uint32_t hiHash{};    ///< Next 31 bits of the git SHA-1 (bits[30:0]).
    bool     dirty{};     ///< True if the shell was built from a dirty tree.

    /// Returns the 60-bit commit prefix as a "0x"-prefixed hex string.
    std::string commitHex() const;
};

/// @brief Reads the build-ID register from the device at @p bdf over PCIe.
/// @throws std::exception on any vrtd/BAR access failure.
BuildId readBuildId(const std::string& bdf);

#endif // SMI_SHELL_BUILD_ID_HPP
