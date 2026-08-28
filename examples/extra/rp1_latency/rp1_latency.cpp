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

/**
 * @file rp1_latency.cpp
 * @brief Compare RP1 command-processor latency with the legacy VRT path.
 */

#include "latency_stats.hpp"

#include <slash/uapi/rp1_protocol.h>

#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/graph/backend_executable.hpp>
#include <vrt/graph/device/fpga/rp1_program.hpp>
#include <vrt/graph/graph.hpp>
#include <vrt/kernel.hpp>
#include <vrt/utils/logger.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

/// Monotonic host clock used for every wall-clock sample.
using Clock = std::chrono::steady_clock;

/// Logical image name registered with the graph-side VRT device.
constexpr const char* kImageName = "latency";
/// CU name emitted by the benchmark connectivity configuration.
constexpr const char* kKernelName = "latency_kernel_0";
/// Per-submission timeout; a timeout poisons the RP1 submitter.
constexpr std::chrono::milliseconds kRp1Timeout{30000};
/// First private scratch range in the host-visible RP1 DDR window.
constexpr std::uint32_t kScratchSourceOffset = 32u << 20;
/// Second private scratch range in the host-visible RP1 DDR window.
constexpr std::uint32_t kScratchDestinationOffset = 48u << 20;
/// Maximum transfer size that keeps both scratch ranges disjoint.
constexpr std::size_t kScratchCapacity = 8u << 20;
static_assert(kScratchCapacity <= RP1_DMA_LENGTH_MASK);
/// Largest chain supported by the lowering's 31-by-31 barrier-bit budget.
constexpr std::size_t kMaxBatchSize = 961;
static_assert(kMaxBatchSize + 1 <= RP1_MAX_NODES);
static_assert(3u * kMaxBatchSize + 4u <= RP1_MAX_TRACE_ENTRIES);
/// Default immediate-operation chain used to amortize 80 ns PMU ticks.
constexpr std::size_t kMemoryChainLength = 128;
/// Entries in firmware's fixed 4 KiB BTCM trace staging page.
constexpr std::size_t kTraceStagingEntries =
    4096u / sizeof(rp1_trace_entry_t);
/// Largest chain whose activation events fit without a trace-page flush.
constexpr std::size_t kMaxMemoryChainLength =
    kTraceStagingEntries - 4u;
static_assert(kMaxMemoryChainLength <= RP1_MAX_NODES);
/// Signal slot reserved for the SCALAR_READ memory microbenchmark.
constexpr std::uint8_t kMemorySignalSlot =
    static_cast<std::uint8_t>(RP1_MAX_SIGNALS - 2u);
/// Distinct fill pattern used to verify every DMA_FILL graph.
constexpr std::uint32_t kFillPattern = 0xA5A5A5A5u;
/// Distinct source pattern used to verify every DMA_COPY graph.
constexpr std::uint32_t kCopyPattern = 0x5A5A5A5Au;

/**
 * @brief Immediate operation exercised by one memory-trace chain.
 */
enum class MemoryOperation {
    /// Scanner-only baseline.
    Nop,
    /// Repeated DDR stores followed by DSB ST.
    DmaFill,
    /// Repeated DDR loads/stores followed by DSB ST.
    DmaCopy,
    /// One DDR store followed by DSB ST.
    ScalarWrite,
    /// One DDR load followed by signal-slot publication.
    ScalarRead,
};

/**
 * @brief User-selected benchmark dimensions and output format.
 */
struct Config {
    /// Target device PCI BDF.
    std::string bdf;
    /// Hardware vbin containing the no-op kernel.
    std::string vbin;
    /// vrtd Unix socket used by both runtime paths.
    std::string socket = "/run/vrtd.sock";
    /// Samples for kernel and batch metrics.
    std::size_t iterations = 50;
    /// Untimed runs before kernel, batch, and transfer samples.
    std::size_t warmup = 5;
    /// Samples that reprogram the user region through each path.
    std::size_t programIterations = 3;
    /// Samples for each transfer size and direction.
    std::size_t transferIterations = 10;
    /// Instrumented submissions used for RP1 trace intervals.
    std::size_t traceIterations = 5;
    /// Sequential dispatch counts to compare.
    std::vector<std::size_t> batchSizes{1, 10, 100};
    /// Byte counts for VRT sync and RP1 local copies.
    std::vector<std::size_t> transferSizes{4, 64, 4096, 1u << 20};
    /// Byte counts for differential RP1 DDR tracing.
    std::vector<std::size_t> memorySizes{4, 64, 256, 4096};
    /// Immediate nodes in each memory-trace chain.
    std::size_t memoryChainLength = kMemoryChainLength;
    /// Optional R5 clock for converting 64-cycle PMU ticks.
    std::optional<std::uint64_t> r5FrequencyHz;
    /// Emit machine-readable CSV instead of aligned columns.
    bool csv = false;
};

/**
 * @brief Named samples sharing one unit and benchmark boundary.
 */
struct Measurement {
    /// Stable metric identifier.
    std::string name;
    /// Unit shared by every sample.
    std::string unit;
    /// Raw observations summarized only at output time.
    std::vector<std::uint64_t> samples;
};

/**
 * @brief Separate host durations for a split launch/wait API.
 */
struct SplitSamples {
    /// Time spent in the launch call.
    std::vector<std::uint64_t> launch;
    /// Time spent in the following wait call.
    std::vector<std::uint64_t> wait;
    /// Launch-to-completed wall-clock interval.
    std::vector<std::uint64_t> total;
};

/**
 * @brief Host duration paired with the immutable result returned by RP1.
 */
struct TimedGraphResult {
    /// submitAndWait() wall-clock duration in nanoseconds.
    std::uint64_t elapsed = 0;
    /// Typed protocol-v6 result captured before the stop timestamp.
    vrt::graph::fpga::Rp1GraphResult result;
};

/**
 * @brief RP1 trace intervals expressed in protocol PMU ticks.
 */
struct TraceIntervals {
    /// Graph-start to first kernel-launch interval.
    std::uint32_t dispatch = 0;
    /// First kernel-launch to last kernel-completion interval.
    std::uint32_t kernelSpan = 0;
    /// Graph-start to graph-done interval.
    std::uint32_t graph = 0;
    /// Each kernel launch to its dependent successor's launch.
    std::vector<std::uint32_t> launchGaps;
    /// Launch gaps after subtracting any bracketed BTCM-to-DDR flush.
    std::vector<std::uint32_t> launchGapsExcludingFlush;
    /// Each kernel completion to its dependent successor's launch.
    std::vector<std::uint32_t> handoffGaps;
    /// Handoff gaps after subtracting any bracketed BTCM-to-DDR flush.
    std::vector<std::uint32_t> handoffGapsExcludingFlush;
    /// Each blocking BTCM-to-DDR trace flush.
    std::vector<std::uint32_t> flushDurations;
};

/**
 * @brief Print command-line syntax and benchmark defaults.
 */
void printUsage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " --bdf BDF --vbin FILE [options]\n"
        << "\n"
        << "Options:\n"
        << "  --socket PATH              vrtd socket (default: /run/vrtd.sock)\n"
        << "  --iterations N             kernel/batch samples (default: 50)\n"
        << "  --warmup N                 untimed warmups (default: 5)\n"
        << "  --program-iterations N     programming samples (default: 3)\n"
        << "  --transfer-iterations N    transfer samples (default: 10)\n"
        << "  --trace-iterations N       instrumented RP1 samples (default: 5)\n"
        << "  --batch-sizes N,N,...      sequential kernels per batch\n"
        << "  --transfer-sizes N,N,...   transfer bytes; multiples of four\n"
        << "  --memory-sizes N,N,...     traced DDR bytes; multiples of four\n"
        << "  --memory-chain N           immediate nodes per traced graph\n"
        << "  --r5-hz HZ                 also convert PMU ticks to estimated ns\n"
        << "  --csv                       emit CSV summaries\n"
        << "  --help, -h                  show this help\n";
}

/**
 * @brief Parse a bounded decimal size, optionally accepting zero.
 *
 * @throws std::runtime_error when @p text is malformed or out of range.
 */
std::size_t parseSize(
    const std::string& text, const char* option, bool allowZero = false) {
    try {
        std::size_t consumed = 0;
        if (text.empty() || text.front() == '-') {
            throw std::invalid_argument("negative");
        }
        const unsigned long long value = std::stoull(text, &consumed, 10);
        if (consumed != text.size() ||
            value > std::numeric_limits<std::size_t>::max() ||
            (!allowZero && value == 0)) {
            throw std::out_of_range("size");
        }
        return static_cast<std::size_t>(value);
    } catch (const std::exception&) {
        throw std::runtime_error(
            std::string("invalid value for ") + option + ": " + text);
    }
}

/**
 * @brief Parse a non-empty comma-separated list of positive sizes.
 */
std::vector<std::size_t> parseSizeList(
    const std::string& text, const char* option) {
    std::vector<std::size_t> values;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t comma = text.find(',', begin);
        const std::size_t end =
            comma == std::string::npos ? text.size() : comma;
        values.push_back(parseSize(text.substr(begin, end - begin), option));
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return values;
}

/**
 * @brief Parse CLI options and reject dimensions unsupported by RP1 buffers.
 */
Config parseArgs(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto need = [&](const char* option) {
            if (++i >= argc) {
                throw std::runtime_error(
                    std::string("missing argument to ") + option);
            }
            return std::string(argv[i]);
        };

        if (argument == "--bdf") {
            config.bdf = need("--bdf");
        } else if (argument == "--vbin") {
            config.vbin = need("--vbin");
        } else if (argument == "--socket") {
            config.socket = need("--socket");
        } else if (argument == "--iterations") {
            config.iterations =
                parseSize(need("--iterations"), "--iterations");
        } else if (argument == "--warmup") {
            config.warmup =
                parseSize(need("--warmup"), "--warmup", true);
        } else if (argument == "--program-iterations") {
            config.programIterations = parseSize(
                need("--program-iterations"), "--program-iterations");
        } else if (argument == "--transfer-iterations") {
            config.transferIterations = parseSize(
                need("--transfer-iterations"), "--transfer-iterations");
        } else if (argument == "--trace-iterations") {
            config.traceIterations = parseSize(
                need("--trace-iterations"), "--trace-iterations");
        } else if (argument == "--batch-sizes") {
            config.batchSizes =
                parseSizeList(need("--batch-sizes"), "--batch-sizes");
        } else if (argument == "--transfer-sizes") {
            config.transferSizes =
                parseSizeList(need("--transfer-sizes"), "--transfer-sizes");
        } else if (argument == "--memory-sizes") {
            config.memorySizes =
                parseSizeList(need("--memory-sizes"), "--memory-sizes");
        } else if (argument == "--memory-chain") {
            config.memoryChainLength =
                parseSize(need("--memory-chain"), "--memory-chain");
        } else if (argument == "--r5-hz") {
            config.r5FrequencyHz = static_cast<std::uint64_t>(
                parseSize(need("--r5-hz"), "--r5-hz"));
        } else if (argument == "--csv") {
            config.csv = true;
        } else if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }

    if (config.bdf.empty() || config.vbin.empty()) {
        printUsage(argv[0]);
        throw std::runtime_error("--bdf and --vbin are required");
    }
    for (std::size_t batch : config.batchSizes) {
        /*
         * The image builder has 31 work buckets with 31 usable bits each.
         * This bound also makes the following node and trace arithmetic safe.
         */
        if (batch > kMaxBatchSize) {
            throw std::runtime_error(
                "batch size exceeds RP1 barrier capacity (" +
                std::to_string(kMaxBatchSize) + "): " +
                std::to_string(batch));
        }
    }
    for (std::size_t bytes : config.transferSizes) {
        if ((bytes & 3u) != 0 || bytes > kScratchCapacity) {
            throw std::runtime_error(
                "transfer sizes must be multiples of four and no larger than " +
                std::to_string(kScratchCapacity) + " bytes");
        }
    }
    for (std::size_t bytes : config.memorySizes) {
        if ((bytes & 3u) != 0 || bytes > kScratchCapacity) {
            throw std::runtime_error(
                "memory sizes must be multiples of four and no larger than " +
                std::to_string(kScratchCapacity) + " bytes");
        }
    }
    if (config.memoryChainLength < 2u ||
        config.memoryChainLength > kMaxMemoryChainLength) {
        throw std::runtime_error(
            "memory chain must be in [2, " +
            std::to_string(kMaxMemoryChainLength) + "]");
    }
    return config;
}

/**
 * @brief Run @p operation once and return elapsed host time in nanoseconds.
 */
template <class Operation>
std::uint64_t elapsedNs(Operation&& operation) {
    const Clock::time_point start = Clock::now();
    std::forward<Operation>(operation)();
    const Clock::time_point finish = Clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            finish - start)
            .count());
}

/**
 * @brief Measure APIs whose launch and wait calls expose distinct host costs.
 */
template <class Launch, class Wait>
SplitSamples measureSplit(
    std::size_t warmup, std::size_t iterations,
    Launch&& launch, Wait&& wait) {
    for (std::size_t i = 0; i < warmup; ++i) {
        launch();
        wait();
    }

    SplitSamples samples;
    samples.launch.reserve(iterations);
    samples.wait.reserve(iterations);
    samples.total.reserve(iterations);
    for (std::size_t i = 0; i < iterations; ++i) {
        const Clock::time_point start = Clock::now();
        launch();
        const Clock::time_point launched = Clock::now();
        wait();
        const Clock::time_point finished = Clock::now();
        samples.launch.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                launched - start)
                .count()));
        samples.wait.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - launched)
                .count()));
        samples.total.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - start)
                .count()));
    }
    return samples;
}

/**
 * @brief Return the smallest power-of-two ring that contains @p entries.
 */
std::uint32_t ringSize(std::size_t entries, std::uint32_t maximum) {
    std::uint32_t size = 1;
    while (size < entries && size < maximum) size <<= 1u;
    if (size < entries || size > maximum) {
        throw std::runtime_error("requested RP1 ring is too small");
    }
    return size;
}

/**
 * @brief Time one complete @c submitAndWait() host round trip.
 *
 * The interval includes submitter preflight, staging, polling, result reads,
 * and protocol consistency validation. Benchmark-specific semantic checks run
 * after this function returns and remain outside the latency boundary.
 */
TimedGraphResult timedSubmission(
    const std::shared_ptr<vrt::graph::fpga::Rp1Submitter>& submitter,
    const vrt::graph::fpga::Rp1GraphImage& image) {
    const Clock::time_point start = Clock::now();
    vrt::graph::fpga::Rp1GraphResult result =
        submitter->submitAndWait(image, kRp1Timeout);
    const Clock::time_point finish = Clock::now();
    return {
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finish - start)
                .count()),
        std::move(result),
    };
}

/**
 * @brief Require one clean benchmark result with the expected operation count.
 *
 * @throws std::runtime_error when terminal, image, timing, trace, or
 * quiescence evidence is inconsistent with a reusable benchmark device.
 */
void validateGraphResult(
    const vrt::graph::fpga::Rp1GraphResult& result,
    std::size_t expectedOperations, bool traceExpected) {
    constexpr std::uint32_t kKnownFlags =
        RP1_RESULT_RECOVERY_REQUIRED |
        RP1_RESULT_EFFECTS_MAY_BE_PARTIAL |
        RP1_RESULT_INFINITE_WORK_REMAINS |
        RP1_RESULT_TRACE_ENABLED |
        RP1_RESULT_TRACE_OVERFLOW |
        RP1_RESULT_UNREACHED_NODES;
    constexpr std::uint32_t kFailureFlags =
        RP1_RESULT_RECOVERY_REQUIRED |
        RP1_RESULT_EFFECTS_MAY_BE_PARTIAL |
        RP1_RESULT_INFINITE_WORK_REMAINS |
        RP1_RESULT_TRACE_OVERFLOW |
        RP1_RESULT_UNREACHED_NODES;

    if (!result.succeeded() || result.terminal ||
        (result.flags & ~kKnownFlags) != 0u ||
        (result.flags & kFailureFlags) != 0u) {
        throw std::runtime_error(
            "RP1 benchmark received a failed or non-reusable graph result");
    }
    const bool traceEnabled =
        result.hasFlags(RP1_RESULT_TRACE_ENABLED);
    if (traceEnabled != traceExpected ||
        (traceExpected && result.traceWriteIndex == 0u) ||
        (!traceExpected && result.traceWriteIndex != 0u)) {
        throw std::runtime_error(
            "RP1 benchmark result has inconsistent trace evidence");
    }
    if (result.imageState != vrt::graph::fpga::Rp1ImageState::Known ||
        result.activeImageId == 0u) {
        throw std::runtime_error(
            "RP1 benchmark result does not preserve a known active image");
    }
    if (expectedOperations > std::numeric_limits<std::uint32_t>::max() ||
        result.completedOperations != expectedOperations) {
        throw std::runtime_error(
            "RP1 benchmark result has an unexpected completed-operation count");
    }
    if (result.publishElapsedTicks < result.graphElapsedTicks) {
        throw std::runtime_error(
            "RP1 benchmark result publication precedes graph completion");
    }
    if (result.quiescence.finiteDone != 0u ||
        result.quiescence.finiteTimeout != 0u ||
        result.quiescence.infinite != 0u) {
        throw std::runtime_error(
            "RP1 benchmark result required terminal quiescence");
    }
}

/**
 * @brief Build one direct RP1 PDI_LOAD program for the selected image.
 */
vrt::graph::Rp1QueueProgram makeReprogramProgram(
    const std::string& deviceId,
    const vrt::graph::FpgaImageHandle& image) {
    vrt::graph::Rp1QueueProgram program;
    program.device = vrt::graph::DeviceId(deviceId);

    vrt::graph::Rp1ReprogramCommand load;
    load.id = "load_" + image.id();
    load.deviceId = deviceId;
    load.imageId = image.id();
    load.pdiPath = image.pdiPath();
    program.commands.emplace_back(std::move(load));
    return program;
}

/**
 * @brief Build a dependency chain of no-argument dispatches to one FPGA CU.
 */
vrt::graph::Rp1QueueProgram makeKernelProgram(
    const std::string& deviceId, const std::string& imageId,
    std::size_t count) {
    vrt::graph::Rp1QueueProgram program;
    program.device = vrt::graph::DeviceId(deviceId);

    std::string predecessor;
    for (std::size_t i = 0; i < count; ++i) {
        vrt::graph::Rp1KernelCommand kernel;
        kernel.id = "kernel_" + std::to_string(i);
        kernel.deviceId = deviceId;
        kernel.kernel.name = kKernelName;
        kernel.kernel.type = vrt::graph::DeviceType::FPGA;
        kernel.kernel.image = imageId;
        if (!predecessor.empty()) kernel.dependsOn.push_back(predecessor);
        predecessor = kernel.id;
        program.commands.emplace_back(std::move(kernel));
    }
    return program;
}

/**
 * @brief Build one raw phase-1 DDR-to-DDR software-copy graph image.
 */
vrt::graph::fpga::Rp1GraphImage makeDmaImage(std::size_t bytes) {
    if (bytes > RP1_DMA_LENGTH_MASK) {
        throw std::invalid_argument(
            "RP1 DMA_COPY length exceeds the protocol-v6 28-bit field");
    }
    vrt::graph::fpga::Rp1GraphImage image;
    image.nodes.resize(1);
    rp1_node_t& node = image.nodes.front();
    rp1_node_set_opcode(&node, RP1_OP_DMA_COPY);
    rp1_node_set_status(&node, RP1_NODE_PENDING);
    node.payload.dma_copy.src_addr_lo =
        RP1_CTRL_PHYS_ADDR + kScratchSourceOffset;
    node.payload.dma_copy.dst_addr_lo =
        RP1_CTRL_PHYS_ADDR + kScratchDestinationOffset;
    node.payload.dma_copy.length_types = rp1_dma_pack(
        static_cast<std::uint32_t>(bytes), 0u, 0u);
    return image;
}

/**
 * @brief Build one dependency chain of identical immediate memory operations.
 *
 * Every activation raises a unique barrier consumed by the following node, so
 * the flat scanner executes the whole chain in one pass. Adjacent
 * NODE_ACTIVATE timestamps therefore bracket the preceding operation without
 * heartbeat, WAIT-scan, or outer-pass overhead.
 */
vrt::graph::fpga::Rp1GraphImage makeMemoryTraceImage(
    MemoryOperation operation, std::size_t bytes, std::size_t nodeCount) {
    if (nodeCount < 2u || nodeCount > kMaxMemoryChainLength) {
        throw std::invalid_argument("memory trace node count is out of range");
    }
    if (bytes == 0u || (bytes & 3u) != 0u ||
        bytes > kScratchCapacity) {
        throw std::invalid_argument("memory trace byte count is out of range");
    }
    if ((operation == MemoryOperation::ScalarWrite ||
         operation == MemoryOperation::ScalarRead) &&
        bytes != sizeof(std::uint32_t)) {
        throw std::invalid_argument("scalar memory trace must be four bytes");
    }

    vrt::graph::fpga::Rp1GraphImage image;
    image.nodes.resize(nodeCount);
    image.trace_enable = true;
    image.trace_size_override = ringSize(
        2u + nodeCount, RP1_MAX_TRACE_ENTRIES);

    for (std::size_t i = 0u; i < nodeCount; ++i) {
        rp1_node_t& node = image.nodes[i];
        rp1_node_set_status(&node, RP1_NODE_PENDING);
        if (i != 0u) {
            const std::size_t predecessor = i - 1u;
            node.barrier_await_bucket =
                static_cast<std::uint8_t>(predecessor / 32u);
            node.barrier_await_mask =
                1u << static_cast<std::uint32_t>(predecessor % 32u);
        }
        node.barrier_set_bucket =
            static_cast<std::uint8_t>(i / 32u);
        node.barrier_set_mask =
            1u << static_cast<std::uint32_t>(i % 32u);

        switch (operation) {
        case MemoryOperation::Nop:
            rp1_node_set_opcode(&node, RP1_OP_NOP);
            break;
        case MemoryOperation::DmaFill:
            rp1_node_set_opcode(&node, RP1_OP_DMA_FILL);
            node.payload.dma_fill.dst_addr_lo =
                RP1_CTRL_PHYS_ADDR + kScratchDestinationOffset;
            node.payload.dma_fill.length =
                static_cast<std::uint32_t>(bytes);
            node.payload.dma_fill.pattern = kFillPattern;
            break;
        case MemoryOperation::DmaCopy:
            rp1_node_set_opcode(&node, RP1_OP_DMA_COPY);
            node.payload.dma_copy.src_addr_lo =
                RP1_CTRL_PHYS_ADDR + kScratchSourceOffset;
            node.payload.dma_copy.dst_addr_lo =
                RP1_CTRL_PHYS_ADDR + kScratchDestinationOffset;
            node.payload.dma_copy.length_types = rp1_dma_pack(
                static_cast<std::uint32_t>(bytes), 0u, 0u);
            break;
        case MemoryOperation::ScalarWrite:
            rp1_node_set_opcode(&node, RP1_OP_SCALAR_WRITE);
            node.payload.scalar_write.writes[0].addr =
                RP1_CTRL_PHYS_ADDR + kScratchDestinationOffset;
            node.payload.scalar_write.writes[0].value = kFillPattern;
            break;
        case MemoryOperation::ScalarRead:
            rp1_node_set_opcode(&node, RP1_OP_SCALAR_READ);
            node.payload.scalar_read.source_addr =
                RP1_CTRL_PHYS_ADDR + kScratchSourceOffset;
            node.payload.scalar_read.target_slot = kMemorySignalSlot;
            break;
        }
    }
    if (operation == MemoryOperation::ScalarRead) {
        image.clear_signal_slots.push_back(kMemorySignalSlot);
    }
    return image;
}

/**
 * @brief Extract each adjacent NODE_ACTIVATE interval from one trace capture.
 *
 * @throws std::runtime_error for overflow, flush markers, duplicate/missing
 * activation events, or missing graph lifecycle markers.
 */
std::vector<std::uint32_t> extractActivationGaps(
    const vrt::graph::fpga::Rp1TraceCapture& trace,
    std::size_t nodeCount) {
    if (trace.overflow) {
        throw std::runtime_error(
            "RP1 memory trace overflowed during benchmark");
    }

    bool graphStart = false;
    bool graphDone = false;
    std::vector<std::optional<std::uint32_t>> activations(nodeCount);
    for (const rp1_trace_entry_t& entry : trace.entries) {
        if (entry.event == RP1_TRACE_GRAPH_START) {
            graphStart = true;
        } else if (entry.event == RP1_TRACE_GRAPH_DONE) {
            graphDone = true;
        } else if (entry.event == RP1_TRACE_FLUSH_START ||
                   entry.event == RP1_TRACE_FLUSH_END) {
            throw std::runtime_error(
                "RP1 memory trace unexpectedly flushed");
        } else if (entry.event == RP1_TRACE_NODE_ACTIVATE &&
                   entry.node_index < nodeCount) {
            if (activations[entry.node_index]) {
                throw std::runtime_error(
                    "RP1 memory trace contains duplicate activation");
            }
            activations[entry.node_index] = entry.timestamp;
        }
    }
    if (!graphStart || !graphDone) {
        throw std::runtime_error(
            "RP1 memory trace is missing graph lifecycle events");
    }

    std::vector<std::uint32_t> gaps;
    gaps.reserve(nodeCount - 1u);
    for (std::size_t i = 0u; i < nodeCount; ++i) {
        if (!activations[i]) {
            throw std::runtime_error(
                "RP1 memory trace is missing node activation " +
                std::to_string(i));
        }
        if (i != 0u) {
            gaps.push_back(*activations[i] - *activations[i - 1u]);
        }
    }
    return gaps;
}

/**
 * @brief Submit one traced immediate chain and return its activation gaps.
 */
std::vector<std::uint32_t> runMemoryTrace(
    const std::shared_ptr<vrt::graph::fpga::Rp1Submitter>& submitter,
    const vrt::graph::fpga::Rp1GraphImage& image) {
    const vrt::graph::fpga::Rp1GraphResult result =
        submitter->submitAndWait(image, kRp1Timeout);
    validateGraphResult(
        result, image.nodes.size(), /*traceExpected=*/true);
    const vrt::graph::fpga::Rp1TraceCapture trace =
        submitter->drainTrace();
    if (result.traceWriteIndex != trace.written) {
        throw std::runtime_error(
            "RP1 memory result and trace cursor disagree");
    }
    return extractActivationGaps(trace, image.nodes.size());
}

/**
 * @brief Extract wrap-safe kernel intervals from one non-overflowing trace.
 */
TraceIntervals extractTrace(
    const vrt::graph::fpga::Rp1TraceCapture& trace,
    std::size_t kernelCount) {
    if (trace.overflow) {
        throw std::runtime_error("RP1 trace overflowed during benchmark");
    }

    std::optional<std::uint32_t> graphStart;
    std::optional<std::uint32_t> firstLaunch;
    std::optional<std::uint32_t> lastDone;
    std::optional<std::uint32_t> graphDone;
    std::vector<std::optional<std::uint32_t>> launches(kernelCount);
    std::vector<std::optional<std::uint32_t>> completions(kernelCount);
    std::optional<std::uint32_t> flushStart;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> flushes;
    for (const rp1_trace_entry_t& entry : trace.entries) {
        if (entry.event == RP1_TRACE_GRAPH_START) {
            graphStart = entry.timestamp;
        } else if (
            entry.event == RP1_TRACE_KERNEL_LAUNCH &&
            entry.node_index < kernelCount) {
            launches[entry.node_index] = entry.timestamp;
            if (!firstLaunch) firstLaunch = entry.timestamp;
        } else if (
            entry.event == RP1_TRACE_KERNEL_DONE &&
            entry.node_index < kernelCount) {
            completions[entry.node_index] = entry.timestamp;
            lastDone = entry.timestamp;
        } else if (entry.event == RP1_TRACE_GRAPH_DONE) {
            graphDone = entry.timestamp;
        } else if (entry.event == RP1_TRACE_FLUSH_START) {
            if (flushStart) {
                throw std::runtime_error(
                    "RP1 trace contains nested flush starts");
            }
            flushStart = entry.timestamp;
        } else if (entry.event == RP1_TRACE_FLUSH_END) {
            if (!flushStart) {
                throw std::runtime_error(
                    "RP1 trace flush end has no matching start");
            }
            flushes.emplace_back(*flushStart, entry.timestamp);
            flushStart.reset();
        }
    }

    if (!graphStart || !firstLaunch || !lastDone || !graphDone ||
        flushStart) {
        throw std::runtime_error(
            "RP1 trace is missing graph or kernel lifecycle events");
    }
    for (std::size_t i = 0; i < kernelCount; ++i) {
        if (!launches[i] || !completions[i]) {
            throw std::runtime_error(
                "RP1 trace is missing launch/completion for kernel node " +
                std::to_string(i));
        }
    }

    /*
     * Protocol timestamps are uint32_t PMU ticks. Unsigned subtraction keeps
     * intervals valid across one counter wrap.
     */
    const std::size_t adjacentCount =
        kernelCount > 0 ? kernelCount - 1 : 0;
    std::vector<std::uint32_t> launchGaps;
    std::vector<std::uint32_t> launchGapsExcludingFlush;
    std::vector<std::uint32_t> handoffGaps;
    std::vector<std::uint32_t> handoffGapsExcludingFlush;
    launchGaps.reserve(adjacentCount);
    launchGapsExcludingFlush.reserve(adjacentCount);
    handoffGaps.reserve(adjacentCount);
    handoffGapsExcludingFlush.reserve(adjacentCount);
    for (std::size_t i = 0; i + 1 < kernelCount; ++i) {
        if (!completions[i] || !launches[i + 1]) {
            throw std::runtime_error(
                "RP1 trace is missing an adjacent kernel handoff");
        }
        const std::uint32_t launchGap =
            *launches[i + 1] - *launches[i];
        const std::uint32_t gap =
            *launches[i + 1] - *completions[i];
        std::uint32_t launchFlushTicks = 0;
        std::uint32_t flushTicks = 0;
        for (const auto& [start, end] : flushes) {
            const std::uint32_t launchStartOffset =
                start - *launches[i];
            const std::uint32_t launchEndOffset =
                end - *launches[i];
            if (launchStartOffset <= launchGap &&
                launchEndOffset >= launchStartOffset &&
                launchEndOffset <= launchGap) {
                launchFlushTicks += end - start;
            }
            const std::uint32_t startOffset =
                start - *completions[i];
            const std::uint32_t endOffset =
                end - *completions[i];
            if (startOffset <= gap && endOffset >= startOffset &&
                endOffset <= gap) {
                flushTicks += end - start;
            }
        }
        if (launchFlushTicks > launchGap) {
            throw std::runtime_error(
                "RP1 trace flush exceeds its containing launch interval");
        }
        if (flushTicks > gap) {
            throw std::runtime_error(
                "RP1 trace flush exceeds its containing handoff");
        }
        launchGaps.push_back(launchGap);
        launchGapsExcludingFlush.push_back(
            launchGap - launchFlushTicks);
        handoffGaps.push_back(gap);
        handoffGapsExcludingFlush.push_back(gap - flushTicks);
    }

    std::vector<std::uint32_t> flushDurations;
    flushDurations.reserve(flushes.size());
    for (const auto& [start, end] : flushes) {
        flushDurations.push_back(end - start);
    }

    return TraceIntervals{
        static_cast<std::uint32_t>(*firstLaunch - *graphStart),
        static_cast<std::uint32_t>(*lastDone - *firstLaunch),
        static_cast<std::uint32_t>(*graphDone - *graphStart),
        std::move(launchGaps),
        std::move(launchGapsExcludingFlush),
        std::move(handoffGaps),
        std::move(handoffGapsExcludingFlush),
        std::move(flushDurations),
    };
}

/**
 * @brief Add PMU-tick samples and an optional 64-cycle tick conversion.
 */
void addTicks(
    std::vector<Measurement>& measurements, std::string name,
    std::vector<std::uint64_t> ticks,
    const std::optional<std::uint64_t>& r5FrequencyHz) {
    measurements.push_back(
        {name, "rp1_pmu_ticks", ticks});
    if (!r5FrequencyHz) return;

    std::vector<std::uint64_t> nanoseconds;
    nanoseconds.reserve(ticks.size());
    for (std::uint64_t tick : ticks) {
        const long double value =
            static_cast<long double>(tick) * 64.0L *
            1'000'000'000.0L /
            static_cast<long double>(*r5FrequencyHz);
        nanoseconds.push_back(
            static_cast<std::uint64_t>(std::llround(value)));
    }
    measurements.push_back(
        {std::move(name) + ".estimated", "ns", std::move(nanoseconds)});
}

/**
 * @brief Add core-cycle samples and an optional clock-derived conversion.
 */
void addCoreCycles(
    std::vector<Measurement>& measurements, std::string name,
    std::vector<std::uint64_t> cycles,
    const std::optional<std::uint64_t>& r5FrequencyHz) {
    measurements.push_back({name, "r5_cycles", cycles});
    if (!r5FrequencyHz) return;

    std::vector<std::uint64_t> nanoseconds;
    nanoseconds.reserve(cycles.size());
    for (std::uint64_t cycle : cycles) {
        const long double value =
            static_cast<long double>(cycle) * 1'000'000'000.0L /
            static_cast<long double>(*r5FrequencyHz);
        nanoseconds.push_back(
            static_cast<std::uint64_t>(std::llround(value)));
    }
    measurements.push_back(
        {std::move(name) + ".estimated", "ns", std::move(nanoseconds)});
}

/**
 * @brief Append 32-bit PMU intervals to an aggregate measurement vector.
 */
void appendTicks(
    std::vector<std::uint64_t>& destination,
    const std::vector<std::uint32_t>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

/**
 * @brief Return mean per-operation excess in core cycles over a paired chain.
 *
 * Aggregating before converting the divided PMU timestamps recovers
 * sub-80-nanosecond mean resolution without claiming per-access raw timing.
 */
std::uint64_t meanExcessCycles(
    const std::vector<std::uint32_t>& operation,
    const std::vector<std::uint32_t>& baseline) {
    if (operation.empty() || operation.size() != baseline.size()) {
        throw std::runtime_error(
            "RP1 memory trace gap vectors are incompatible");
    }
    std::uint64_t operationTicks = 0u;
    std::uint64_t baselineTicks = 0u;
    for (std::size_t i = 0u; i < operation.size(); ++i) {
        operationTicks += operation[i];
        baselineTicks += baseline[i];
    }
    if (operationTicks < baselineTicks) {
        throw std::runtime_error(
            "RP1 memory operation is faster than its NOP baseline");
    }
    const std::uint64_t excessTicks = operationTicks - baselineTicks;
    return (excessTicks * 64u + operation.size() / 2u) /
           operation.size();
}

/**
 * @brief Print one summary row per measurement in human or CSV form.
 */
void printMeasurements(
    const std::vector<Measurement>& measurements, bool csv) {
    if (csv) {
        std::cout << "metric,unit,count,min,p50,p95,p99,max,mean\n";
    } else {
        std::cout << std::left << std::setw(54) << "metric"
                  << std::right << std::setw(8) << "count"
                  << std::setw(14) << "min"
                  << std::setw(14) << "p50"
                  << std::setw(14) << "p95"
                  << std::setw(14) << "p99"
                  << std::setw(14) << "max"
                  << std::setw(16) << "mean"
                  << "  unit\n";
    }

    for (const Measurement& measurement : measurements) {
        const rp1_bench::Summary summary =
            rp1_bench::summarize(measurement.samples);
        if (csv) {
            std::cout << measurement.name << ',' << measurement.unit << ','
                      << summary.count << ',' << summary.minimum << ','
                      << summary.p50 << ',' << summary.p95 << ','
                      << summary.p99 << ',' << summary.maximum << ','
                      << std::fixed << std::setprecision(2)
                      << summary.mean << '\n';
        } else {
            std::cout << std::left << std::setw(54) << measurement.name
                      << std::right << std::setw(8) << summary.count
                      << std::setw(14) << summary.minimum
                      << std::setw(14) << summary.p50
                      << std::setw(14) << summary.p95
                      << std::setw(14) << summary.p99
                      << std::setw(14) << summary.maximum
                      << std::setw(16) << std::fixed << std::setprecision(2)
                      << summary.mean << "  " << measurement.unit << '\n';
        }
    }
}

/**
 * @brief Measure split legacy/RP1 backend calls and raw RP1 submissions.
 */
void benchmarkSingleKernel(
    const Config& config, vrt::Device& legacy,
    const std::shared_ptr<vrt::graph::FpgaDevice>& device,
    std::vector<Measurement>& measurements) {
    vrt::Kernel kernel(legacy, kKernelName);
    const SplitSamples vrtSamples = measureSplit(
        config.warmup, config.iterations,
        [&] { kernel.start(); }, [&] { kernel.wait(); });
    measurements.push_back(
        {"vrt.kernel.start", "ns", vrtSamples.launch});
    measurements.push_back(
        {"vrt.kernel.wait", "ns", vrtSamples.wait});
    measurements.push_back(
        {"vrt.kernel.start_wait", "ns", vrtSamples.total});

    const vrt::graph::Rp1QueueProgram program =
        makeKernelProgram(device->id(), kImageName, 1);
    {
        auto plan = device->compileProgram(program);
        const SplitSamples rp1Samples = measureSplit(
            config.warmup, config.iterations,
            [&] { plan->launch(); }, [&] { plan->wait(); });
        measurements.push_back(
            {"rp1.backend.launch_thread", "ns", rp1Samples.launch});
        measurements.push_back(
            {"rp1.backend.wait", "ns", rp1Samples.wait});
        measurements.push_back(
            {"rp1.backend.launch_wait", "ns", rp1Samples.total});
    }

    vrt::graph::fpga::Rp1GraphImage image =
        device->projectProgram(program);
    const auto submitter = device->submitter();
    for (std::size_t i = 0; i < config.warmup; ++i) {
        const vrt::graph::fpga::Rp1GraphResult result =
            submitter->submitAndWait(image, kRp1Timeout);
        validateGraphResult(
            result, image.nodes.size(), /*traceExpected=*/false);
    }

    std::vector<std::uint64_t> host;
    std::vector<std::uint64_t> resultGraph;
    host.reserve(config.iterations);
    resultGraph.reserve(config.iterations);
    for (std::size_t i = 0; i < config.iterations; ++i) {
        TimedGraphResult sample = timedSubmission(submitter, image);
        host.push_back(sample.elapsed);
        validateGraphResult(
            sample.result, image.nodes.size(), /*traceExpected=*/false);
        resultGraph.push_back(sample.result.graphElapsedTicks);
    }
    measurements.push_back(
        {"rp1.raw.kernel_graph.submit_wait", "ns", std::move(host)});
    addTicks(
        measurements, "rp1.result.kernel.graph_elapsed",
        std::move(resultGraph), config.r5FrequencyHz);

    image.trace_enable = true;
    image.trace_size_override = ringSize(
        rp1_bench::traceEntriesWithFlushMarkers(
            2u + image.nodes.size() + 2u),
        RP1_MAX_TRACE_ENTRIES);
    std::vector<std::uint64_t> dispatch;
    std::vector<std::uint64_t> execute;
    std::vector<std::uint64_t> graph;
    for (std::size_t i = 0; i < config.traceIterations; ++i) {
        const vrt::graph::fpga::Rp1GraphResult result =
            submitter->submitAndWait(image, kRp1Timeout);
        validateGraphResult(
            result, image.nodes.size(), /*traceExpected=*/true);
        const vrt::graph::fpga::Rp1TraceCapture trace =
            submitter->drainTrace();
        if (result.traceWriteIndex != trace.written) {
            throw std::runtime_error(
                "RP1 kernel result and trace cursor disagree");
        }
        const TraceIntervals intervals = extractTrace(trace, 1);
        dispatch.push_back(intervals.dispatch);
        execute.push_back(intervals.kernelSpan);
        graph.push_back(intervals.graph);
    }
    addTicks(
        measurements, "rp1.trace.kernel.dispatch", std::move(dispatch),
        config.r5FrequencyHz);
    addTicks(
        measurements, "rp1.trace.kernel.launch_to_done",
        std::move(execute), config.r5FrequencyHz);
    addTicks(
        measurements, "rp1.trace.kernel.graph", std::move(graph),
        config.r5FrequencyHz);
}

/**
 * @brief Compare sequential batches through one legacy CU and one RP1 graph.
 */
void benchmarkBatches(
    const Config& config, vrt::Device& legacy,
    const std::shared_ptr<vrt::graph::FpgaDevice>& device,
    std::vector<Measurement>& measurements) {
    vrt::Kernel kernel(legacy, kKernelName);
    const auto submitter = device->submitter();

    for (std::size_t batch : config.batchSizes) {
        const std::string suffix = std::to_string(batch);
        const auto runVrt = [&] {
            for (std::size_t i = 0; i < batch; ++i) {
                kernel.start();
                kernel.wait();
            }
        };
        for (std::size_t i = 0; i < config.warmup; ++i) runVrt();

        std::vector<std::uint64_t> vrtHost;
        vrtHost.reserve(config.iterations);
        for (std::size_t i = 0; i < config.iterations; ++i) {
            vrtHost.push_back(elapsedNs(runVrt));
        }
        measurements.push_back({
            "vrt.batch." + suffix + ".start_wait",
            "ns",
            std::move(vrtHost),
        });

        /*
         * Timestamp each returned start and wait call. Adjacent start
         * differences approximate host-issued ap_start to ap_start, while
         * wait(i) return to start(i+1) return matches RP1's observed-completion
         * to next-launch boundary.
         */
        std::vector<std::uint64_t> vrtStartGaps;
        std::vector<std::uint64_t> vrtHandoffGaps;
        vrtStartGaps.reserve(
            config.traceIterations * (batch > 0 ? batch - 1 : 0));
        vrtHandoffGaps.reserve(
            config.traceIterations * (batch > 0 ? batch - 1 : 0));
        for (std::size_t sample = 0;
             sample < config.traceIterations; ++sample) {
            std::optional<Clock::time_point> previousStart;
            std::optional<Clock::time_point> previousDone;
            for (std::size_t i = 0; i < batch; ++i) {
                kernel.start();
                const Clock::time_point started = Clock::now();
                if (previousStart) {
                    vrtStartGaps.push_back(
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                started - *previousStart)
                                .count()));
                }
                if (previousDone) {
                    vrtHandoffGaps.push_back(
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                started - *previousDone)
                                .count()));
                }
                previousStart = started;
                kernel.wait();
                previousDone = Clock::now();
            }
        }
        if (!vrtStartGaps.empty()) {
            measurements.push_back({
                "vrt.batch." + suffix + ".start_to_next_start",
                "ns",
                std::move(vrtStartGaps),
            });
        }
        if (!vrtHandoffGaps.empty()) {
            measurements.push_back({
                "vrt.batch." + suffix + ".done_to_next_start",
                "ns",
                std::move(vrtHandoffGaps),
            });
        }

        const vrt::graph::Rp1QueueProgram program =
            makeKernelProgram(device->id(), kImageName, batch);
        vrt::graph::fpga::Rp1GraphImage image =
            device->projectProgram(program);
        for (std::size_t i = 0; i < config.warmup; ++i) {
            const vrt::graph::fpga::Rp1GraphResult result =
                submitter->submitAndWait(image, kRp1Timeout);
            validateGraphResult(
                result, image.nodes.size(), /*traceExpected=*/false);
        }

        std::vector<std::uint64_t> rp1Host;
        std::vector<std::uint64_t> resultGraph;
        rp1Host.reserve(config.iterations);
        resultGraph.reserve(config.iterations);
        for (std::size_t i = 0; i < config.iterations; ++i) {
            TimedGraphResult sample = timedSubmission(submitter, image);
            rp1Host.push_back(sample.elapsed);
            validateGraphResult(
                sample.result, image.nodes.size(), /*traceExpected=*/false);
            resultGraph.push_back(sample.result.graphElapsedTicks);
        }
        measurements.push_back({
            "rp1.raw.batch." + suffix + ".submit_wait",
            "ns",
            std::move(rp1Host),
        });
        addTicks(
            measurements,
            "rp1.result.batch." + suffix + ".graph_elapsed",
            std::move(resultGraph), config.r5FrequencyHz);

        image.trace_enable = true;
        image.trace_size_override = ringSize(
            rp1_bench::traceEntriesWithFlushMarkers(
                2u + image.nodes.size() + 2u * batch),
            RP1_MAX_TRACE_ENTRIES);
        std::vector<std::uint64_t> dispatch;
        std::vector<std::uint64_t> kernelSpan;
        std::vector<std::uint64_t> graph;
        std::vector<std::uint64_t> launchGaps;
        std::vector<std::uint64_t> launchGapsExcludingFlush;
        std::vector<std::uint64_t> handoffGaps;
        std::vector<std::uint64_t> handoffGapsExcludingFlush;
        std::vector<std::uint64_t> flushDurations;
        for (std::size_t i = 0; i < config.traceIterations; ++i) {
            const vrt::graph::fpga::Rp1GraphResult result =
                submitter->submitAndWait(image, kRp1Timeout);
            validateGraphResult(
                result, image.nodes.size(), /*traceExpected=*/true);
            const vrt::graph::fpga::Rp1TraceCapture trace =
                submitter->drainTrace();
            if (result.traceWriteIndex != trace.written) {
                throw std::runtime_error(
                    "RP1 batch result and trace cursor disagree");
            }
            const TraceIntervals intervals =
                extractTrace(trace, batch);
            dispatch.push_back(intervals.dispatch);
            kernelSpan.push_back(intervals.kernelSpan);
            graph.push_back(intervals.graph);
            launchGaps.insert(
                launchGaps.end(),
                intervals.launchGaps.begin(),
                intervals.launchGaps.end());
            launchGapsExcludingFlush.insert(
                launchGapsExcludingFlush.end(),
                intervals.launchGapsExcludingFlush.begin(),
                intervals.launchGapsExcludingFlush.end());
            handoffGaps.insert(
                handoffGaps.end(),
                intervals.handoffGaps.begin(),
                intervals.handoffGaps.end());
            handoffGapsExcludingFlush.insert(
                handoffGapsExcludingFlush.end(),
                intervals.handoffGapsExcludingFlush.begin(),
                intervals.handoffGapsExcludingFlush.end());
            flushDurations.insert(
                flushDurations.end(),
                intervals.flushDurations.begin(),
                intervals.flushDurations.end());
        }
        addTicks(
            measurements, "rp1.trace.batch." + suffix + ".dispatch",
            std::move(dispatch), config.r5FrequencyHz);
        addTicks(
            measurements, "rp1.trace.batch." + suffix + ".kernel_span",
            std::move(kernelSpan), config.r5FrequencyHz);
        addTicks(
            measurements, "rp1.trace.batch." + suffix + ".graph",
            std::move(graph), config.r5FrequencyHz);
        if (!launchGaps.empty()) {
            addTicks(
                measurements,
                "rp1.trace.batch." + suffix +
                    ".launch_to_next_launch",
                std::move(launchGaps), config.r5FrequencyHz);
            addTicks(
                measurements,
                "rp1.trace.batch." + suffix +
                    ".launch_to_next_launch_excluding_flush",
                std::move(launchGapsExcludingFlush),
                config.r5FrequencyHz);
        }
        if (!handoffGaps.empty()) {
            addTicks(
                measurements,
                "rp1.trace.batch." + suffix +
                    ".done_to_next_launch",
                std::move(handoffGaps), config.r5FrequencyHz);
            addTicks(
                measurements,
                "rp1.trace.batch." + suffix +
                    ".done_to_next_launch_excluding_flush",
                std::move(handoffGapsExcludingFlush),
                config.r5FrequencyHz);
        }
        if (!flushDurations.empty()) {
            addTicks(
                measurements,
                "rp1.trace.batch." + suffix + ".trace_flush",
                std::move(flushDurations), config.r5FrequencyHz);
        }
    }
}

/**
 * @brief Measure differential RP1 DDR access latency with immediate chains.
 *
 * NOP, DMA_FILL, and DMA_COPY graphs share identical barrier topology and
 * trace boundaries. Per-submission aggregate subtraction removes scanner and
 * trace overhead, while COPY-minus-FILL approximates one additional DDR read.
 */
void benchmarkMemoryAccesses(
    const Config& config,
    const std::shared_ptr<vrt::graph::FpgaDevice>& device,
    std::vector<Measurement>& measurements) {
    const auto submitter = device->submitter();
    const auto window = device->window();

    for (std::size_t bytes : config.memorySizes) {
        const std::string prefix =
            "rp1.trace.memory." + std::to_string(bytes) + "B.";
        const auto nopImage = makeMemoryTraceImage(
            MemoryOperation::Nop, bytes, config.memoryChainLength);
        const auto fillImage = makeMemoryTraceImage(
            MemoryOperation::DmaFill, bytes, config.memoryChainLength);
        const auto copyImage = makeMemoryTraceImage(
            MemoryOperation::DmaCopy, bytes, config.memoryChainLength);

        std::vector<std::uint32_t> source(bytes / sizeof(std::uint32_t),
                                          kCopyPattern);
        std::vector<std::uint32_t> readback(source.size(), 0u);
        window->writeAt(
            kScratchSourceOffset, source.data(), bytes);
        window->zeroAt(kScratchDestinationOffset, bytes);

        for (std::size_t i = 0u; i < config.warmup; ++i) {
            (void)runMemoryTrace(submitter, nopImage);
            (void)runMemoryTrace(submitter, fillImage);
            (void)runMemoryTrace(submitter, copyImage);
        }

        std::vector<std::uint64_t> nopGaps;
        std::vector<std::uint64_t> fillGaps;
        std::vector<std::uint64_t> copyGaps;
        std::vector<std::uint64_t> fillExcess;
        std::vector<std::uint64_t> copyExcess;
        std::vector<std::uint64_t> readEstimate;
        const std::size_t gapSamples =
            config.traceIterations * (config.memoryChainLength - 1u);
        nopGaps.reserve(gapSamples);
        fillGaps.reserve(gapSamples);
        copyGaps.reserve(gapSamples);
        fillExcess.reserve(config.traceIterations);
        copyExcess.reserve(config.traceIterations);
        readEstimate.reserve(config.traceIterations);

        for (std::size_t i = 0u; i < config.traceIterations; ++i) {
            const std::vector<std::uint32_t> nop =
                runMemoryTrace(submitter, nopImage);
            const std::vector<std::uint32_t> fill =
                runMemoryTrace(submitter, fillImage);
            const std::vector<std::uint32_t> copy =
                runMemoryTrace(submitter, copyImage);
            appendTicks(nopGaps, nop);
            appendTicks(fillGaps, fill);
            appendTicks(copyGaps, copy);
            fillExcess.push_back(meanExcessCycles(fill, nop));
            copyExcess.push_back(meanExcessCycles(copy, nop));
            readEstimate.push_back(meanExcessCycles(copy, fill));
        }

        /*
         * Verify each mutating operation with untimed submissions so BAR reads
         * cannot perturb the paired timing sequence above.
         */
        (void)runMemoryTrace(submitter, fillImage);
        window->readAt(
            kScratchDestinationOffset, readback.data(), bytes);
        if (!std::all_of(
                readback.begin(), readback.end(),
                [](std::uint32_t value) {
                    return value == kFillPattern;
                })) {
            throw std::runtime_error(
                "RP1 DMA_FILL memory benchmark verification failed");
        }
        (void)runMemoryTrace(submitter, copyImage);
        window->readAt(
            kScratchDestinationOffset, readback.data(), bytes);
        if (readback != source) {
            throw std::runtime_error(
                "RP1 DMA_COPY memory benchmark verification failed");
        }

        addTicks(
            measurements, prefix + "nop_gap", std::move(nopGaps),
            config.r5FrequencyHz);
        addTicks(
            measurements, prefix + "dma_fill_gap", std::move(fillGaps),
            config.r5FrequencyHz);
        addTicks(
            measurements, prefix + "dma_copy_gap", std::move(copyGaps),
            config.r5FrequencyHz);
        addCoreCycles(
            measurements, prefix + "dma_fill_minus_nop",
            std::move(fillExcess), config.r5FrequencyHz);
        addCoreCycles(
            measurements, prefix + "dma_copy_minus_nop",
            std::move(copyExcess), config.r5FrequencyHz);
        addCoreCycles(
            measurements, prefix + "ddr_read_copy_minus_fill",
            std::move(readEstimate), config.r5FrequencyHz);
    }

    /*
     * Scalar operations have fixed four-byte payloads. Their excess metrics
     * retain the real SCALAR_READ signal publication rather than pretending it
     * is a pure load.
     */
    const auto nopImage = makeMemoryTraceImage(
        MemoryOperation::Nop, sizeof(std::uint32_t),
        config.memoryChainLength);
    const auto writeImage = makeMemoryTraceImage(
        MemoryOperation::ScalarWrite, sizeof(std::uint32_t),
        config.memoryChainLength);
    const auto readImage = makeMemoryTraceImage(
        MemoryOperation::ScalarRead, sizeof(std::uint32_t),
        config.memoryChainLength);
    window->writeAt(
        kScratchSourceOffset, &kCopyPattern, sizeof(kCopyPattern));
    window->zeroAt(
        kScratchDestinationOffset, sizeof(std::uint32_t));

    for (std::size_t i = 0u; i < config.warmup; ++i) {
        (void)runMemoryTrace(submitter, nopImage);
        (void)runMemoryTrace(submitter, writeImage);
        (void)runMemoryTrace(submitter, readImage);
    }

    std::vector<std::uint64_t> scalarNopGaps;
    std::vector<std::uint64_t> scalarWriteGaps;
    std::vector<std::uint64_t> scalarReadGaps;
    std::vector<std::uint64_t> scalarWriteExcess;
    std::vector<std::uint64_t> scalarReadExcess;
    for (std::size_t i = 0u; i < config.traceIterations; ++i) {
        const std::vector<std::uint32_t> nop =
            runMemoryTrace(submitter, nopImage);
        const std::vector<std::uint32_t> write =
            runMemoryTrace(submitter, writeImage);
        const std::vector<std::uint32_t> read =
            runMemoryTrace(submitter, readImage);
        appendTicks(scalarNopGaps, nop);
        appendTicks(scalarWriteGaps, write);
        appendTicks(scalarReadGaps, read);
        scalarWriteExcess.push_back(meanExcessCycles(write, nop));
        scalarReadExcess.push_back(meanExcessCycles(read, nop));
    }

    (void)runMemoryTrace(submitter, writeImage);
    std::uint32_t written = 0u;
    window->readAt(
        kScratchDestinationOffset, &written, sizeof(written));
    if (written != kFillPattern) {
        throw std::runtime_error(
            "RP1 SCALAR_WRITE memory benchmark verification failed");
    }
    (void)runMemoryTrace(submitter, readImage);
    rp1_signal_slot_t signal{};
    window->readSignal(kMemorySignalSlot, signal);
    if (signal.value != kCopyPattern ||
        signal.last_writer_node != config.memoryChainLength - 1u) {
        throw std::runtime_error(
            "RP1 SCALAR_READ memory benchmark verification failed");
    }

    addTicks(
        measurements, "rp1.trace.memory.scalar.nop_gap",
        std::move(scalarNopGaps), config.r5FrequencyHz);
    addTicks(
        measurements, "rp1.trace.memory.scalar.write_gap",
        std::move(scalarWriteGaps), config.r5FrequencyHz);
    addTicks(
        measurements, "rp1.trace.memory.scalar.read_gap",
        std::move(scalarReadGaps), config.r5FrequencyHz);
    addCoreCycles(
        measurements, "rp1.trace.memory.scalar.write_minus_nop",
        std::move(scalarWriteExcess), config.r5FrequencyHz);
    addCoreCycles(
        measurements, "rp1.trace.memory.scalar.read_minus_nop",
        std::move(scalarReadExcess), config.r5FrequencyHz);
}

/**
 * @brief Measure VRT host-device DMA and RP1 local DDR software copies.
 *
 * These transfer domains are deliberately reported as separate metrics:
 * phase-1 RP1 has no host/HBM DMA path, so a speedup ratio would be invalid.
 */
void benchmarkTransfers(
    const Config& config, vrt::Device& legacy,
    const std::shared_ptr<vrt::graph::FpgaDevice>& device,
    std::vector<Measurement>& measurements) {
    const auto submitter = device->submitter();
    const auto window = device->window();

    for (std::size_t bytes : config.transferSizes) {
        const std::string suffix = std::to_string(bytes) + "B";

        vrt::Buffer<std::uint8_t> buffer(
            legacy, bytes, vrt::MemoryRangeType::DDR);
        std::fill(buffer.get(), buffer.get() + bytes, std::uint8_t{0x5a});
        for (std::size_t i = 0; i < config.warmup; ++i) {
            buffer.sync(vrt::SyncType::HOST_TO_DEVICE);
            buffer.sync(vrt::SyncType::DEVICE_TO_HOST);
        }

        std::vector<std::uint64_t> h2d;
        std::vector<std::uint64_t> d2h;
        h2d.reserve(config.transferIterations);
        d2h.reserve(config.transferIterations);
        for (std::size_t i = 0; i < config.transferIterations; ++i) {
            h2d.push_back(elapsedNs([&] {
                buffer.sync(vrt::SyncType::HOST_TO_DEVICE);
            }));
            d2h.push_back(elapsedNs([&] {
                buffer.sync(vrt::SyncType::DEVICE_TO_HOST);
            }));
        }
        measurements.push_back({
            "vrt.transfer.logical_" + suffix + ".host_to_ddr",
            "ns",
            std::move(h2d),
        });
        measurements.push_back({
            "vrt.transfer.logical_" + suffix + ".ddr_to_host",
            "ns",
            std::move(d2h),
        });

        std::vector<std::uint8_t> source(bytes, 0xa5);
        std::vector<std::uint8_t> destination(bytes, 0);
        window->writeAt(
            kScratchSourceOffset, source.data(), source.size());
        window->zeroAt(kScratchDestinationOffset, bytes);

        vrt::graph::fpga::Rp1GraphImage image = makeDmaImage(bytes);
        for (std::size_t i = 0; i < config.warmup; ++i) {
            const vrt::graph::fpga::Rp1GraphResult result =
                submitter->submitAndWait(image, kRp1Timeout);
            validateGraphResult(
                result, image.nodes.size(), /*traceExpected=*/false);
        }

        std::vector<std::uint64_t> rp1Host;
        std::vector<std::uint64_t> rp1Device;
        rp1Host.reserve(config.transferIterations);
        rp1Device.reserve(config.transferIterations);
        for (std::size_t i = 0; i < config.transferIterations; ++i) {
            /*
             * Alternate the payload so every timed command must overwrite the
             * prior destination. BAR staging and verification remain untimed.
             */
            std::fill(
                source.begin(), source.end(),
                (i & 1u) == 0u ? std::uint8_t{0x5a}
                               : std::uint8_t{0xa5});
            window->writeAt(
                kScratchSourceOffset, source.data(), source.size());
            TimedGraphResult sample = timedSubmission(submitter, image);
            rp1Host.push_back(sample.elapsed);
            validateGraphResult(
                sample.result, image.nodes.size(), /*traceExpected=*/false);
            rp1Device.push_back(sample.result.graphElapsedTicks);
            window->readAt(
                kScratchDestinationOffset,
                destination.data(), destination.size());
            if (destination != source) {
                throw std::runtime_error(
                    "RP1 DMA_COPY verification failed for " +
                    std::to_string(bytes) + " bytes at iteration " +
                    std::to_string(i));
            }
        }

        measurements.push_back({
            "rp1.transfer." + suffix + ".ddr_copy_host_round_trip",
            "ns",
            std::move(rp1Host),
        });
        addTicks(
            measurements,
            "rp1.transfer." + suffix + ".graph_elapsed",
            std::move(rp1Device), config.r5FrequencyHz);
    }
}

}  // namespace

#ifndef RP1_LATENCY_TESTING
/**
 * @brief Run all latency microbenchmarks and print aggregate summaries.
 */
int main(int argc, char** argv) try {
    const Config config = parseArgs(argc, argv);
    if (::setenv("VRTD_SOCKET", config.socket.c_str(), 1) != 0) {
        throw std::runtime_error("failed to set VRTD_SOCKET");
    }
    vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::WARN);

    std::vector<Measurement> measurements;

    vrt::Device legacy;
    const std::uint64_t vrtSetup = elapsedNs([&] {
        legacy = vrt::Device(
            config.bdf, config.vbin, /*program=*/false);
    });
    measurements.push_back(
        {"vrt.setup.no_program", "ns", {vrtSetup}});

    /*
     * Compare the legacy programming RPC before constructing the RP1 path.
     * addFpga() then revalidates firmware and obtains a fresh BAR mapping.
     */
    std::vector<std::uint64_t> vrtPrograms;
    vrtPrograms.reserve(config.programIterations);
    for (std::size_t i = 0; i < config.programIterations; ++i) {
        vrtPrograms.push_back(elapsedNs(
            [&] { legacy.getHandle()->programDevice(); }));
    }
    measurements.push_back(
        {"vrt.program.design_write_reset", "ns", std::move(vrtPrograms)});

    vrt::graph::Graph graph = vrt::graph::Graph::withDefaults();
    vrt::graph::FpgaHandle fpga;
    const std::uint64_t rp1Setup = elapsedNs([&] {
        vrt::graph::FpgaSpec spec;
        spec.bdf = config.bdf;
        spec.socket = config.socket;
        spec.images.push_back({kImageName, config.vbin});
        spec.waitTimeout = kRp1Timeout;
        fpga = graph.addFpga(spec);
    });
    measurements.push_back(
        {"rp1.setup.add_fpga", "ns", {rp1Setup}});

    const vrt::graph::FpgaImageHandle image =
        fpga.image(kImageName);
    const auto device = fpga.device();

    /*
     * Program through RP1 after the legacy samples. This both measures
     * PDI_LOAD and synchronizes firmware's expected-image guard.
     */
    auto programPlan = device->compileProgram(
        makeReprogramProgram(device->id(), image));
    std::vector<std::uint64_t> rp1Programs;
    rp1Programs.reserve(config.programIterations);
    for (std::size_t i = 0; i < config.programIterations; ++i) {
        rp1Programs.push_back(elapsedNs([&] {
            programPlan->launch();
            programPlan->wait();
        }));
    }
    measurements.push_back({
        "rp1.program.first_with_staging",
        "ns",
        {rp1Programs.front()},
    });
    if (rp1Programs.size() > 1) {
        measurements.push_back({
            "rp1.program.cached_pdi",
            "ns",
            std::vector<std::uint64_t>(
                rp1Programs.begin() + 1, rp1Programs.end()),
        });
    }
    programPlan.reset();

    benchmarkSingleKernel(config, legacy, device, measurements);
    benchmarkBatches(config, legacy, device, measurements);
    benchmarkMemoryAccesses(config, device, measurements);
    benchmarkTransfers(config, legacy, device, measurements);

    printMeasurements(measurements, config.csv);
    return 0;
} catch (const std::exception& error) {
    std::cerr << "rp1_latency: " << error.what() << '\n';
    return 1;
}
#endif
