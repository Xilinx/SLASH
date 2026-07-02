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

#ifndef SMI_VALIDATE_HPP
#define SMI_VALIDATE_HPP

/// @file validate.hpp
/// @brief Declaration of the Validate command.
///
/// The Validate command optionally resets a V80 board and then exercises DDR
/// and HBM memory via PCIe by running data integrity checks followed by
/// parallel bandwidth measurements. Raw transfer modes skip reset and bypass
/// the default VRTD buffer path.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// @brief Static entry-point for the validate command.
///
/// This class is not instantiable; it groups the command's option
/// struct and its run() entry-point.
class Validate {
    Validate() = delete;
public:
    /// @brief Options parsed from the CLI for the validate command.
    struct Options {
        /// @brief How raw-transfer buffers map QDMA MM/NoC channels onto memory.
        ///
        /// On CPM5 the host-side NoC ingress port (NMU) is selected per queue by
        /// the SW-context mm-channel/host_id (SLASH uses qid&1), while the
        /// memory-side NoC egress endpoint (NSU / pseudo-channel) is selected by
        /// the device address.  Sustaining both NMUs requires also spreading
        /// across two NSUs; otherwise both ports converge on one memory endpoint
        /// and bandwidth caps at a single path.  This mirrors the off-the-shelf
        /// dma-perf knobs offset_ch0/offset_ch1.
        enum class ChannelAllocation {
            Auto,    ///< Interleaved: driver picks mm-channel (qid&1), addresses linear. Default; current behaviour.
            Paired,  ///< Couple mm-channel to a distinct memory region: even positions -> region 0, odd -> region 1.
        };

        /// @brief Per-queue AXI-MM/NoC channel selection for a buffer.
        ///
        /// Auto lets the driver stripe by qid&1; Ch0/Ch1 pin the queue to a
        /// single AXI-MM channel (and hence NoC channel).  Applies to the VRTD,
        /// raw SLASH, and off-the-shelf QDMA-driver backends.
        enum class MmChannel {
            Auto, ///< Driver stripes by qid&1 (default).
            Ch0,  ///< Pin to AXI-MM/NoC channel 0.
            Ch1,  ///< Pin to AXI-MM/NoC channel 1.
        };

        std::string bdf;           ///< BDF (Bus:Device.Function) address of the target device.
        unsigned threads = 8;      ///< Number of parallel buffers/threads (1-64).
        bool noReset = false;      ///< Skip the device reset step before running memory tests.
        bool ddrOnly = false;      ///< Skip HBM phase (mutually exclusive with hbmOnly).
        bool hbmOnly = false;      ///< Skip DDR phase (mutually exclusive with ddrOnly).
        bool rawTransferTest = false; ///< Use libslash raw QDMA transfers instead of VRTD buffers.
        bool useQdmaDriver = false;   ///< Run the raw test over the off-the-shelf Xilinx QDMA driver.
        /// Per-buffer AXI-MM channel selection, indexed by buffer position
        /// modulo size (a single entry applies to every buffer). Default auto.
        std::vector<MmChannel> mmChannels{MmChannel::Auto};
        uint64_t bufferSize = 512ULL * 1024ULL * 1024ULL; ///< Size of each test buffer.
        uint64_t offset = 512ULL * 1024ULL * 1024ULL; ///< Distance between logical buffer positions.
        uint64_t startingOffset = 0; ///< Offset from memory-space base for position 0.
        bool placementExplicit = false; ///< True when any placement option was provided.
        /// Raw-transfer NoC channel/memory placement strategy (raw modes only).
        ChannelAllocation channelAllocation = ChannelAllocation::Auto;
        /// Paired-mode byte distance between the two per-channel memory regions
        /// (the NSU / pseudo-channel stride). Default 16 GiB == MEMORY_SPACE_SIZE/2,
        /// which matches the dma-perf HBM offset_ch1-offset_ch0 spacing.
        uint64_t channelRegionStride = 16ULL * 1024ULL * 1024ULL * 1024ULL;
        /// Number of whole-buffer transfers per buffer in raw bandwidth phases.
        uint64_t bandwidthIterations = 1;
        /// Raw bandwidth phase duration in seconds. 0 means use fixed iterations.
        double bandwidthDuration = 0.0;
        /// Optional descriptor-ring size index for raw QDMA queue creation.
        std::optional<uint32_t> ringSizeIndex;
    };

    /// @brief Executes the validate command.
    /// @param options Populated options struct.
    /// @return Exit code (0 on success).
    static int run(const Options& options);

    /// @brief Parse a byte-size option accepting bare values and k/K/m/M suffixes.
    static uint64_t parseByteSizeOption(const std::string& text);

    /// @brief Parse an --mm-channel spec: a single auto|0|1 or a comma-separated list.
    static std::vector<Options::MmChannel> parseMmChannelSpec(const std::string& text);
};

#endif // SMI_VALIDATE_HPP
