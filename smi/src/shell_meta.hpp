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

/// @file shell_meta.hpp
/// @brief Reads the per-RM metadata RAMs (compressed system-map XML) from the
///        static shell over PCIe, so inspection reflects what is actually
///        programmed rather than trusting the host-side vbin.

#ifndef SMI_SHELL_META_HPP
#define SMI_SHELL_META_HPP

#include <string>

/// Which RM's metadata RAM to read.
enum class MetaRegion {
    User,
    Service,
};

/// @brief Reads and decodes the metadata XML from the RAM for @p region on the
///        device at @p bdf over PCIe.
/// @return The decompressed system-map XML for that region.
/// @throws std::exception on BAR access failure, empty/uninitialized RAM, or
///         decode error.
std::string readShellMeta(const std::string& bdf, MetaRegion region);

#endif  // SMI_SHELL_META_HPP
