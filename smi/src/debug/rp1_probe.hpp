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

/// @file debug/rp1_probe.hpp
/// @brief Declaration of the RP1 firmware bring-up probe debug commands.

#ifndef SMI_DEBUG_RP1_PROBE_HPP
#define SMI_DEBUG_RP1_PROBE_HPP

#include <cstdint>
#include <string>

/// @brief Low-level probes for the RP1 HSA command processor over BAR4.
///
/// These are diagnostics for firmware bring-up on real silicon, folded in
/// from the former @c examples/rp1_bringup scaffolding.  They talk to the
/// RP1 control block directly through the host-visible BAR window (no VRT
/// graph runtime) so they can validate firmware liveness and the shared-DDR
/// plumbing before any higher-level graph path is exercised.
///
/// This class is not instantiable; it groups the shared options and the
/// per-subcommand run() entry-points.
class Rp1Probe {
    Rp1Probe() = delete;
public:
    /// @brief Options parsed from the CLI for the RP1 probe commands.
    struct Options {
        std::string bdf;                        ///< Target board address.
        unsigned bar = 4;                       ///< BAR that maps the RP1 DDR window.
        std::string ctrlOffsetText = "0x4000000"; ///< Host BAR offset of the control block (default 64 MiB).
    };

    /// @brief Read and print the RP1 control block, sampling heartbeat for liveness.
    /// @param options Populated options struct.
    /// @return Exit code (0 on success).
    static int dump(const Options& options);

    /// @brief Submit a one-node SIGNAL graph and verify it completes end-to-end.
    /// @param options Populated options struct.
    /// @return Exit code (0 on success).
    static int ping(const Options& options);
};

#endif // SMI_DEBUG_RP1_PROBE_HPP
