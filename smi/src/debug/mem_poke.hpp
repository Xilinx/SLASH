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

/// @file debug/mem_poke.hpp
/// @brief Declaration of the MemPoke debug command.

#ifndef SMI_DEBUG_MEM_POKE_HPP
#define SMI_DEBUG_MEM_POKE_HPP

#include <cstdint>
#include <optional>
#include <string>

/// @brief Identifies which broad memory space a mem-poke region belongs to.
enum class MemRegionKind { Raw, Ddr, Hbm };

/// @brief A parsed memory region specifier.
///
/// For HBM, @p hbmIndex selects a specific 512 MiB channel (0–63).
/// When the user passes bare "HBM" (no index), @p hbmWholeSpace is true
/// and bounds checking covers the entire HBM address space.
struct MemRegion {
    MemRegionKind kind         = MemRegionKind::Raw;
    uint32_t      hbmIndex     = 0;
    bool          hbmWholeSpace = false; ///< True when region is "HBM" (all channels).
};

/// @brief Static entry-point for the debug mem-poke command.
///
/// Reads or writes device memory at a raw physical address, bypassing the
/// allocator entirely.  Requires the raw-mem-access permission in vrtd.
///
/// This class is not instantiable; it groups the command options and
/// its run() entry-point.
class MemPoke {
    MemPoke() = delete;
public:
    /// @brief Options parsed from the CLI for the mem-poke command.
    struct Options {
        std::string bdf;                        ///< Target board address.
        bool readMode{};                        ///< True for read operations.
        bool writeMode{};                       ///< True for write operations.
        bool hexMode{};                         ///< True for hex-formatted read output.
        unsigned wordSize = 4;                  ///< Access width in bytes: 1, 2, 4, or 8.
        uint64_t count = 1;                     ///< Number of words to read (must be 1 for write).
        std::string addressText;                ///< Raw device physical address argument from CLI.
        std::optional<std::string> valueText;   ///< Optional raw value argument from CLI (scalar write).
        std::optional<std::string> filePath;    ///< Optional file path for file-mode read/write.
        std::string regionText;                 ///< Memory region: DDR, HBM, HBM0..HBM63, or RAW.
        bool relativeAddress{};                 ///< Interpret address as relative to region base.
        bool printBaseAddress{};                ///< Print region base address (hex) and exit.
        bool printSize{};                       ///< Print region size in bytes (hex) and exit.
    };

    /// @brief Executes the mem-poke command.
    /// @param options Populated options struct.
    /// @return Exit code (0 on success).
    static int run(const Options& options);
};

#endif // SMI_DEBUG_MEM_POKE_HPP
