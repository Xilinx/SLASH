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

#include <vrt/graph/device/fpga_device.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/device/fpga/control_lowering.hpp>
#include <vrt/graph/device/fpga/vbin_spec.hpp>
#include <vrt/graph/node/compiled_node.hpp>
#include <vrt/graph/node/io_type_map.hpp>

namespace vrt::graph {

namespace {

constexpr std::chrono::seconds kBridgeWaitTimeout{35};

// One bucket = 32 bits.  Bit 31 is reserved for the sentinel.
constexpr std::uint32_t kBarrierBitsPerBucket = 32u;
constexpr std::uint8_t  kSentinelBucket       = 0u;
constexpr std::uint32_t kSentinelBit          = 1u << 31;
constexpr std::uint32_t kKernelBitsPerBucket  = 31u;
constexpr std::uint32_t kArgBufferWords =
    (RP1_DEFAULT_SIG_ARRAY_OFFSET - RP1_DEFAULT_ARG_BUF_OFFSET) / sizeof(std::uint32_t);

constexpr std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

constexpr std::uint32_t kBufferArenaStart =
    alignUp(RP1_DEFAULT_SIG_ARRAY_OFFSET +
                RP1_MAX_SIGNALS * sizeof(rp1_signal_slot_t),
            4096u);

/// How many 32-bit words does a scalar bit pattern occupy in the
/// argument buffer?  HLS AXI-Lite arg registers are 32-bit; wider
/// values use consecutive registers in little-endian order.
std::uint32_t scalarWidthInWords(ScalarType t) {
    switch (t) {
        case ScalarType::U8: case ScalarType::U16: case ScalarType::U32:
        case ScalarType::I8: case ScalarType::I16: case ScalarType::I32:
        case ScalarType::F32:
            return 1u;
        case ScalarType::U64: case ScalarType::I64: case ScalarType::F64:
            return 2u;
    }
    return 1u;
}

/// Zero-/sign-extend a value's raw bits to a sequence of 32-bit words.
/// Output @p dst must point to at least scalarWidthInWords(type) entries.
void writeScalarToArgWords(ScalarType type, std::uint64_t bits, std::uint32_t* dst) {
    switch (type) {
        case ScalarType::U8: {
            std::uint8_t v;
            std::memcpy(&v, &bits, sizeof(v));
            dst[0] = static_cast<std::uint32_t>(v);
            return;
        }
        case ScalarType::U16: {
            std::uint16_t v;
            std::memcpy(&v, &bits, sizeof(v));
            dst[0] = static_cast<std::uint32_t>(v);
            return;
        }
        case ScalarType::U32:
        case ScalarType::F32: {
            std::uint32_t v;
            std::memcpy(&v, &bits, sizeof(v));
            dst[0] = v;
            return;
        }
        case ScalarType::I8: {
            std::int8_t v;
            std::memcpy(&v, &bits, sizeof(v));
            // Sign-extend to int32, then reinterpret to uint32 word.
            const std::int32_t e = v;
            std::memcpy(&dst[0], &e, sizeof(e));
            return;
        }
        case ScalarType::I16: {
            std::int16_t v;
            std::memcpy(&v, &bits, sizeof(v));
            const std::int32_t e = v;
            std::memcpy(&dst[0], &e, sizeof(e));
            return;
        }
        case ScalarType::I32: {
            std::int32_t v;
            std::memcpy(&v, &bits, sizeof(v));
            std::memcpy(&dst[0], &v, sizeof(v));
            return;
        }
        case ScalarType::U64:
        case ScalarType::I64:
        case ScalarType::F64: {
            std::uint64_t v;
            std::memcpy(&v, &bits, sizeof(v));
            dst[0] = static_cast<std::uint32_t>(v & 0xFFFFFFFFu);
            dst[1] = static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu);
            return;
        }
    }
}

void writeU64ToArgWords(std::uint64_t value, std::uint32_t* dst) {
    dst[0] = static_cast<std::uint32_t>(value & 0xFFFFFFFFu);
    dst[1] = static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFu);
}

std::int64_t signedScalarValue(ScalarType type, std::uint64_t bits) {
    switch (type) {
        case ScalarType::I8: {
            std::int8_t v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        case ScalarType::I16: {
            std::int16_t v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        case ScalarType::I32: {
            std::int32_t v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        case ScalarType::I64: {
            std::int64_t v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        default:
            return static_cast<std::int64_t>(bits);
    }
}

std::uint64_t unsignedScalarValue(ScalarType type, std::uint64_t bits) {
    switch (type) {
        case ScalarType::U8: {
            std::uint8_t v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        case ScalarType::U16: {
            std::uint16_t v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        case ScalarType::U32: {
            std::uint32_t v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        case ScalarType::U64: {
            std::uint64_t v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        default:
            return bits;
    }
}

void ensureArgCapacity(std::uint32_t cursor_words,
                       std::uint32_t width,
                       const std::string& kernelName,
                       const std::string& portName) {
    const std::uint64_t total = static_cast<std::uint64_t>(cursor_words) + width;
    if (total > kArgBufferWords) {
        throw std::logic_error(
            "FpgaDevice: argument buffer overflow while packing kernel '" +
            kernelName + "' port '" + portName + "'");
    }
}

/// Default AXI-Lite register offset of the first argument when no system_map
/// is available (mock/lookup path).  Matches HLS' conventional AP-block base
/// and the historical contiguous layout.
constexpr std::uint32_t kApArgBlockBase = 0x10u;

/// Resolves the AXI-Lite register byte offset for each kernel port.
///
/// When constructed from a non-empty `system_map` offset table (real vbin),
/// every port must be present and its offset is honoured exactly.  When the
/// table is empty (mock/lookup path) offsets are handed out contiguously from
/// `kApArgBlockBase`, reproducing the historical dense layout.
class ArgLayout {
   public:
    ArgLayout(std::map<std::string, std::uint32_t> offsets,
              std::string kernelName)
        : offsets_(std::move(offsets)),
          kernelName_(std::move(kernelName)),
          haveSpec_(!offsets_.empty()) {}

    /// Returns the base register offset for a port occupying @p words 32-bit
    /// words.  On the contiguous path this also advances the running cursor.
    std::uint32_t take(const std::string& port, std::uint32_t words) {
        if (haveSpec_) {
            return baseFor(port);
        }
        const std::uint32_t base = fallback_;
        fallback_ += words * 4u;
        return base;
    }

   private:
    std::uint32_t baseFor(const std::string& port) const {
        auto it = offsets_.find(port);
        if (it != offsets_.end()) {
            return it->second;
        }
        // An RW buffer's synthetic "<name>_out" port shares the single HLS
        // pointer register named "<name>".  (Current limitation: in and out
        // addresses are both written to that one register; tracked for a
        // future protocol revision that distinguishes them.)
        if (port.size() > 4 &&
            port.compare(port.size() - 4, 4, "_out") == 0) {
            auto in = offsets_.find(port.substr(0, port.size() - 4));
            if (in != offsets_.end()) {
                return in->second;
            }
        }
        throw std::runtime_error(
            "FpgaDevice: kernel '" + kernelName_ + "' port '" + port +
            "' has no s_axilite register offset in the system_map");
    }

    std::map<std::string, std::uint32_t> offsets_;
    std::string                          kernelName_;
    bool                                 haveSpec_;
    std::uint32_t                        fallback_ = kApArgBlockBase;
};

/// Appends @p width (reg_offset, value) pairs to @p arg_buf, one per 32-bit
/// value word, with register offset `base_offset + 4*w`.  Returns the arg_buf
/// word index of the first *value* word (every other word from there, because
/// the pairs interleave offset/value) — used to patch deferred scalars later.
std::uint32_t appendArgWordsAsPairs(std::vector<std::uint32_t>& arg_buf,
                                    std::uint32_t& cursor_words,
                                    std::uint32_t base_offset,
                                    const std::uint32_t* words,
                                    std::uint32_t width,
                                    const std::string& kernelName,
                                    const std::string& portName) {
    ensureArgCapacity(cursor_words, width * 2u, kernelName, portName);
    arg_buf.resize(cursor_words + width * 2u, 0u);
    const std::uint32_t firstValueWord = cursor_words + 1u;
    for (std::uint32_t w = 0; w < width; ++w) {
        arg_buf[cursor_words + 2u * w]      = base_offset + 4u * w;
        arg_buf[cursor_words + 2u * w + 1u] = words[w];
    }
    cursor_words += width * 2u;
    return firstValueWord;
}

/// Bookkeeping for a single global-scalar binding that must be
/// re-read from the per-graph scalar map at launch time.
struct DeferredScalar {
    std::string  scopedKey;          // scope:N:varName, with N=scopeId
    std::string  fallbackKey;        // bare varName (used iff scopeId==0)
    bool         hasFallback;
    ScalarType   type;
    std::uint32_t arg_word_offset;   // arg_buf index of the first *value* word
                                     // of this scalar's (offset,value) pairs;
                                     // subsequent words are at +2 each.
    std::string  diagnostic;         // "<kernelId>.<portName>"
};

/// Bookkeeping for a PDI_LOAD node whose PDI bytes are staged at launch time.
struct DeferredPdi {
    std::size_t  nodeIndex = 0;
    std::string  imageId;
    std::string  pdiPath;
};

struct DeferredLoopTripCount {
    std::size_t nodeIndex = 0;
    std::string scopedKey;
    std::string fallbackKey;
    bool hasFallback = false;
    ScalarType type = ScalarType::U32;
    std::string diagnostic;
};

struct DeferredBufferAddress {
    GraphBuffer   buffer;
    std::uint32_t arg_word_offset = 0;  // first value word for the 64-bit address
    std::string   diagnostic;
};

struct DeferredBufferAlias {
    std::string sourceKey;
    std::string targetKey;
    GraphBuffer sizeToken;
    std::string diagnostic;
};

const char* deviceTypeName(DeviceType t) {
    switch (t) {
        case DeviceType::CPU:      return "CPU";
        case DeviceType::GPU:      return "GPU";
        case DeviceType::FPGA:     return "FPGA";
        case DeviceType::MOCK_CPU: return "MOCK_CPU";
    }
    return "?";
}

}  // namespace

// =========================================================================
// FpgaDevicePlan
// =========================================================================

class FpgaDevicePlan : public IDevicePlan {
   public:
    FpgaDevicePlan(std::shared_ptr<fpga::Rp1Submitter>           submitter,
                   fpga::Rp1GraphImage                            image,
                   std::vector<DeferredScalar>                    deferred,
                   std::shared_ptr<std::map<std::string, std::uint64_t>> scalarValues,
                   std::uint32_t                                  sentinelSlot,
                   std::uint32_t                                  sentinelValue,
                   std::chrono::milliseconds                      timeout)
        : submitter_(std::move(submitter)),
          image_(std::move(image)),
          deferred_(std::move(deferred)),
          scalarValues_(std::move(scalarValues)),
          sentinelSlot_(sentinelSlot),
          sentinelValue_(sentinelValue),
          timeout_(timeout) {}

    FpgaDevicePlan(FpgaDevice&                                     device,
                   const DGraph&                                   dg,
                   std::shared_ptr<std::map<std::string, std::uint64_t>> scalarValues,
                   std::uint32_t                                   sentinelSlot,
                   std::uint32_t                                   sentinelValue,
                   std::chrono::milliseconds                       timeout)
        : device_(&device),
          submitter_(device.submitter_),
          scalarValues_(std::move(scalarValues)),
          sentinelSlot_(sentinelSlot),
          sentinelValue_(sentinelValue),
          timeout_(timeout) {
        image_ = dgraphHasControl(dg) ? buildControlImage(dg) : buildMainlineImage(dg);
    }

    static bool dgraphHasControl(const DGraph& dg) {
        for (const CompiledNode& node : dg.nodes) {
            if (std::holds_alternative<CompiledLoopNode>(node) ||
                std::holds_alternative<CompiledConditionalNode>(node)) {
                return true;
            }
        }
        return false;
    }

    ~FpgaDevicePlan() override {
        try { wait(); } catch (...) { /* swallow */ }
    }

    static void dumpImage(const fpga::Rp1GraphImage& image) {
        auto opName = [](std::uint16_t op) -> const char* {
            switch (op) {
                case RP1_OP_WAIT:            return "WAIT";
                case RP1_OP_SIGNAL:          return "SIGNAL";
                case RP1_OP_SCALAR_READ:     return "SCALAR_READ";
                case RP1_OP_SCALAR_COPY:     return "SCALAR_COPY";
                case RP1_OP_KERNEL_DISPATCH: return "KERNEL_DISPATCH";
                case RP1_OP_DMA_COPY:        return "DMA_COPY";
                case RP1_OP_PDI_LOAD:        return "PDI_LOAD";
                case RP1_OP_LOOP:            return "LOOP";
                case RP1_OP_COND:            return "COND";
                case RP1_OP_RERUN:           return "RERUN";
                default:                     return "?";
            }
        };
        std::cerr << "[rp1-dump] " << image.nodes.size() << " nodes\n";
        for (std::size_t i = 0; i < image.nodes.size(); ++i) {
            const rp1_node_t& n = image.nodes[i];
            std::cerr << "[rp1-dump] #" << i << " " << opName(n.opcode)
                      << " await(b" << int(n.barrier_await_bucket) << ":0x"
                      << std::hex << n.barrier_await_mask << std::dec << ")"
                      << " set(b" << int(n.barrier_set_bucket) << ":0x"
                      << std::hex << n.barrier_set_mask << std::dec << ")";
            if (n.opcode == RP1_OP_WAIT) {
                std::cerr << " wait[sig=" << n.payload.wait.condition_signal
                          << " op=" << n.payload.wait.condition_op
                          << " val=" << n.payload.wait.condition_value << "]";
            } else if (n.opcode == RP1_OP_SIGNAL) {
                std::cerr << " sig[slot=" << n.payload.signal.target_slot
                          << " op=" << n.payload.signal.operation
                          << " val=" << n.payload.signal.value << "]";
            } else if (n.opcode == RP1_OP_LOOP) {
                std::cerr << " loop[body=" << n.payload.loop.body_start << ".."
                          << n.payload.loop.body_end
                          << " maxIter=" << n.payload.loop.max_iterations
                          << " condSig=" << n.payload.loop.condition_signal
                          << " condVal=" << n.payload.loop.condition_value
                          << " condOp=" << n.payload.loop.condition_op
                          << " clearB=" << int(n.payload.loop.bucket_clear_start) << ".."
                          << int(n.payload.loop.bucket_clear_end) << "]";
            } else if (n.opcode == RP1_OP_PDI_LOAD) {
                std::cerr << " pdi[img=" << n.payload.pdi_load.image_id << "]";
            } else if (n.opcode == RP1_OP_KERNEL_DISPATCH) {
                std::cerr << " kd[img=" << n.payload.kernel_dispatch.expected_image_id << "]";
            } else if (n.opcode == RP1_OP_SCALAR_COPY) {
                std::cerr << " scopy[srcSlot=" << n.payload.scalar_copy.source_slot << "]";
            } else if (n.opcode == RP1_OP_SCALAR_READ) {
                std::cerr << " sread[slot=" << n.payload.scalar_read.target_slot << "]";
            }
            std::cerr << "\n";
        }
        std::cerr << std::flush;
    }

    static void clearHandshakeSlots(fpga::Rp1GraphImage& image) {
        std::set<std::uint32_t> carried;
        for (const rp1_node_t& n : image.nodes) {
            if (n.opcode == RP1_OP_SCALAR_COPY) {
                carried.insert(n.payload.scalar_copy.source_slot);
            }
        }

        std::set<std::uint32_t> toClear;
        for (const rp1_node_t& n : image.nodes) {
            if (n.opcode != RP1_OP_LOOP) continue;
            toClear.insert(n.payload.loop.condition_signal);
            const std::uint32_t bs = n.payload.loop.body_start;
            const std::uint32_t be = n.payload.loop.body_end;
            for (std::uint32_t i = bs; i <= be && i < image.nodes.size(); ++i) {
                const rp1_node_t& b = image.nodes[i];
                if (b.opcode == RP1_OP_SIGNAL) {
                    toClear.insert(b.payload.signal.target_slot);
                } else if (b.opcode == RP1_OP_WAIT) {
                    toClear.insert(b.payload.wait.condition_signal);
                }
            }
        }
        for (std::uint32_t s : toClear) {
            if (carried.count(s)) continue;
            if (std::find(image.clear_signal_slots.begin(),
                          image.clear_signal_slots.end(), s) ==
                image.clear_signal_slots.end()) {
                image.clear_signal_slots.push_back(s);
            }
        }
    }

    void prepareLaunch() override {
        wait();
        submitter_->clearSignalSlots(image_.clear_signal_slots);
        signalsPrepared_ = true;
    }

    void launch() override {
        wait();
        if (std::getenv("VRT_RP1_DUMP")) dumpImage(image_);
        workerEx_ = nullptr;
        worker_ = std::thread([this] {
            try {
                lastCq_.clear();
                resolveDeferredScalars();
                resolveDeferredLoopTripCounts();
                stageDeferredPdis();
                resolveDeferredBufferAliases();
                resolveDeferredBufferAddresses();
                const bool signalsPrepared = signalsPrepared_;
                signalsPrepared_ = false;
                fpga::Rp1GraphImage submitImage = image_;
                if (signalsPrepared) submitImage.clear_signal_slots.clear();
                submitter_->submitAndWait(submitImage, timeout_);
                lastCq_ = submitter_->drainCq();
                applyImageSideEffects();
            } catch (...) {
                workerEx_ = std::current_exception();
                try {
                    std::rethrow_exception(workerEx_);
                } catch (const std::exception& e) {
                    std::cerr << "[FpgaDevicePlan] worker exception: " << e.what()
                              << std::endl;
                } catch (...) {
                    std::cerr << "[FpgaDevicePlan] worker exception: (non-std)"
                              << std::endl;
                }
            }
        });
    }

    void wait() override {
        if (worker_.joinable()) worker_.join();
        if (workerEx_) {
            std::exception_ptr ex = workerEx_;
            workerEx_ = nullptr;
            std::rethrow_exception(ex);
        }
    }

    const std::vector<rp1_cq_entry_t>& lastCq() const noexcept { return lastCq_; }
    std::uint32_t sentinelSlot()  const noexcept { return sentinelSlot_; }
    std::uint32_t sentinelValue() const noexcept { return sentinelValue_; }
    const fpga::Rp1GraphImage& image() const noexcept { return image_; }

   private:
    void applyImageSideEffects() {
        if (!device_) return;
        for (const DeferredPdi& pdi : deferredPdis_) {
            device_->setActiveImage(pdi.imageId);
        }
    }

    void resolveDeferredScalars() {
        if (deferred_.empty()) return;
        if (!scalarValues_) {
            throw std::runtime_error(
                "FpgaDevicePlan: deferred scalar resolution requires a scalar map "
                "(DGraph::scalarValues was null at compile time)");
        }
        for (const DeferredScalar& d : deferred_) {
            auto it = scalarValues_->find(d.scopedKey);
            if (it == scalarValues_->end() && d.hasFallback) {
                it = scalarValues_->find(d.fallbackKey);
            }
            if (it == scalarValues_->end()) {
                throw std::runtime_error(
                    "FpgaDevicePlan: global scalar bound to port '" + d.diagnostic +
                    "' is not set in the graph scalar map (key='" + d.scopedKey + "')");
            }
            // Values live in the odd (value) words of the interleaved
            // (reg_offset, value) pairs, so scatter them at a stride of 2.
            std::uint32_t words[2] = {0u, 0u};
            writeScalarToArgWords(d.type, it->second, words);
            const std::uint32_t width = scalarWidthInWords(d.type);
            for (std::uint32_t w = 0; w < width; ++w) {
                image_.arg_buf[d.arg_word_offset + 2u * w] = words[w];
            }
        }
    }

    void resolveDeferredLoopTripCounts() {
        if (deferredTripCounts_.empty()) return;
        if (!scalarValues_) {
            throw std::runtime_error(
                "FpgaDevicePlan: deferred loop trip-count resolution requires a scalar map");
        }
        for (const DeferredLoopTripCount& d : deferredTripCounts_) {
            auto it = scalarValues_->find(d.scopedKey);
            if (it == scalarValues_->end() && d.hasFallback) {
                it = scalarValues_->find(d.fallbackKey);
            }
            if (it == scalarValues_->end()) {
                throw std::runtime_error(
                    "FpgaDevicePlan: loop trip-count scalar '" + d.diagnostic +
                    "' is not set in the graph scalar map (key='" + d.scopedKey + "')");
            }
            const std::uint64_t value =
                isSignedIntegerScalarType(d.type)
                    ? static_cast<std::uint64_t>(signedScalarValue(d.type, it->second))
                    : unsignedScalarValue(d.type, it->second);
            if (isSignedIntegerScalarType(d.type) &&
                signedScalarValue(d.type, it->second) < 0) {
                throw std::runtime_error(
                    "FpgaDevicePlan: loop trip count '" + d.diagnostic +
                    "' must be non-negative");
            }
            if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error(
                    "FpgaDevicePlan: loop trip count '" + d.diagnostic +
                    "' must be in the range 1..UINT32_MAX");
            }
            if (d.nodeIndex >= image_.nodes.size() ||
                image_.nodes[d.nodeIndex].opcode != RP1_OP_LOOP) {
                throw std::logic_error(
                    "FpgaDevicePlan: deferred trip count points at a non-LOOP node");
            }
            image_.nodes[d.nodeIndex].payload.loop.max_iterations =
                static_cast<std::uint32_t>(value);
        }
    }

    void resolveDeferredBufferAliases() {
        if (deferredBufferAliases_.empty()) return;
        if (!device_) {
            throw std::runtime_error(
                "FpgaDevicePlan: deferred buffer aliases require an FpgaDevice");
        }
        for (const DeferredBufferAlias& alias : deferredBufferAliases_) {
            const std::size_t bytes =
                resolvedBufferSizeBytes(alias.sizeToken, scalarValues_, "FpgaDevicePlan");
            if (device_->bufferSize(alias.sourceKey) == 0) {
                device_->setInputBuffer(alias.sourceKey, nullptr, bytes);
            }
            device_->aliasBufferKey(alias.targetKey, alias.sourceKey);
        }
    }

    void resolveDeferredBufferAddresses() {
        if (deferredBufferAddresses_.empty()) return;
        if (!device_) {
            throw std::runtime_error(
                "FpgaDevicePlan: deferred buffer address resolution requires an FpgaDevice");
        }
        for (const DeferredBufferAddress& d : deferredBufferAddresses_) {
            const std::size_t bytes =
                resolvedBufferSizeBytes(d.buffer, scalarValues_, "FpgaDevicePlan");
            const std::uint64_t addr = device_->bufferDeviceAddress(d.buffer, bytes);
            std::uint32_t words[2] = {0u, 0u};
            writeU64ToArgWords(addr, words);
            image_.arg_buf[d.arg_word_offset] = words[0];
            image_.arg_buf[d.arg_word_offset + 2u] = words[1];
        }
    }

    void stageDeferredPdis() {
        if (deferredPdis_.empty()) return;
        std::map<std::string, std::uint64_t> stagedByPath;
        for (const DeferredPdi& d : deferredPdis_) {
            const std::string key = d.imageId + "\n" + d.pdiPath;
            auto it = stagedByPath.find(key);
            if (it == stagedByPath.end()) {
                CompiledReprogramNode node;
                node.imageId = d.imageId;
                node.pdiPath = d.pdiPath;
                it = stagedByPath.emplace(key, stagePdi(node)).first;
            }
            if (d.nodeIndex >= image_.nodes.size() ||
                image_.nodes[d.nodeIndex].opcode != RP1_OP_PDI_LOAD) {
                throw std::logic_error(
                    "FpgaDevicePlan: deferred PDI fixup points at a non-PDI_LOAD node");
            }
            auto& payload = image_.nodes[d.nodeIndex].payload.pdi_load;
            payload.pdi_addr_lo = static_cast<std::uint32_t>(it->second & 0xFFFFFFFFull);
            payload.pdi_addr_hi = static_cast<std::uint32_t>((it->second >> 32) & 0xFFFFFFFFull);
        }
    }

    std::uint64_t scalarBits(const GraphScalar& gs, const std::string& diagnostic) const {
        if (!scalarValues_) {
            throw std::runtime_error(
                "FpgaDevicePlan: scalar bound to port '" + diagnostic +
                "' requires a scalar map");
        }
        const std::string scopedKey = scopedScalarKey(gs.scopeId(), gs.varName());
        auto it = scalarValues_->find(scopedKey);
        if (it == scalarValues_->end() && gs.scopeId() == 0) {
            it = scalarValues_->find(gs.varName());
        }
        if (it == scalarValues_->end()) {
            throw std::runtime_error(
                "FpgaDevicePlan: global scalar bound to port '" + diagnostic +
                "' is not set in the graph scalar map (key='" + scopedKey + "')");
        }
        return it->second;
    }

    std::size_t currentBufferSize(const GraphBuffer& buffer) const {
        return resolvedBufferSizeBytes(buffer, scalarValues_, "FpgaDevicePlan");
    }

    void appendBufferAddress(fpga::Rp1GraphImage& image,
                             const CompiledKernelNode& node,
                             ArgLayout& layout,
                             const std::string& portName,
                             const GraphBuffer& buffer,
                             std::size_t sizeBytes,
                             std::uint32_t& cursor_words,
                             std::uint32_t& arg_count) {
        std::uint32_t words[2] = {0u, 0u};
        const std::uint32_t base = layout.take(portName, 2u);
        const std::uint32_t firstValueWord =
            appendArgWordsAsPairs(image.arg_buf, cursor_words, base, words, 2u,
                                  node.kernel.name, portName);
        (void)sizeBytes;
        deferredBufferAddresses_.push_back(DeferredBufferAddress{
            buffer,
            firstValueWord,
            node.id + "." + portName});
        arg_count += 2u;
    }

    std::uint32_t packKernelArgs(fpga::Rp1GraphImage& image,
                                 const CompiledKernelNode& node,
                                 const std::set<std::string>& skipInputScalars = {}) {
        std::uint32_t cursor_words = static_cast<std::uint32_t>(image.arg_buf.size());
        std::uint32_t arg_count = 0;

        ArgLayout layout(device_->kernelArgOffsets(node.kernel), node.kernel.name);

        for (const ScalarPort& port : node.kernel.ioType.inputScalars) {
            // A loop-carried scalar input is fed each iteration by a SCALAR_COPY
            // from its signal slot into this register, not by a static arg.
            if (skipInputScalars.count(port.name)) continue;
            auto it = node.ioMap.inputScalars().find(port.name);
            if (it == node.ioMap.inputScalars().end()) {
                throw std::runtime_error(
                    "FpgaDevice: kernel '" + node.kernel.name +
                    "' input scalar port '" + port.name + "' has no IOMap binding");
            }
            const std::uint32_t width = scalarWidthInWords(port.type);
            std::uint32_t words[2] = {0u, 0u};
            const std::uint32_t base = layout.take(port.name, width);
            const std::uint32_t firstValueWord =
                appendArgWordsAsPairs(image.arg_buf, cursor_words, base, words, width,
                                      node.kernel.name, port.name);
            deferred_.push_back(DeferredScalar{
                scopedScalarKey(it->second.scopeId(), it->second.varName()),
                it->second.varName(),
                it->second.scopeId() == 0,
                port.type,
                firstValueWord,
                node.id + "." + port.name});
            arg_count += width;
        }

        // Output scalars are not dispatch arguments: the kernel writes them to
        // its own s_axilite output registers, which the graph captures after the
        // kernel completes via an RP1_OP_SCALAR_READ into a signal slot
        // (see emitOutputScalarReads).  Nothing to pack here.

        for (const BufferPort& port : node.kernel.ioType.inputs) {
            auto it = node.ioMap.inputs().find(port.name);
            if (it == node.ioMap.inputs().end()) {
                throw std::runtime_error(
                    "FpgaDevice: kernel '" + node.kernel.name +
                    "' input buffer port '" + port.name + "' has no IOMap binding");
            }
            appendBufferAddress(image, node, layout, port.name, it->second,
                                0, cursor_words, arg_count);
        }

        for (const BufferPort& port : node.kernel.ioType.outputs) {
            auto it = node.ioMap.outputs().find(port.name);
            if (it == node.ioMap.outputs().end()) {
                throw std::runtime_error(
                    "FpgaDevice: kernel '" + node.kernel.name +
                    "' output buffer port '" + port.name + "' has no IOMap binding");
            }
            appendBufferAddress(image, node, layout, port.name, it->second,
                                0, cursor_words, arg_count);
        }

        for (const RWBufferPort& port : node.kernel.ioType.inouts) {
            auto it = std::find_if(node.ioMap.inouts().begin(),
                                   node.ioMap.inouts().end(),
                                   [&](const IOMap::InoutBinding& binding) {
                                       return binding.inPort == port.in.name &&
                                              binding.outPort == port.out.name;
                                   });
            if (it == node.ioMap.inouts().end()) {
                throw std::runtime_error(
                    "FpgaDevice: kernel '" + node.kernel.name +
                    "' RW buffer ports '" + port.in.name + "'/'" +
                    port.out.name + "' have no IOMap binding");
            }
            appendBufferAddress(image, node, layout, port.in.name, it->in,
                                0, cursor_words, arg_count);
            appendBufferAddress(image, node, layout, port.out.name, it->out,
                                0, cursor_words, arg_count);
        }

        if (arg_count > UINT16_MAX) {
            throw std::logic_error(
                "FpgaDevice: kernel '" + node.kernel.name + "' has " +
                std::to_string(arg_count) + " arg pairs, exceeds uint16_t cap");
        }
        return arg_count;
    }

    // Opt-in diagnostic: dump the kernel base and the (reg_offset, value)
    // argument pairs RP1 will write.  Enabled by setting VRT_FPGA_DEBUG_ARGS
    // in the environment; invaluable for confirming the v2 packing against a
    // real s_axilite register map during hardware bring-up.
    static void dumpKernelArgs(const std::string& kernelName,
                               const rp1_payload_kernel_dispatch_t& kd,
                               const std::vector<std::uint32_t>& arg_buf) {
        static const bool enabled = (std::getenv("VRT_FPGA_DEBUG_ARGS") != nullptr);
        if (!enabled) return;
        const std::uint32_t firstWord = kd.arg_buffer_offset / sizeof(std::uint32_t);
        std::cerr << "[FpgaDevice] dispatch '" << kernelName << "' base=0x" << std::hex
                  << kd.kernel_base_addr << std::dec << " arg_count=" << kd.arg_count
                  << " (reg_offset, value) pairs:";
        for (std::uint32_t i = 0; i < kd.arg_count; ++i) {
            const std::uint32_t w = firstWord + 2u * i;
            if (w + 1u >= arg_buf.size()) break;
            std::cerr << "\n    +0x" << std::hex << arg_buf[w]
                      << " = 0x" << arg_buf[w + 1u] << std::dec;
        }
        std::cerr << std::endl;
    }

    std::uint64_t stagePdi(const CompiledReprogramNode& node) {
        if (device_->vbinSpec_ && device_->vbinSpec_->hasImage(node.imageId)) {
            const auto& imageSpec = device_->vbinSpec_->image(node.imageId);
            return device_->stagePdiBytes(imageSpec.id, imageSpec.pdiBytes);
        }
        return device_->stagePdiFile(node.pdiPath);
    }

    // Gather every node a control body owns across all its per-device child
    // DGraphs (the FPGA kernels/reprograms plus the boundary nodes the compiler
    // emits on the CPU DGraph for carried-buffer import/export), ordered so the
    // import (Start) boundaries run first, then the work, then the export (End)
    // boundaries.  That ordering lets the Start aliases resolve before the body
    // kernels are packed and the End aliases resolve after they produce.
    static std::vector<const CompiledNode*> collectControlBody(
        const DGraph& dg, const std::string& controlId, DGraphChildRole role) {
        std::vector<const CompiledNode*> starts, mids, ends;
        for (const DGraphChild& child : dg.childDGraphs) {
            if (child.parentNodeId != controlId || child.role != role) continue;
            for (const auto& body : child.dgraphs) {
                if (!body) continue;
                for (const CompiledNode& n : body->nodes) {
                    if (const auto* b = std::get_if<CompiledBoundaryNode>(&n)) {
                        (b->side == CompiledBoundaryNode::Side::Start ? starts : ends).push_back(&n);
                    } else {
                        mids.push_back(&n);
                    }
                }
            }
        }
        {
            std::unordered_map<std::string, std::size_t> idxById;
            for (std::size_t i = 0; i < mids.size(); ++i) {
                idxById[compiledNodeId(*mids[i])] = i;
            }
            std::vector<int> indeg(mids.size(), 0);
            std::vector<std::vector<std::size_t>> succ(mids.size());
            for (std::size_t i = 0; i < mids.size(); ++i) {
                for (const std::string& d : compiledNodeDependsOn(*mids[i])) {
                    auto it = idxById.find(d);
                    if (it != idxById.end()) {
                        succ[it->second].push_back(i);
                        ++indeg[i];
                    }
                }
            }
            std::set<std::size_t> ready;
            for (std::size_t i = 0; i < mids.size(); ++i) {
                if (indeg[i] == 0) ready.insert(i);
            }
            std::vector<const CompiledNode*> sorted;
            sorted.reserve(mids.size());
            while (!ready.empty()) {
                const std::size_t i = *ready.begin();
                ready.erase(ready.begin());
                sorted.push_back(mids[i]);
                for (std::size_t s : succ[i]) {
                    if (--indeg[s] == 0) ready.insert(s);
                }
            }
            if (sorted.size() == mids.size()) mids = std::move(sorted);
        }
        std::vector<const CompiledNode*> out;
        out.reserve(starts.size() + mids.size() + ends.size());
        out.insert(out.end(), starts.begin(), starts.end());
        out.insert(out.end(), mids.begin(), mids.end());
        out.insert(out.end(), ends.begin(), ends.end());
        return out;
    }

    // Emit one KERNEL_DISPATCH packet, packing its args into image.arg_buf.
    std::size_t emitKernelPacket(fpga::Rp1GraphImage& image, const CompiledKernelNode& k,
                                 std::uint8_t awBucket, std::uint32_t awMask,
                                 std::uint8_t setBucket, std::uint32_t setMask,
                                 const std::set<std::string>& skipInputScalars = {}) {
        const FpgaKernelLocation loc = device_->resolveKernelLocation(k.kernel);
        const std::uint32_t argOffset =
            static_cast<std::uint32_t>(image.arg_buf.size()) * sizeof(std::uint32_t);
        const std::uint32_t argCount = packKernelArgs(image, k, skipInputScalars);
        rp1_node_t pkt{};
        pkt.status               = RP1_NODE_PENDING;
        pkt.opcode               = RP1_OP_KERNEL_DISPATCH;
        pkt.barrier_await_bucket = awBucket;
        pkt.barrier_await_mask   = awMask;
        pkt.barrier_set_bucket   = setBucket;
        pkt.barrier_set_mask     = setMask;
        auto& kd = pkt.payload.kernel_dispatch;
        kd.kernel_base_addr  = loc.r5_base_addr;
        kd.arg_buffer_offset = argOffset;
        kd.arg_count         = static_cast<std::uint16_t>(argCount);
        kd.timeout_cycles    = loc.timeout_cycles;
        kd.expected_image_id = k.kernel.image ? device_->imageNumericId(*k.kernel.image) : 0u;
        image.nodes.push_back(pkt);
        return image.nodes.size() - 1;
    }

    std::size_t emitReprogramPacket(fpga::Rp1GraphImage& image, const CompiledReprogramNode& r,
                                    std::uint8_t awBucket, std::uint32_t awMask,
                                    std::uint8_t setBucket, std::uint32_t setMask) {
        rp1_node_t pkt{};
        pkt.status               = RP1_NODE_PENDING;
        pkt.opcode               = RP1_OP_PDI_LOAD;
        pkt.flags                = RP1_FLAG_HALT_ON_ERROR;
        pkt.barrier_await_bucket = awBucket;
        pkt.barrier_await_mask   = awMask;
        pkt.barrier_set_bucket   = setBucket;
        pkt.barrier_set_mask     = setMask;
        auto& pl = pkt.payload.pdi_load;
        pl.pdi_addr_lo    = 0;
        pl.pdi_addr_hi    = 0;
        pl.timeout_cycles = r.timeoutCycles;
        pl.image_id       = device_->imageNumericId(r.imageId);
        image.nodes.push_back(pkt);
        const std::size_t idx = image.nodes.size() - 1;
        deferredPdis_.push_back(DeferredPdi{idx, r.imageId, r.pdiPath});
        return idx;
    }

    fpga::Rp1GraphImage buildMainlineImage(const DGraph& dg) {
        if (dg.nodes.empty()) throw std::logic_error("FpgaDevice: empty FPGA DGraph");

        const std::size_t N = dg.nodes.size();
        fpga::SignalSlotAllocator slotAlloc;
        slotAlloc.reserve(sentinelSlot_);
        std::size_t totalWork = N;
        std::vector<bool> hasOutputScalarReads(N, false);
        for (std::size_t p = 0; p < N; ++p) {
            if (const auto* k = std::get_if<CompiledKernelNode>(&dg.nodes[p])) {
                totalWork += k->kernel.ioType.outputScalars.size();
                hasOutputScalarReads[p] = !k->kernel.ioType.outputScalars.empty();
            }
        }
        const std::size_t nodeBuckets =
            (totalWork + (kKernelBitsPerBucket - 1)) / kKernelBitsPerBucket;
        if (nodeBuckets >= RP1_MAX_BUCKETS) {
            throw std::logic_error(
                "FpgaDevice: FPGA image has " + std::to_string(N) + " main-line nodes");
        }

        auto nodeBucketOf = [](std::size_t p) -> std::uint8_t {
            return static_cast<std::uint8_t>(p / kKernelBitsPerBucket);
        };
        auto nodeBitOf = [](std::size_t p) -> std::uint32_t {
            return 1u << (p % kKernelBitsPerBucket);
        };

        std::unordered_map<std::string, std::size_t> posOf;
        for (std::size_t p = 0; p < N; ++p) posOf[compiledNodeId(dg.nodes[p])] = p;
        std::size_t nextExtraPos = N;

        std::vector<bool> isLeaf(N, true);
        std::map<std::uint8_t, std::uint32_t> extraLeafGroups;
        for (std::size_t p = 0; p < N; ++p) {
            for (const std::string& depId : compiledNodeDependsOn(dg.nodes[p])) {
                auto it = posOf.find(depId);
                if (it != posOf.end()) isLeaf[it->second] = false;
            }
        }

        fpga::Rp1GraphImage image;
        std::vector<rp1_node_t> aggregators;
        std::uint8_t  joinBucket = static_cast<std::uint8_t>(nodeBuckets);
        std::uint32_t joinBit = 0;
        auto resolveAwait =
            [&](const std::map<std::uint8_t, std::uint32_t>& groups)
            -> std::pair<std::uint8_t, std::uint32_t> {
            if (groups.empty()) return {std::uint8_t{0}, 0u};
            if (groups.size() == 1) return {groups.begin()->first, groups.begin()->second};

            const std::uint32_t m = static_cast<std::uint32_t>(groups.size());
            if (joinBit + m > kKernelBitsPerBucket) { ++joinBucket; joinBit = 0; }
            if (joinBucket >= RP1_MAX_BUCKETS) {
                throw std::logic_error(
                    "FpgaDevice: ran out of barrier buckets for main-line join aggregation");
            }

            const std::uint8_t cb = joinBucket;
            const std::uint32_t base = joinBit;
            joinBit += m;
            std::uint32_t mask = 0;
            std::uint32_t j = 0;
            for (const auto& [bucket, bits] : groups) {
                rp1_node_t agg{};
                agg.opcode = RP1_OP_NOP;
                agg.flags = RP1_FLAG_SILENT;
                agg.status = RP1_NODE_PENDING;
                agg.barrier_await_bucket = bucket;
                agg.barrier_await_mask = bits;
                agg.barrier_set_bucket = cb;
                agg.barrier_set_mask = 1u << (base + j);
                mask |= 1u << (base + j);
                aggregators.push_back(agg);
                ++j;
            }
            return {cb, mask};
        };

        for (std::size_t p = 0; p < N; ++p) {
            const CompiledNode& n = dg.nodes[p];
            std::map<std::uint8_t, std::uint32_t> predGroups;
            for (const std::string& depId : compiledNodeDependsOn(n)) {
                auto it = posOf.find(depId);
                if (it != posOf.end()) predGroups[nodeBucketOf(it->second)] |= nodeBitOf(it->second);
            }
            const auto [awBucket, awMask] = resolveAwait(predGroups);
            const std::uint8_t setBucket = nodeBucketOf(p);
            const std::uint32_t setMask = nodeBitOf(p);

            if (const auto* k = std::get_if<CompiledKernelNode>(&n)) {
                emitKernelPacket(image, *k, awBucket, awMask, setBucket, setMask);
                std::uint8_t lastBucket = setBucket;
                std::uint32_t lastMask = setMask;
                for (const ScalarPort& sp : k->kernel.ioType.outputScalars) {
                    const std::size_t rpos = nextExtraPos++;
                    const std::uint8_t rbucket = nodeBucketOf(rpos);
                    const std::uint32_t rmask = nodeBitOf(rpos);
                    const std::uint32_t slot = slotAlloc.alloc();
                    const FpgaKernelLocation loc = device_->resolveKernelLocation(k->kernel);
                    const std::uint32_t off = device_->outputScalarRegOffset(k->kernel, sp.name);
                    rp1_node_t sr{};
                    sr.opcode = RP1_OP_SCALAR_READ;
                    sr.status = RP1_NODE_PENDING;
                    sr.barrier_await_bucket = lastBucket;
                    sr.barrier_await_mask = lastMask;
                    sr.barrier_set_bucket = rbucket;
                    sr.barrier_set_mask = rmask;
                    sr.payload.scalar_read.source_addr = loc.r5_base_addr + off;
                    sr.payload.scalar_read.target_slot = slot;
                    image.nodes.push_back(sr);
                    auto bindIt = k->ioMap.outputScalars().find(sp.name);
                    if (bindIt != k->ioMap.outputScalars().end()) {
                        const GraphScalar& gs = bindIt->second;
                        scalarSlots_[scopedScalarKey(gs.scopeId(), gs.varName())] = slot;
                    }
                    extraLeafGroups[rbucket] |= rmask;
                    lastBucket = rbucket;
                    lastMask = rmask;
                }
            } else if (const auto* r = std::get_if<CompiledReprogramNode>(&n)) {
                emitReprogramPacket(image, *r, awBucket, awMask, setBucket, setMask);
            } else if (const auto* sg = std::get_if<CompiledSignalNode>(&n)) {
                rp1_node_t pkt{};
                pkt.opcode = RP1_OP_SIGNAL;
                pkt.status = RP1_NODE_PENDING;
                pkt.barrier_await_bucket = awBucket;
                pkt.barrier_await_mask = awMask;
                pkt.barrier_set_bucket = setBucket;
                pkt.barrier_set_mask = setMask;
                pkt.payload.signal.target_slot = sg->slot;
                pkt.payload.signal.value = sg->value;
                pkt.payload.signal.operation = sg->operation;
                image.nodes.push_back(pkt);
            } else if (const auto* wt = std::get_if<CompiledWaitNode>(&n)) {
                rp1_node_t pkt{};
                pkt.opcode = RP1_OP_WAIT;
                pkt.status = RP1_NODE_PENDING;
                pkt.barrier_await_bucket = awBucket;
                pkt.barrier_await_mask = awMask;
                pkt.barrier_set_bucket = setBucket;
                pkt.barrier_set_mask = setMask;
                pkt.payload.wait.condition_signal = wt->slot;
                pkt.payload.wait.condition_value = wt->value;
                pkt.payload.wait.condition_op = wt->conditionOp;
                image.nodes.push_back(pkt);
            } else {
                throw std::logic_error(
                    "FpgaDevice: non-RP1 node '" + compiledNodeId(n) +
                    "' reached main-line FPGA image lowering");
            }
        }

        std::map<std::uint8_t, std::uint32_t> leafGroups = extraLeafGroups;
        for (std::size_t p = 0; p < N; ++p) {
            if (isLeaf[p] && !hasOutputScalarReads[p]) leafGroups[nodeBucketOf(p)] |= nodeBitOf(p);
        }
        const auto [sentBucket, sentMask] = resolveAwait(leafGroups);
        for (const rp1_node_t& agg : aggregators) image.nodes.push_back(agg);

        rp1_node_t sentinel{};
        sentinel.opcode = RP1_OP_SIGNAL;
        sentinel.status = RP1_NODE_PENDING;
        sentinel.barrier_await_bucket = sentBucket;
        sentinel.barrier_await_mask = sentMask;
        sentinel.barrier_set_bucket = kSentinelBucket;
        sentinel.barrier_set_mask = kSentinelBit;
        sentinel.payload.signal.target_slot = sentinelSlot_;
        sentinel.payload.signal.value = sentinelValue_;
        sentinel.payload.signal.operation = RP1_SIGOP_SET;
        image.nodes.push_back(sentinel);
        image.clear_signal_slots.push_back(sentinelSlot_);
        clearHandshakeSlots(image);

        if (image.nodes.size() > RP1_MAX_NODES) {
            throw std::logic_error("FpgaDevice: main-line image exceeds RP1_MAX_NODES");
        }
        return image;
    }

    // Lower a control-flow DGraph (loops/conditionals whose body lives entirely
    // on this FPGA queue) into a single RP1 image that the firmware executes
    // autonomously.  Main-line nodes (pre/post-control kernels, the control
    // nodes' completion signals, the sentinel) occupy bucket 0; each loop body
    // gets its own bucket that the LOOP node clears per iteration.
    //
    // Supported: scalar fixed-count loops (termination via max_iterations),
    // data-dependent while-loops (termination via a signal-slot predicate fed
    // by a body output scalar's SCALAR_READ), and conditionals (COND-gated
    // then/else with an OR-join, predicate from a main-line output scalar).
    // Nested control, control body data boundaries, and >31 nodes per scope
    // throw a descriptive diagnostic (the compiler keeps those on the CPU path).
    fpga::Rp1GraphImage buildControlImage(const DGraph& dg) {
        fpga::Rp1GraphImage image;
        fpga::LoopIdAllocator loopIds;
        fpga::SignalSlotAllocator slotAlloc;
        slotAlloc.reserve(sentinelSlot_);

        std::unordered_map<std::string, std::uint32_t> mainBit;  // id -> done bit (bucket 0)
        std::uint32_t nextMainBit  = 0;
        std::uint8_t  nextBodyBkt  = 1;
        std::vector<std::string> consumedMain;

        auto allocMainBit = [&]() -> std::uint32_t {
            if (nextMainBit >= kKernelBitsPerBucket) {
                throw std::logic_error(
                    "FpgaDevice: control-flow image exceeds 31 main-line nodes "
                    "(bucket 0 is full); multi-bucket main line is a future phase");
            }
            return 1u << (nextMainBit++);
        };
        auto awaitMaskFor = [&](const std::vector<std::string>& deps) -> std::uint32_t {
            std::uint32_t m = 0;
            for (const auto& d : deps) {
                auto it = mainBit.find(d);
                if (it != mainBit.end()) {
                    m |= it->second;
                    consumedMain.push_back(d);
                }
            }
            return m;
        };

        for (const CompiledNode& node : dg.nodes) {
            if (const auto* k = std::get_if<CompiledKernelNode>(&node)) {
                const std::uint32_t aw  = awaitMaskFor(k->dependsOn);
                const std::uint32_t bit = allocMainBit();
                emitKernelPacket(image, *k, 0, aw, 0, bit);
                mainBit[k->id] = bit;
                // Capture this kernel's output scalars into signal slots (each
                // SCALAR_READ chains after the previous node so the kernel's
                // result is settled first); the last node becomes the leaf.
                std::uint32_t lastBit = bit;
                std::string   lastId  = k->id;
                for (const ScalarPort& sp : k->kernel.ioType.outputScalars) {
                    const std::uint32_t slot = slotAlloc.alloc();
                    const FpgaKernelLocation loc = device_->resolveKernelLocation(k->kernel);
                    const std::uint32_t off = device_->outputScalarRegOffset(k->kernel, sp.name);
                    const std::uint32_t rbit = allocMainBit();
                    rp1_node_t sr{};
                    sr.opcode               = RP1_OP_SCALAR_READ;
                    sr.status               = RP1_NODE_PENDING;
                    sr.barrier_await_bucket = 0;
                    sr.barrier_await_mask   = lastBit;
                    sr.barrier_set_bucket   = 0;
                    sr.barrier_set_mask     = rbit;
                    sr.payload.scalar_read.source_addr = loc.r5_base_addr + off;
                    sr.payload.scalar_read.target_slot = slot;
                    image.nodes.push_back(sr);
                    auto bindIt = k->ioMap.outputScalars().find(sp.name);
                    if (bindIt != k->ioMap.outputScalars().end()) {
                        const GraphScalar& gs = bindIt->second;
                        scalarSlots_[scopedScalarKey(gs.scopeId(), gs.varName())] = slot;
                    }
                    consumedMain.push_back(lastId);
                    lastId  = k->id + ".sread." + sp.name;
                    lastBit = rbit;
                    mainBit[lastId] = rbit;
                }
            } else if (const auto* r = std::get_if<CompiledReprogramNode>(&node)) {
                const std::uint32_t aw  = awaitMaskFor(r->dependsOn);
                const std::uint32_t bit = allocMainBit();
                emitReprogramPacket(image, *r, 0, aw, 0, bit);
                mainBit[r->id] = bit;
            } else if (const auto* loop = std::get_if<CompiledLoopNode>(&node)) {
                lowerLoop(image, dg, *loop, loopIds, slotAlloc, mainBit, consumedMain,
                          awaitMaskFor, allocMainBit, nextBodyBkt);
            } else if (std::holds_alternative<CompiledBridgeOpNode>(node)) {
                throw std::logic_error(
                    "FpgaDevice: host bridge node '" + compiledNodeId(node) +
                    "' reached the FPGA DGraph; bridge actions must be owned by the CPU DGraph");
            } else if (const auto* cnd = std::get_if<CompiledConditionalNode>(&node)) {
                lowerConditional(image, dg, *cnd, slotAlloc, mainBit, consumedMain,
                                 awaitMaskFor, allocMainBit, nextBodyBkt);
            } else if (const auto* sg = std::get_if<CompiledSignalNode>(&node)) {
                const std::uint32_t aw  = awaitMaskFor(sg->dependsOn);
                const std::uint32_t bit = allocMainBit();
                rp1_node_t pkt{};
                pkt.opcode               = RP1_OP_SIGNAL;
                pkt.status               = RP1_NODE_PENDING;
                pkt.barrier_await_bucket = 0;
                pkt.barrier_await_mask   = aw;
                pkt.barrier_set_bucket   = 0;
                pkt.barrier_set_mask     = bit;
                pkt.payload.signal.target_slot = sg->slot;
                pkt.payload.signal.value       = sg->value;
                pkt.payload.signal.operation   = sg->operation;
                image.nodes.push_back(pkt);
                mainBit[sg->id] = bit;
            } else if (const auto* wt = std::get_if<CompiledWaitNode>(&node)) {
                const std::uint32_t aw  = awaitMaskFor(wt->dependsOn);
                const std::uint32_t bit = allocMainBit();
                rp1_node_t pkt{};
                pkt.opcode               = RP1_OP_WAIT;
                pkt.status               = RP1_NODE_PENDING;
                pkt.barrier_await_bucket = 0;
                pkt.barrier_await_mask   = aw;
                pkt.barrier_set_bucket   = 0;
                pkt.barrier_set_mask     = bit;
                pkt.payload.wait.condition_signal = wt->slot;
                pkt.payload.wait.condition_value  = wt->value;
                pkt.payload.wait.condition_op     = wt->conditionOp;
                image.nodes.push_back(pkt);
                mainBit[wt->id] = bit;
            } else if (std::holds_alternative<CompiledBoundaryNode>(node)) {
                throw std::logic_error(
                    "FpgaDevice: top-level region boundary in a control DGraph is "
                    "not supported (node '" + compiledNodeId(node) + "')");
            } else {
                throw std::logic_error(
                    "FpgaDevice: unexpected node in control DGraph '" +
                    compiledNodeId(node) + "'");
            }
        }

        // Sentinel awaits every main-line leaf (a node nothing else awaits).
        std::uint32_t sentMask = 0;
        for (const auto& [id, bit] : mainBit) {
            if (std::find(consumedMain.begin(), consumedMain.end(), id) == consumedMain.end()) {
                sentMask |= bit;
            }
        }

        rp1_node_t sentinel{};
        sentinel.opcode               = RP1_OP_SIGNAL;
        sentinel.status               = RP1_NODE_PENDING;
        sentinel.barrier_await_bucket = kSentinelBucket;
        sentinel.barrier_await_mask   = sentMask;
        sentinel.barrier_set_bucket   = kSentinelBucket;
        sentinel.barrier_set_mask     = kSentinelBit;
        sentinel.payload.signal.target_slot = sentinelSlot_;
        sentinel.payload.signal.value       = sentinelValue_;
        sentinel.payload.signal.operation   = RP1_SIGOP_SET;
        image.nodes.push_back(sentinel);
        image.clear_signal_slots.push_back(sentinelSlot_);
        clearHandshakeSlots(image);

        if (image.nodes.size() > RP1_MAX_NODES) {
            throw std::logic_error("FpgaDevice: control-flow image exceeds RP1_MAX_NODES");
        }
        return image;
    }

    struct BarrierRef {
        std::uint8_t bucket = 0;
        std::uint32_t mask = 0;
    };

    class ResetDomainEmitter {
       public:
        ResetDomainEmitter(fpga::Rp1GraphImage& image, std::uint8_t& nextBucket)
            : image_(image), nextBucket_(nextBucket) {}

        BarrierRef define(const std::string& id) {
            BarrierRef ref = allocBit();
            events_[id] = ref;
            return ref;
        }

        std::vector<BarrierRef> refsFor(const std::vector<std::string>& deps) {
            std::vector<BarrierRef> refs;
            for (const auto& dep : deps) {
                auto it = events_.find(dep);
                if (it == events_.end()) continue;
                refs.push_back(it->second);
                consumed_.insert(dep);
            }
            return refs;
        }

        BarrierRef awaitFor(const std::vector<BarrierRef>& refs) {
            std::map<std::uint8_t, std::uint32_t> groups;
            for (const BarrierRef& ref : refs) {
                if (ref.mask != 0) groups[ref.bucket] |= ref.mask;
            }
            if (groups.empty()) return {};
            if (groups.size() == 1) return {groups.begin()->first, groups.begin()->second};

            std::vector<BarrierRef> collectors;
            collectors.reserve(groups.size());
            for (const auto& [bucket, mask] : groups) {
                BarrierRef c = allocBit();
                rp1_node_t pkt{};
                pkt.opcode = RP1_OP_NOP;
                pkt.flags = RP1_FLAG_SILENT;
                pkt.status = RP1_NODE_PENDING;
                pkt.barrier_await_bucket = bucket;
                pkt.barrier_await_mask = mask;
                pkt.barrier_set_bucket = c.bucket;
                pkt.barrier_set_mask = c.mask;
                image_.nodes.push_back(pkt);
                collectors.push_back(c);
            }
            return awaitFor(collectors);
        }

        BarrierRef leafAwait() const {
            std::map<std::uint8_t, std::uint32_t> groups;
            for (const auto& [id, ref] : events_) {
                if (consumed_.find(id) == consumed_.end()) groups[ref.bucket] |= ref.mask;
            }
            if (groups.empty()) return {};
            if (groups.size() == 1) return {groups.begin()->first, groups.begin()->second};
            throw std::logic_error(
                "ResetDomainEmitter::leafAwait: leaf set spans buckets; call mutableLeafAwait");
        }

        BarrierRef mutableLeafAwait() {
            std::vector<BarrierRef> refs;
            refs.reserve(events_.size());
            for (const auto& [id, ref] : events_) {
                if (consumed_.find(id) == consumed_.end()) refs.push_back(ref);
            }
            return awaitFor(refs);
        }

        std::uint8_t clearStart() const { return startBucket_; }
        std::uint8_t clearEnd() const { return endBucket_; }

       private:
        BarrierRef allocBit() {
            if (!haveBucket_ || nextBit_ >= kKernelBitsPerBucket) {
                if (nextBucket_ >= RP1_MAX_BUCKETS) {
                    throw std::logic_error("FpgaDevice: ran out of RP1 barrier buckets");
                }
                currentBucket_ = nextBucket_++;
                if (!haveBucket_) startBucket_ = currentBucket_;
                endBucket_ = currentBucket_;
                nextBit_ = 0;
                haveBucket_ = true;
            }
            return {currentBucket_, 1u << nextBit_++};
        }

        fpga::Rp1GraphImage& image_;
        std::uint8_t& nextBucket_;
        std::unordered_map<std::string, BarrierRef> events_;
        std::set<std::string> consumed_;
        std::uint8_t currentBucket_ = 0;
        std::uint32_t nextBit_ = 0;
        std::uint8_t startBucket_ = 0;
        std::uint8_t endBucket_ = 0;
        bool haveBucket_ = false;
    };

    // Emit LOOP + flattened body + RERUN for a scalar fixed-count loop or a
    // data-dependent while-loop.  A while-loop's predicate is evaluated by RP1
    // against a signal slot that a body kernel's output scalar feeds each
    // iteration via SCALAR_READ; the loop exits when the while-continue
    // predicate goes false (the inverse RP1 comparison matches).
    template <class AwaitFn, class AllocFn>
    void lowerLoop(fpga::Rp1GraphImage& image, const DGraph& dg,
                   const CompiledLoopNode& loop, fpga::LoopIdAllocator& loopIds,
                   fpga::SignalSlotAllocator& slotAlloc,
                   std::unordered_map<std::string, std::uint32_t>& mainBit,
                   std::vector<std::string>& consumedMain,
                   AwaitFn awaitMaskFor, AllocFn allocMainBit,
                   std::uint8_t& nextBodyBkt) {
        // A data-dependent split Follower reads the Authority's broadcast as its
        // exit predicate, so it neither evaluates the condition itself nor needs
        // a local trip count -- skip the while/fixed-count validation.
        const bool isFollower = loop.broadcastRole == SplitBroadcastRole::Follower;
        const bool isWhile = !isFollower && loop.loopKind == CompiledLoopKind::WhileCondition;
        if (isFollower) {
            // nothing to validate here; broadcast wiring is applied below.
        } else if (isWhile) {
            if (!loop.condition) {
                throw std::logic_error(
                    "FpgaDevice: while-loop '" + loop.id + "' is missing its condition");
            }
            if (!fpga::isRp1EvaluableCondition(*loop.condition)) {
                throw std::logic_error(
                    "FpgaDevice: while-loop '" + loop.id + "' condition is not RP1-evaluable "
                    "(needs one integer scalar compared against a constant)");
            }
        } else if (loop.loopKind != CompiledLoopKind::FixedCount || !loop.tripCount) {
            throw std::logic_error(
                "FpgaDevice: autonomous fixed-count loop '" + loop.id +
                "' is missing its scalar trip count");
        }

        const std::vector<const CompiledNode*> bodyNodes =
            collectControlBody(dg, loop.id, DGraphChildRole::LoopBody);
        if (bodyNodes.empty()) {
            throw std::logic_error(
                "FpgaDevice: loop '" + loop.id +
                "' has no FPGA body nodes (a loop body running entirely on another "
                "device is not yet supported)");
        }

        const std::uint32_t loopAwait = awaitMaskFor(loop.dependsOn);
        const std::uint32_t exitBit   = allocMainBit();
        ResetDomainEmitter bodyDomain(image, nextBodyBkt);

        // LOOP packet (body_start/end backpatched after the body is emitted).
        rp1_node_t loopPkt{};
        loopPkt.opcode               = RP1_OP_LOOP;
        loopPkt.status               = RP1_NODE_PENDING;
        loopPkt.barrier_await_bucket = 0;
        loopPkt.barrier_await_mask   = loopAwait;
        loopPkt.barrier_set_bucket   = 0;
        loopPkt.barrier_set_mask     = exitBit;
        const std::size_t loopIdx = image.nodes.size();
        image.nodes.push_back(loopPkt);
        if (!isFollower && !isWhile) {
            deferredTripCounts_.push_back(DeferredLoopTripCount{
                loopIdx,
                scopedScalarKey(loop.tripCount->scopeId(), loop.tripCount->name()),
                loop.tripCount->name(),
                loop.tripCount->scopeId() == 0,
                loop.tripCount->type(),
                loop.id + "." + loop.tripCount->name()});
        }

        // Flatten the body into one reset domain.  The domain emitter allocates
        // as many contiguous buckets as needed and inserts silent collector NOPs
        // whenever an await list spans buckets.

        // Size token of each carried buffer, taken from the body kernels'
        // buffer bindings.  Concrete bytes are resolved at launch so one
        // compiled graph can run with different size-constant values.
        std::unordered_map<std::string, GraphBuffer> tokenBuffers;
        auto noteBuffer = [&](const GraphBuffer& gb) {
            if (!gb.valid() || !gb.hasSizeScalar()) return;
            const std::string key = scopedBufferKey(gb.scopeId(), gb.name());
            tokenBuffers.emplace(key, gb);
        };
        for (const CompiledNode* bnp : bodyNodes) {
            const auto* k = std::get_if<CompiledKernelNode>(bnp);
            if (!k) continue;
            for (const auto& [port, gb] : k->ioMap.inputs())  { (void)port; noteBuffer(gb); }
            for (const auto& [port, gb] : k->ioMap.outputs()) { (void)port; noteBuffer(gb); }
            for (const IOMap::InoutBinding& rw : k->ioMap.inouts())  { noteBuffer(rw.in); noteBuffer(rw.out); }
        }

        // Loop-carried scalars: an import (Start) boundary feeds the parent
        // scalar's slot into a body kernel input register via SCALAR_COPY each
        // iteration; an export (End) boundary captures a body kernel output into
        // that same slot.  Both ends share one stable slot keyed by the parent
        // scalar (allocated here / by a pre-loop init), so the carried value
        // flows entirely on-FPGA across iterations.  Maps are local-scalar-key
        // -> parent-scalar-key.
        std::unordered_map<std::string, std::string> scalarImport;  // local <- parent
        std::unordered_map<std::string, std::string> scalarExport;  // local -> parent
        for (const CompiledNode* bnp : bodyNodes) {
            const auto* b = std::get_if<CompiledBoundaryNode>(bnp);
            if (!b) continue;
            for (const CompiledScalarBoundaryCopy& sc : b->scalarCopies) {
                const std::string parent
                    = scopedScalarKey(b->side == CompiledBoundaryNode::Side::Start
                                          ? sc.sourceScopeId : sc.targetScopeId,
                                      b->side == CompiledBoundaryNode::Side::Start
                                          ? sc.sourceName : sc.targetName);
                const std::string local
                    = scopedScalarKey(b->side == CompiledBoundaryNode::Side::Start
                                          ? sc.targetScopeId : sc.sourceScopeId,
                                      b->side == CompiledBoundaryNode::Side::Start
                                          ? sc.targetName : sc.sourceName);
                (b->side == CompiledBoundaryNode::Side::Start ? scalarImport
                                                              : scalarExport)[local] = parent;
            }
        }
        // Reserve a stable slot per carried parent scalar (reuse the init's slot
        // if a pre-loop producer already captured it).
        auto carriedSlot = [&](const std::string& parentKey) -> std::uint32_t {
            auto it = scalarSlots_.find(parentKey);
            if (it != scalarSlots_.end()) return it->second;
            const std::uint32_t s = slotAlloc.alloc();
            scalarSlots_[parentKey] = s;
            return s;
        };

        for (const CompiledNode* bnp : bodyNodes) {
            const CompiledNode& bn = *bnp;
            if (const auto* k = std::get_if<CompiledKernelNode>(&bn)) {
                if (k->kernel.type != DeviceType::FPGA) {
                    throw std::logic_error(
                        "FpgaDevice: loop '" + loop.id + "' body kernel '" + k->kernel.name +
                        "' is not an FPGA kernel; loop bodies mixing CPU and FPGA kernels "
                        "are not yet supported");
                }
                // Feed any loop-carried scalar inputs from their slots before the
                // dispatch (SCALAR_COPY slot -> input register); skip packing them.
                std::set<std::string> carriedInputs;
                std::vector<BarrierRef> kernelAwaits = bodyDomain.refsFor(k->dependsOn);
                const FpgaKernelLocation kloc = device_->resolveKernelLocation(k->kernel);
                for (const ScalarPort& ip : k->kernel.ioType.inputScalars) {
                    auto bindIt = k->ioMap.inputScalars().find(ip.name);
                    if (bindIt == k->ioMap.inputScalars().end()) continue;
                    const std::string localKey = scopedScalarKey(
                        bindIt->second.scopeId(), bindIt->second.varName());
                    auto impIt = scalarImport.find(localKey);
                    if (impIt == scalarImport.end()) continue;
                    carriedInputs.insert(ip.name);
                    const std::uint32_t slot = carriedSlot(impIt->second);
                    const std::uint32_t off  = device_->outputScalarRegOffset(k->kernel, ip.name);
                    const BarrierRef copyAwait = bodyDomain.awaitFor(kernelAwaits);
                    const BarrierRef copyDone = bodyDomain.define(k->id + ".scopy." + ip.name);
                    rp1_node_t cp{};
                    cp.opcode               = RP1_OP_SCALAR_COPY;
                    cp.status               = RP1_NODE_PENDING;
                    cp.barrier_await_bucket = copyAwait.bucket;
                    cp.barrier_await_mask   = copyAwait.mask;
                    cp.barrier_set_bucket   = copyDone.bucket;
                    cp.barrier_set_mask     = copyDone.mask;
                    cp.payload.scalar_copy.source_slot = slot;
                    cp.payload.scalar_copy.dest_addr   = kloc.r5_base_addr + off;
                    image.nodes.push_back(cp);
                    kernelAwaits.push_back(copyDone);  // the kernel waits for the carried value
                }

                const BarrierRef kernelAwait = bodyDomain.awaitFor(kernelAwaits);
                const BarrierRef kernelDone = bodyDomain.define(k->id);
                emitKernelPacket(image, *k, kernelAwait.bucket, kernelAwait.mask,
                                 kernelDone.bucket, kernelDone.mask, carriedInputs);
                // Capture body output scalars into signal slots each iteration
                // (chained after the kernel) so a data-dependent loop predicate
                // can read the freshly-produced value.  An exported output reuses
                // its carried parent slot; others get a fresh slot.
                BarrierRef lastDone = kernelDone;
                std::string   lastId  = k->id;
                for (const ScalarPort& sp : k->kernel.ioType.outputScalars) {
                    const FpgaKernelLocation loc = device_->resolveKernelLocation(k->kernel);
                    const std::uint32_t off  = device_->outputScalarRegOffset(k->kernel, sp.name);
                    auto bindIt = k->ioMap.outputScalars().find(sp.name);
                    std::uint32_t slot;
                    std::string   localKey;
                    if (bindIt != k->ioMap.outputScalars().end()) {
                        localKey = scopedScalarKey(bindIt->second.scopeId(),
                                                   bindIt->second.varName());
                    }
                    auto expIt = scalarExport.find(localKey);
                    if (!localKey.empty() && expIt != scalarExport.end()) {
                        slot = carriedSlot(expIt->second);          // export -> carried slot
                    } else {
                        slot = slotAlloc.alloc();
                    }
                    const BarrierRef readDone = bodyDomain.define(k->id + ".sread." + sp.name);
                    rp1_node_t sr{};
                    sr.opcode               = RP1_OP_SCALAR_READ;
                    sr.status               = RP1_NODE_PENDING;
                    sr.barrier_await_bucket = lastDone.bucket;
                    sr.barrier_await_mask   = lastDone.mask;
                    sr.barrier_set_bucket   = readDone.bucket;
                    sr.barrier_set_mask     = readDone.mask;
                    sr.payload.scalar_read.source_addr = loc.r5_base_addr + off;
                    sr.payload.scalar_read.target_slot = slot;
                    image.nodes.push_back(sr);
                    if (!localKey.empty()) scalarSlots_[localKey] = slot;
                    lastDone = readDone;
                }
            } else if (const auto* r = std::get_if<CompiledReprogramNode>(&bn)) {
                const BarrierRef aw = bodyDomain.awaitFor(bodyDomain.refsFor(r->dependsOn));
                const BarrierRef done = bodyDomain.define(r->id);
                emitReprogramPacket(image, *r, aw.bucket, aw.mask, done.bucket, done.mask);
            } else if (const auto* b = std::get_if<CompiledBoundaryNode>(&bn)) {
                // Carried-buffer / import-export region boundary.  On the FPGA
                // queue the parent and local tokens are the same device buffer,
                // so we honour each copy as a zero-copy alias and emit no RP1
                // node (the LOOP body re-dispatches in place each iteration).
                // Carried-scalar import/export boundaries emit no RP1 node here:
                // they were resolved above into per-kernel SCALAR_COPY (import,
                // slot -> input register) and SCALAR_READ-into-carried-slot
                // (export), all sharing one stable slot per parent scalar.
                for (const CompiledBufferBoundaryCopy& copy : b->bufferCopies) {
                    const std::string src = scopedBufferKey(copy.sourceScopeId, copy.sourceName);
                    const std::string tgt = scopedBufferKey(copy.targetScopeId, copy.targetName);
                    auto tokIt = tokenBuffers.find(tgt);
                    if (tokIt == tokenBuffers.end()) tokIt = tokenBuffers.find(src);
                    if (tokIt == tokenBuffers.end()) {
                        throw std::runtime_error(
                            "FpgaDevice: boundary alias '" + src + "' -> '" + tgt +
                            "' has no sized token in the loop body");
                    }
                    deferredBufferAliases_.push_back(DeferredBufferAlias{
                        src,
                        tgt,
                        tokIt->second,
                        b->id});
                }
                // Boundaries emit no RP1 node and take no barrier bit: they are
                // compile-time aliases, so a body kernel that "depends on" a
                // boundary simply sees await 0 (its data is the aliased buffer,
                // already staged before the loop / by an earlier body kernel).
            } else if (const auto* sg = std::get_if<CompiledSignalNode>(&bn)) {
                // Cross-queue rendezvous: raise/clear a signal slot for a peer
                // queue.  Lives in the body bucket so it re-arms each iteration.
                const BarrierRef aw = bodyDomain.awaitFor(bodyDomain.refsFor(sg->dependsOn));
                const BarrierRef done = bodyDomain.define(sg->id);
                rp1_node_t pkt{};
                pkt.opcode               = RP1_OP_SIGNAL;
                pkt.status               = RP1_NODE_PENDING;
                pkt.barrier_await_bucket = aw.bucket;
                pkt.barrier_await_mask   = aw.mask;
                pkt.barrier_set_bucket   = done.bucket;
                pkt.barrier_set_mask     = done.mask;
                pkt.payload.signal.target_slot = sg->slot;
                pkt.payload.signal.value       = sg->value;
                pkt.payload.signal.operation   = sg->operation;
                image.nodes.push_back(pkt);
            } else if (const auto* wt = std::get_if<CompiledWaitNode>(&bn)) {
                const BarrierRef aw = bodyDomain.awaitFor(bodyDomain.refsFor(wt->dependsOn));
                const BarrierRef done = bodyDomain.define(wt->id);
                rp1_node_t pkt{};
                pkt.opcode               = RP1_OP_WAIT;
                pkt.status               = RP1_NODE_PENDING;
                pkt.barrier_await_bucket = aw.bucket;
                pkt.barrier_await_mask   = aw.mask;
                pkt.barrier_set_bucket   = done.bucket;
                pkt.barrier_set_mask     = done.mask;
                pkt.payload.wait.condition_signal = wt->slot;
                pkt.payload.wait.condition_value  = wt->value;
                pkt.payload.wait.condition_op     = wt->conditionOp;
                image.nodes.push_back(pkt);
            } else {
                throw std::logic_error(
                    "FpgaDevice: loop '" + loop.id + "' body node '" + compiledNodeId(bn) +
                    "' is unsupported for autonomous execution (only FPGA kernels/reprograms/"
                    "boundaries/rendezvous; nested control is a future phase)");
            }
        }

        // RERUN awaits the body leaves, re-arms the LOOP node.  Its own bit lives
        // in the body bucket so it is cleared each iteration.
        BarrierRef bodyLeaves = bodyDomain.mutableLeafAwait();

        // Data-dependent split Follower: before re-arming the LOOP, await the
        // Authority's fresh continue/stop decision (broadcastReady), clear it,
        // and acknowledge (broadcastAck).  This gates the next top-of-loop check
        // on a decision the Authority wrote this iteration, and stops the
        // Authority outpacing the Follower.
        if (loop.broadcastRole == SplitBroadcastRole::Follower) {
            const BarrierRef wBit = bodyDomain.define(loop.id + ".broadcast_wait");
            rp1_node_t w{};
            w.opcode = RP1_OP_WAIT; w.status = RP1_NODE_PENDING;
            w.barrier_await_bucket = bodyLeaves.bucket; w.barrier_await_mask = bodyLeaves.mask;
            w.barrier_set_bucket = wBit.bucket;   w.barrier_set_mask = wBit.mask;
            w.payload.wait.condition_signal = loop.broadcastReadySlot;
            w.payload.wait.condition_op = RP1_COP_AND_NZ;
            w.payload.wait.condition_value = 1;
            image.nodes.push_back(w);

            const BarrierRef clrBit = bodyDomain.define(loop.id + ".broadcast_clear");
            rp1_node_t clr{};
            clr.opcode = RP1_OP_SIGNAL; clr.status = RP1_NODE_PENDING;
            clr.barrier_await_bucket = wBit.bucket; clr.barrier_await_mask = wBit.mask;
            clr.barrier_set_bucket = clrBit.bucket;   clr.barrier_set_mask = clrBit.mask;
            clr.payload.signal.target_slot = loop.broadcastReadySlot;
            clr.payload.signal.value = 0; clr.payload.signal.operation = RP1_SIGOP_SET;
            image.nodes.push_back(clr);

            const BarrierRef ackBit = bodyDomain.define(loop.id + ".broadcast_ack");
            rp1_node_t ack{};
            ack.opcode = RP1_OP_SIGNAL; ack.status = RP1_NODE_PENDING;
            ack.barrier_await_bucket = clrBit.bucket; ack.barrier_await_mask = clrBit.mask;
            ack.barrier_set_bucket = ackBit.bucket;   ack.barrier_set_mask = ackBit.mask;
            ack.payload.signal.target_slot = loop.broadcastAckSlot;
            ack.payload.signal.value = 1; ack.payload.signal.operation = RP1_SIGOP_SET;
            image.nodes.push_back(ack);

            bodyLeaves = ackBit;  // RERUN now gates on the acknowledged decision
        }

        const BarrierRef rerunBit = bodyDomain.define(loop.id + ".rerun");
        rp1_node_t rerun{};
        rerun.opcode               = RP1_OP_RERUN;
        rerun.status               = RP1_NODE_PENDING;
        rerun.barrier_await_bucket = bodyLeaves.bucket;
        rerun.barrier_await_mask   = bodyLeaves.mask;
        rerun.barrier_set_bucket   = rerunBit.bucket;
        rerun.barrier_set_mask     = rerunBit.mask;
        rerun.payload.rerun.target_node = static_cast<std::uint32_t>(loopIdx);
        const std::size_t rerunIdx = image.nodes.size();
        image.nodes.push_back(rerun);

        // Backpatch the LOOP payload now that the body range is known.
        auto& lp = image.nodes[loopIdx].payload.loop;
        lp.body_start         = static_cast<std::uint32_t>(loopIdx + 1);
        lp.body_end           = static_cast<std::uint32_t>(rerunIdx);
        if (loop.broadcastRole == SplitBroadcastRole::Follower) {
            // Exit when the Authority broadcasts stop (decision slot != 0).  A
            // large safety cap guards against a stuck peer.
            lp.max_iterations   = 1u << 24;
            lp.condition_signal = loop.conditionBroadcastSlot;
            lp.condition_value  = 1;
            lp.condition_op     = RP1_COP_AND_NZ;
        } else if (isWhile) {
            // Exit when the while-continue predicate goes false: RP1 exits when
            // compare(slot, op, value) holds, so use the inverse of the mapped
            // continue comparison.  max_iterations 0 => predicate alone governs.
            const fpga::Rp1Compare c = fpga::mapRp1Condition(*loop.condition);
            const std::string key = scopedScalarKey(c.scalarScopeId, c.scalarName);
            auto slotIt = scalarSlots_.find(key);
            if (slotIt == scalarSlots_.end()) {
                throw std::logic_error(
                    "FpgaDevice: while-loop '" + loop.id + "' predicate scalar '" +
                    c.scalarName + "' is not produced as a body output scalar; a "
                    "data-dependent FPGA loop variable must be a body kernel output scalar");
            }
            lp.max_iterations   = 0;
            lp.condition_signal = slotIt->second;
            lp.condition_value  = c.value;
            lp.condition_op     = fpga::invertRp1Op(c.op);
        } else {
            lp.max_iterations     = 1;
            lp.condition_signal   = 0;
            lp.condition_value    = fpga::kNeverValue;
            lp.condition_op       = fpga::kNeverOp;  // (sig & 0) != 0 -> never; max_iter governs
        }
        lp.bucket_clear_start = bodyDomain.clearStart();
        lp.bucket_clear_end   = bodyDomain.clearEnd();
        lp.loop_id            = loopIds.alloc();

        mainBit[loop.id] = exitBit;
    }

    // Emit a COND-gated then/else with an OR-join for an autonomous FPGA
    // conditional.
    //
    // RP1's COND only reliably acts as a *pure boolean*: on a met predicate it
    // raises done_mask, otherwise it does nothing useful (its "re-pend body"
    // path can't gate a body whose roots await 0 -- they would run regardless).
    // So each branch is gated by a COND that raises a per-branch "go" bit living
    // in that branch's own barrier bucket; the branch's root nodes await the go
    // bit, so the body runs only when the branch is taken.  A single shared
    // `condDone` bit (bucket 0) is the OR-join: the taken branch's join node
    // sets it, and an absent/empty branch's COND sets it directly -- exactly one
    // path fires it, so downstream proceeds regardless of which side ran.
    template <class AwaitFn, class AllocFn>
    void lowerConditional(fpga::Rp1GraphImage& image, const DGraph& dg,
                          const CompiledConditionalNode& cond,
                          fpga::SignalSlotAllocator& slotAlloc,
                          std::unordered_map<std::string, std::uint32_t>& mainBit,
                          std::vector<std::string>& consumedMain,
                          AwaitFn awaitMaskFor, AllocFn allocMainBit,
                          std::uint8_t& nextBodyBkt) {
        (void)slotAlloc;
        if (!fpga::isRp1EvaluableCondition(cond.condition)) {
            throw std::logic_error(
                "FpgaDevice: conditional '" + cond.id + "' condition is not RP1-evaluable "
                "(needs one integer scalar compared against a constant)");
        }
        const fpga::Rp1Compare c = fpga::mapRp1Condition(cond.condition);
        auto slotIt = scalarSlots_.find(scopedScalarKey(c.scalarScopeId, c.scalarName));
        if (slotIt == scalarSlots_.end()) {
            throw std::logic_error(
                "FpgaDevice: conditional '" + cond.id + "' predicate scalar '" + c.scalarName +
                "' is not produced as a prior output scalar (no SCALAR_READ slot); the "
                "predicate must come from a main-line FPGA kernel output scalar");
        }
        const std::uint32_t condSlot  = slotIt->second;
        const std::uint32_t condAwait = awaitMaskFor(cond.dependsOn);
        // Shared OR-join bit: set by whichever branch is taken.
        const std::uint32_t condDone = allocMainBit();

        // Emit a COND that raises its done_mask when the predicate's truth equals
        // @p runWhenTrue, plus (when non-empty) the branch body gated on a go bit
        // and a join that raises condDone.  An empty branch's COND raises
        // condDone directly.  Returns true if this branch contributed a body.
        auto emitBranch = [&](DGraphChildRole role, bool runWhenTrue) -> bool {
            const std::vector<const CompiledNode*> bodyNodes =
                collectControlBody(dg, cond.id, role);
            const bool hasBody = !bodyNodes.empty();
            // mapRp1Condition.op makes compare == truth(cond); the then branch
            // (run-when-true) keeps it, the else branch (run-when-false) inverts.
            const rp1_condop_t op = runWhenTrue ? c.op : fpga::invertRp1Op(c.op);

            ResetDomainEmitter branchDomain(image, nextBodyBkt);
            BarrierRef goBit;
            if (hasBody) {
                goBit = branchDomain.define(cond.id + (runWhenTrue ? ".then_go" : ".else_go"));
            }

            rp1_node_t cnode{};
            cnode.opcode               = RP1_OP_COND;
            cnode.status               = RP1_NODE_PENDING;
            cnode.barrier_await_bucket = 0;
            cnode.barrier_await_mask   = condAwait;
            cnode.barrier_set_bucket   = 0;
            cnode.barrier_set_mask     = 0;
            cnode.payload.cond.condition_signal   = condSlot;
            cnode.payload.cond.condition_value    = c.value;
            cnode.payload.cond.condition_op       = op;
            cnode.payload.cond.body_start         = 1;  // empty range (start > end):
            cnode.payload.cond.body_end           = 0;  // COND is a pure boolean
            cnode.payload.cond.bucket_clear_start = 1;
            cnode.payload.cond.bucket_clear_end   = 0;
            const std::size_t condIdx = image.nodes.size();
            image.nodes.push_back(cnode);

            if (!hasBody) {
                auto& cp = image.nodes[condIdx].payload.cond;
                cp.done_bucket = 0;
                cp.done_mask   = condDone;  // taken-but-empty branch resolves directly
                return false;
            }

            auto& cp = image.nodes[condIdx].payload.cond;
            cp.done_bucket = goBit.bucket;
            cp.done_mask   = goBit.mask;  // raised when this branch is taken

            // Root nodes (no intra-body dep) gate on the go bit so the body runs
            // only when the COND raised it.
            auto branchAwait = [&](const std::vector<std::string>& deps) -> BarrierRef {
                auto refs = branchDomain.refsFor(deps);
                if (refs.empty()) refs.push_back(goBit);
                return branchDomain.awaitFor(refs);
            };

            for (const CompiledNode* bnp : bodyNodes) {
                const CompiledNode& bn = *bnp;
                if (const auto* k = std::get_if<CompiledKernelNode>(&bn)) {
                    if (k->kernel.type != DeviceType::FPGA) {
                        throw std::logic_error(
                            "FpgaDevice: conditional '" + cond.id + "' branch kernel '" +
                            k->kernel.name + "' is not an FPGA kernel");
                    }
                    const BarrierRef aw = branchAwait(k->dependsOn);
                    const BarrierRef done = branchDomain.define(k->id);
                    emitKernelPacket(image, *k, aw.bucket, aw.mask, done.bucket, done.mask);
                } else if (const auto* r = std::get_if<CompiledReprogramNode>(&bn)) {
                    const BarrierRef aw = branchAwait(r->dependsOn);
                    const BarrierRef done = branchDomain.define(r->id);
                    emitReprogramPacket(image, *r, aw.bucket, aw.mask, done.bucket, done.mask);
                } else if (const auto* bb = std::get_if<CompiledBoundaryNode>(&bn)) {
                    if (!bb->scalarCopies.empty() || !bb->bufferCopies.empty()) {
                        throw std::logic_error(
                            "FpgaDevice: conditional '" + cond.id + "' branch boundary '" +
                            bb->id + "' carries data; autonomous conditional outputs are a "
                            "future phase");
                    }
                } else {
                    throw std::logic_error(
                        "FpgaDevice: conditional '" + cond.id + "' branch node '" +
                        compiledNodeId(bn) + "' is unsupported (only FPGA kernels/reprograms)");
                }
            }

            const BarrierRef leaves = branchDomain.mutableLeafAwait();
            rp1_node_t join{};
            join.opcode               = RP1_OP_NOP;
            join.status               = RP1_NODE_PENDING;
            join.barrier_await_bucket = leaves.bucket;
            join.barrier_await_mask   = leaves.mask;
            join.barrier_set_bucket   = 0;
            join.barrier_set_mask     = condDone;  // OR-join: taken branch resolves
            image.nodes.push_back(join);
            return true;
        };

        const bool thenBody = emitBranch(DGraphChildRole::ConditionalThen, /*runWhenTrue=*/true);
        const bool elseBody = emitBranch(DGraphChildRole::ConditionalElse, /*runWhenTrue=*/false);
        if (!thenBody && !elseBody) {
            throw std::logic_error(
                "FpgaDevice: conditional '" + cond.id + "' has no FPGA branch bodies");
        }
        mainBit[cond.id] = condDone;
    }

    FpgaDevice*                                                device_ = nullptr;
    std::shared_ptr<fpga::Rp1Submitter>                       submitter_;
    fpga::Rp1GraphImage                                        image_;
    std::vector<DeferredScalar>                                deferred_;
    std::vector<DeferredLoopTripCount>                         deferredTripCounts_;
    std::vector<DeferredPdi>                                   deferredPdis_;
    std::vector<DeferredBufferAddress>                         deferredBufferAddresses_;
    std::vector<DeferredBufferAlias>                           deferredBufferAliases_;
    std::shared_ptr<std::map<std::string, std::uint64_t>>      scalarValues_;
    std::uint32_t                                              sentinelSlot_;
    std::uint32_t                                              sentinelValue_;
    std::chrono::milliseconds                                  timeout_;
    std::thread                                                worker_;
    std::exception_ptr                                         workerEx_;
    std::vector<rp1_cq_entry_t>                                lastCq_;
    bool                                                       signalsPrepared_ = false;
    // Output scalars captured into RP1 signal slots by SCALAR_READ during
    // control-image lowering, keyed by the bound scalar's scoped name so a
    // downstream condition (Phase F) can locate the slot to evaluate.
    std::unordered_map<std::string, std::uint32_t> scalarSlots_;

   public:
    const std::unordered_map<std::string, std::uint32_t>& scalarSlots() const noexcept {
        return scalarSlots_;
    }
};

// =========================================================================
// FpgaDevice
// =========================================================================

FpgaDevice::FpgaDevice(std::string                       id,
                        std::shared_ptr<fpga::Rp1BarWindow> window,
                        FpgaKernelLocationLookup           lookup,
                        std::uint32_t                      cq_size)
    : id_(std::move(id)),
      window_(std::move(window)),
      lookup_(std::move(lookup)),
      nextBufferOffset_(kBufferArenaStart) {
    if (!window_) {
        throw std::invalid_argument("FpgaDevice: window must not be null");
    }
    if (!lookup_) {
        throw std::invalid_argument("FpgaDevice: kernel-location lookup must not be null");
    }
    submitter_ = std::make_shared<fpga::Rp1Submitter>(*window_, cq_size);
}

FpgaDevice::FpgaDevice(std::string                       id,
                       std::shared_ptr<fpga::Rp1BarWindow> window,
                       std::shared_ptr<fpga::FpgaVbinSpec> vbinSpec,
                       std::string                       initialImageId,
                       std::uint32_t                     cq_size)
    : id_(std::move(id)),
      window_(std::move(window)),
      vbinSpec_(std::move(vbinSpec)),
      nextBufferOffset_(kBufferArenaStart),
      activeImageId_(std::move(initialImageId)) {
    if (!window_) {
        throw std::invalid_argument("FpgaDevice: window must not be null");
    }
    if (!vbinSpec_ || vbinSpec_->empty()) {
        throw std::invalid_argument("FpgaDevice: vbin spec must contain at least one image");
    }
    // An empty initial image is valid and is the default for the authoring API:
    // the user region starts with no active image, so every FPGA dispatch must
    // be gated behind an explicit reprogram (PDI_LOAD) of its image. A
    // non-empty initial image must still exist in the spec.
    if (!activeImageId_.empty() && !vbinSpec_->hasImage(activeImageId_)) {
        throw std::invalid_argument(
            "FpgaDevice: initial image '" + activeImageId_ + "' is not in the vbin spec");
    }
    submitter_ = std::make_shared<fpga::Rp1Submitter>(*window_, cq_size);
}

FpgaDevice::~FpgaDevice() = default;

void FpgaDevice::setSentinelSlot(std::uint32_t slot) {
    if (slot >= RP1_MAX_SIGNALS) {
        throw std::invalid_argument(
            "FpgaDevice::setSentinelSlot: slot " + std::to_string(slot) +
            " out of range [0, " + std::to_string(RP1_MAX_SIGNALS) + ")");
    }
    sentinelSlot_ = slot;
}

void FpgaDevice::setSentinelValue(std::uint32_t value) {
    sentinelValue_ = value;
}

void FpgaDevice::setWaitTimeout(std::chrono::milliseconds t) {
    waitTimeout_ = t;
}

void FpgaDevice::setPdiStagingDevice(::vrt::Device device) {
    std::lock_guard<std::mutex> lk(pdiMutex_);
    pdiStagingDevice_ = std::make_shared<::vrt::Device>(std::move(device));
    stagedPdis_.clear();
}

std::string FpgaDevice::normalizeBufferKey(const std::string& bufferName) {
    if (bufferName.rfind("scope:", 0) == 0) return bufferName;
    return scopedBufferKey(0, bufferName);
}

void FpgaDevice::aliasBufferKey(const std::string& targetName,
                                const std::string& sourceName) {
    const std::string target = normalizeBufferKey(targetName);
    const std::string source = normalizeBufferKey(sourceName);
    if (target == source) return;
    std::lock_guard<std::mutex> lk(bufferMutex_);
    auto it = buffers_.find(source);
    if (it == buffers_.end()) {
        throw std::runtime_error(
            "FpgaDevice: cannot alias buffer '" + targetName + "' to unallocated source '" +
            sourceName + "' (carried-buffer boundary expects the source staged first)");
    }
    // Share the source's backing (offset/region/mem) so the target token
    // resolves to the same device memory -- a zero-copy carried-buffer alias.
    buffers_[target] = it->second;
    // The region mapping must follow so on-demand reallocation lands in the
    // same place if the target is ever grown.
    auto regionIt = bufferRegion_.find(source);
    if (regionIt != bufferRegion_.end()) {
        bufferRegion_[target] = regionIt->second;
    }
}

FpgaDevice::BufferRecord FpgaDevice::ensureBufferByKey(const std::string& key,
                                                       BufferType type,
                                                       std::size_t sizeBytes) {
    // Caller must hold bufferMutex_.
    auto regionIt = bufferRegion_.find(key);
    const bool deviceMode = (pdiStagingDevice_ != nullptr) &&
                            (regionIt != bufferRegion_.end());

    auto it = buffers_.find(key);
    if (it != buffers_.end() && it->second.capacity >= sizeBytes &&
        ((it->second.mem != nullptr) == deviceMode)) {
        it->second.size = sizeBytes;
        it->second.type = type;
        return it->second;
    }

    BufferRecord rec;
    rec.size = sizeBytes;
    rec.capacity = sizeBytes;
    rec.type = type;

    if (deviceMode) {
        // Allocate in the region the kernel's m_axi master can reach.  vrt
        // buffers are fixed-size, so allocate at least one byte and grow by
        // reallocation when a later size exceeds the current capacity.
        const std::size_t allocBytes = std::max<std::size_t>(sizeBytes, 1u);
        rec.mem = std::make_shared<::vrt::Buffer<std::uint8_t>>(
            *pdiStagingDevice_, allocBytes, regionIt->second);
        rec.capacity = allocBytes;
    } else {
        const std::uint32_t alignedOffset = alignUp(nextBufferOffset_, 64u);
        const std::uint64_t end = static_cast<std::uint64_t>(alignedOffset) +
                                  static_cast<std::uint64_t>(sizeBytes);
        if (end > fpga::Rp1BarWindow::kWindowSize) {
            throw std::out_of_range(
                "FpgaDevice: BAR-backed buffer arena exhausted while allocating '" +
                key + "' (" + std::to_string(sizeBytes) + " bytes)");
        }
        rec.offset = alignedOffset;
        nextBufferOffset_ = static_cast<std::uint32_t>(end);
    }

    buffers_[key] = rec;
    return rec;
}

FpgaDevice::BufferRecord FpgaDevice::ensureBuffer(const GraphBuffer& buffer,
                                                  std::size_t sizeBytes) {
    const std::string key = scopedBufferKey(buffer.scopeId(), buffer.name());
    std::lock_guard<std::mutex> lk(bufferMutex_);
    return ensureBufferByKey(key, buffer.type(), sizeBytes);
}

std::uint64_t FpgaDevice::bufferDeviceAddress(const GraphBuffer& buffer,
                                              std::size_t sizeBytes) {
    const BufferRecord rec = ensureBuffer(buffer, sizeBytes);
    if (rec.mem) {
        return rec.mem->getPhysAddr();
    }
    return RP1_CTRL_PHYS_ADDR + static_cast<std::uint64_t>(rec.offset);
}

void FpgaDevice::populateBufferRegions(const DGraph& dg) {
    auto record = [&](const KernelDescriptor& kernel, const std::string& portName,
                      const GraphBuffer& buffer) {
        auto region = resolveBufferRegion(kernel, portName);
        if (!region) return;
        const std::string key = scopedBufferKey(buffer.scopeId(), buffer.name());
        std::lock_guard<std::mutex> lk(bufferMutex_);
        bufferRegion_[key] = *region;
    };

    std::function<void(const DGraph&)> walk = [&](const DGraph& g) {
        for (const CompiledNode& node : g.nodes) {
            const auto* k = std::get_if<CompiledKernelNode>(&node);
            if (!k) continue;

            for (const BufferPort& port : k->kernel.ioType.inputs) {
                auto it = k->ioMap.inputs().find(port.name);
                if (it != k->ioMap.inputs().end()) {
                    record(k->kernel, port.name, it->second);
                }
            }
            for (const BufferPort& port : k->kernel.ioType.outputs) {
                auto it = k->ioMap.outputs().find(port.name);
                if (it != k->ioMap.outputs().end()) {
                    record(k->kernel, port.name, it->second);
                }
            }
            for (const RWBufferPort& port : k->kernel.ioType.inouts) {
                for (const IOMap::InoutBinding& binding : k->ioMap.inouts()) {
                    if (binding.inPort == port.in.name && binding.outPort == port.out.name) {
                        record(k->kernel, port.in.name, binding.in);
                        record(k->kernel, port.out.name, binding.out);
                    }
                }
            }
        }
        for (const DGraphChild& child : g.childDGraphs) {
            for (const auto& slice : child.dgraphs) {
                if (slice) walk(*slice);
            }
        }
    };
    walk(dg);
}

FpgaKernelLocation FpgaDevice::resolveKernelLocation(const KernelDescriptor& kernel) const {
    if (vbinSpec_) {
        const std::string active = activeImageId();
        const std::string imageId = kernel.image ? *kernel.image : active;
        if (imageId.empty()) {
            throw std::runtime_error(
                "FpgaDevice: no active image available for kernel '" + kernel.name + "'");
        }
        if (kernel.image && !active.empty() && *kernel.image != active) {
            throw std::runtime_error(
                "FpgaDevice: kernel '" + kernel.name + "' requires image '" +
                *kernel.image + "' but active image is '" + active + "'");
        }
        const auto& spec = vbinSpec_->kernel(imageId, kernel.name);
        return FpgaKernelLocation{spec.r5_base_addr, 0};
    }
    if (!lookup_) {
        throw std::runtime_error("FpgaDevice: no kernel-location lookup configured");
    }
    const FpgaKernelLocation loc = lookup_(kernel.name);
    if (loc.r5_base_addr == 0) {
        throw std::runtime_error(
            "FpgaDevice: kernel-location lookup returned r5_base_addr=0 for kernel '" +
            kernel.name + "' (likely unmapped name)");
    }
    return loc;
}

std::map<std::string, std::string>
FpgaDevice::descriptorPortToArgName(const KernelDescriptor& kernel) const {
    std::map<std::string, std::string> out;
    if (!vbinSpec_) {
        return out;
    }
    const std::string active = activeImageId();
    const std::string imageId = kernel.image ? *kernel.image : active;
    if (imageId.empty() || !vbinSpec_->hasImage(imageId)) {
        return out;
    }
    const auto& kernels = vbinSpec_->image(imageId).kernels;
    auto kit = kernels.find(kernel.name);
    if (kit == kernels.end()) {
        return out;
    }
    const IOTypeMap& d = kernel.ioType;  // descriptor (possibly renamed)

    // Map the descriptor's (possibly renamed) ports to the canonical system_map
    // arg names by positional correspondence, grouped only by scalar-vs-buffer.
    //
    // We deliberately do NOT trust the canonical IOTypeMap's input/output buffer
    // categories: the system_map marks every HLS m_axi pointer register as
    // write-only (r=0, w=1, because the *host* writes the pointer address), so
    // ioTypeMapFromFunctionalArgs lumps all buffer pointers into inputs
    // regardless of data-flow direction.  A descriptor that splits ports into
    // input/output by intent would then fail to line up per-category.  Instead
    // we use the spec's idx-ordered `args` (the authoritative argument order)
    // and split scalar vs buffer by the arg type (matching the classification
    // in ioTypeMapFromFunctionalArgs), not by the m_axi port (a buffer may have
    // no connection and an empty port).
    auto isBufferArg = [](const fpga::FpgaKernelArgSpec& arg) {
        std::string t = arg.type;
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return t == "buffer" || t.find('*') != std::string::npos;
    };
    std::vector<std::string> specScalars;
    std::vector<std::string> specBuffers;
    for (const fpga::FpgaKernelArgSpec& arg : kit->second.args) {
        if (isBufferArg(arg)) {
            specBuffers.push_back(arg.name);
        } else {
            specScalars.push_back(arg.name);
        }
    }

    // Flatten the descriptor ports in the order the packer emits them
    // (scalars first, then input/output buffers, then RW in-pointers).
    std::vector<std::string> descScalars;
    std::vector<std::string> descBuffers;
    for (const ScalarPort& p : d.inputScalars)  descScalars.push_back(p.name);
    for (const ScalarPort& p : d.outputScalars) descScalars.push_back(p.name);
    for (const BufferPort& p : d.inputs)  descBuffers.push_back(p.name);
    for (const BufferPort& p : d.outputs) descBuffers.push_back(p.name);
    // An RW pair collapses onto a single underlying pointer arg: its in-port
    // consumes one buffer slot; its out-port aliases the same arg afterwards.
    for (const RWBufferPort& p : d.inouts) descBuffers.push_back(p.in.name);

    for (std::size_t i = 0; i < descScalars.size() && i < specScalars.size(); ++i) {
        out[descScalars[i]] = specScalars[i];
    }
    for (std::size_t i = 0; i < descBuffers.size() && i < specBuffers.size(); ++i) {
        out[descBuffers[i]] = specBuffers[i];
    }
    for (const RWBufferPort& p : d.inouts) {
        auto it = out.find(p.in.name);
        if (it != out.end()) {
            out[p.out.name] = it->second;
        }
    }
    return out;
}

std::uint32_t
FpgaDevice::outputScalarRegOffset(const KernelDescriptor& kernel,
                                  const std::string& portName) const {
    const auto offsets = kernelArgOffsets(kernel);
    auto it = offsets.find(portName);
    if (it != offsets.end()) return it->second;
    // Mock/lookup path (no system_map): a conventional output register offset.
    return 0x10u;
}

std::map<std::string, std::uint32_t>
FpgaDevice::kernelArgOffsets(const KernelDescriptor& kernel) const {
    std::map<std::string, std::uint32_t> offsets;
    if (!vbinSpec_) {
        return offsets;  // mock/lookup path: caller falls back to 0x10 layout
    }
    const std::string active = activeImageId();
    const std::string imageId = kernel.image ? *kernel.image : active;
    if (imageId.empty() || !vbinSpec_->hasImage(imageId)) {
        return offsets;
    }
    const auto& kernels = vbinSpec_->image(imageId).kernels;
    auto kit = kernels.find(kernel.name);
    if (kit == kernels.end()) {
        return offsets;
    }
    std::map<std::string, std::uint32_t> byArgName;
    for (const fpga::FpgaKernelArgSpec& arg : kit->second.args) {
        byArgName[arg.name] = arg.offset;
    }
    // Key offsets by the descriptor's port names so the packer (which iterates
    // node.kernel.ioType) finds them even when ports were renamed.
    const auto trans = descriptorPortToArgName(kernel);
    for (const auto& [descPort, argName] : trans) {
        auto a = byArgName.find(argName);
        if (a != byArgName.end()) {
            offsets[descPort] = a->second;
        }
    }
    // Fallback for descriptors with no IOTypeMap to zip against: key by arg
    // name (preserves behaviour for specs whose port names already match).
    if (offsets.empty()) {
        offsets = std::move(byArgName);
    }
    return offsets;
}

std::optional<::vrt::MemoryConfig>
FpgaDevice::resolveBufferRegion(const KernelDescriptor& kernel,
                                const std::string& portName) const {
    if (!vbinSpec_) {
        return std::nullopt;  // mock/lookup path: caller uses the BAR arena
    }
    const std::string active = activeImageId();
    const std::string imageId = kernel.image ? *kernel.image : active;
    if (imageId.empty() || !vbinSpec_->hasImage(imageId)) {
        return std::nullopt;
    }
    const auto& kernels = vbinSpec_->image(imageId).kernels;
    auto kit = kernels.find(kernel.name);
    if (kit == kernels.end()) {
        return std::nullopt;
    }
    const auto& argMemory = kit->second.argMemory;

    // Translate the (possibly renamed) descriptor port to its system_map arg
    // name, using the same correspondence the offset packer uses so regions
    // and offsets stay consistent.
    const auto trans = descriptorPortToArgName(kernel);
    auto t = trans.find(portName);
    const std::string& argName = (t != trans.end()) ? t->second : portName;

    auto it = argMemory.find(argName);
    if (it != argMemory.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::uint64_t FpgaDevice::stagePdiBytes(const std::string& cacheKey,
                                        const std::vector<std::uint8_t>& bytes) {
    std::lock_guard<std::mutex> lk(pdiMutex_);
    if (pdiStagingDevice_) {
        auto cached = stagedPdis_.find(cacheKey);
        if (cached != stagedPdis_.end()) {
            std::cerr << "[FpgaDevice] stagePdiBytes: reusing DDR/QDMA staged PDI '"
                      << cacheKey << "' (" << cached->second.size
                      << " bytes) @ 0x" << std::hex << cached->second.physAddr
                      << std::dec << std::endl;
            return cached->second.physAddr;
        }

        std::cerr << "[FpgaDevice] stagePdiBytes: allocating DDR/QDMA buffer for '"
                  << cacheKey << "' (" << bytes.size() << " bytes)" << std::endl;
        auto buffer = std::make_unique<::vrt::Buffer<std::uint8_t>>(
            *pdiStagingDevice_, bytes.size(), ::vrt::MemoryRangeType::DDR);
        std::cerr << "[FpgaDevice] stagePdiBytes: DDR/QDMA buffer allocated"
                  << " @ 0x" << std::hex << buffer->getPhysAddr()
                  << std::dec << std::endl;
        if (!bytes.empty()) {
            std::cerr << "[FpgaDevice] stagePdiBytes: copying to host staging buffer" << std::endl;
            std::memcpy(buffer->get(), bytes.data(), bytes.size());
            std::cerr << "[FpgaDevice] stagePdiBytes: QDMA sync HOST_TO_DEVICE start" << std::endl;
            buffer->sync(::vrt::SyncType::HOST_TO_DEVICE);
            std::cerr << "[FpgaDevice] stagePdiBytes: QDMA sync HOST_TO_DEVICE complete" << std::endl;
        }
        const std::uint64_t phys = buffer->getPhysAddr();
        std::cerr << "[FpgaDevice] stagePdiBytes: staged " << bytes.size()
                  << " bytes via QDMA DDR @ 0x" << std::hex << phys
                  << std::dec << std::endl;
        stagedPdis_.emplace(cacheKey, StagedPdiRecord{std::move(buffer), phys, bytes.size()});
        return phys;
    }

    const std::string name = "__pdi_" + std::to_string(std::hash<std::string>{}(cacheKey));
    GraphBuffer token = GraphBuffer::make(BufferType::U8, name, 0);
    const BufferRecord rec = ensureBuffer(token, bytes.size());
    std::cerr << "[FpgaDevice] stagePdiBytes: WARNING no QDMA staging device configured; "
              << "falling back to BAR/RP1 window staging for " << bytes.size()
              << " bytes -> BAR window offset 0x" << std::hex << rec.offset
              << std::dec << std::endl;
    if (!bytes.empty()) {
        window_->writeAt(rec.offset, bytes.data(), bytes.size());
    }
    std::cerr << "[FpgaDevice] stagePdiBytes: BAR write complete" << std::endl;
    return RP1_CTRL_PHYS_ADDR + static_cast<std::uint64_t>(rec.offset);
}

std::uint64_t FpgaDevice::stagePdiFile(const std::string& pdiPath) {
    std::cerr << "[FpgaDevice] stagePdiFile: opening " << pdiPath << std::endl;
    std::ifstream in(pdiPath, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("FpgaDevice: cannot open PDI file '" + pdiPath + "'");
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        throw std::runtime_error("FpgaDevice: failed to size PDI file '" + pdiPath + "'");
    }
    std::cerr << "[FpgaDevice] stagePdiFile: file size is " << size << " bytes" << std::endl;
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::cerr << "[FpgaDevice] stagePdiFile: reading PDI into host memory" << std::endl;
    if (!bytes.empty() &&
        !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("FpgaDevice: short read while staging PDI file '" + pdiPath + "'");
    }
    std::cerr << "[FpgaDevice] stagePdiFile: host read complete" << std::endl;

    return stagePdiBytes(pdiPath, bytes);
}

void FpgaDevice::setActiveImage(std::string imageId) {
    if (vbinSpec_ && !vbinSpec_->hasImage(imageId)) {
        throw std::runtime_error("FpgaDevice: cannot activate unknown image '" + imageId + "'");
    }
    std::lock_guard<std::mutex> lk(imageMutex_);
    activeImageId_ = std::move(imageId);
}

std::string FpgaDevice::activeImageId() const {
    std::lock_guard<std::mutex> lk(imageMutex_);
    return activeImageId_;
}

std::uint32_t FpgaDevice::imageNumericId(const std::string& imageId) const {
    if (!vbinSpec_ || imageId.empty()) {
        return 0;  // mock/lookup path or no image: guard disabled
    }
    // 1-based index in the spec's (name-sorted) image map. The id only needs
    // to be self-consistent within this device: firmware compares equality of
    // whatever the host wrote on PDI_LOAD vs KERNEL_DISPATCH.
    std::uint32_t id = 1;
    for (const auto& [name, spec] : vbinSpec_->images()) {
        (void)spec;
        if (name == imageId) return id;
        ++id;
    }
    return 0;  // unknown image: leave unguarded rather than mis-gate
}

void FpgaDevice::setInputBuffer(const std::string& bufferName,
                                const void*        data,
                                std::size_t        sizeBytes) {
    const std::string key = normalizeBufferKey(bufferName);
    std::lock_guard<std::mutex> lk(bufferMutex_);

    BufferType type = BufferType::U8;
    if (auto existing = buffers_.find(key); existing != buffers_.end()) {
        type = existing->second.type;
    }
    const BufferRecord rec = ensureBufferByKey(key, type, sizeBytes);

    if (sizeBytes == 0) return;
    if (rec.mem) {
        // Device-memory mode: stage into the host mapping and DMA to the
        // region the kernel reads from.
        if (data) {
            std::memcpy(rec.mem->get(), data, sizeBytes);
        } else {
            std::memset(rec.mem->get(), 0, sizeBytes);
        }
        rec.mem->sync(::vrt::SyncType::HOST_TO_DEVICE);
    } else if (data) {
        window_->writeAt(rec.offset, data, sizeBytes);
    } else {
        window_->zeroAt(rec.offset, sizeBytes);
    }
}

void FpgaDevice::getOutputBuffer(const std::string& bufferName,
                                 void*              data,
                                 std::size_t        sizeBytes) const {
    if (sizeBytes == 0) return;
    if (!data) {
        throw std::invalid_argument("FpgaDevice::getOutputBuffer: data must not be null");
    }

    const std::string key = normalizeBufferKey(bufferName);
    std::lock_guard<std::mutex> lk(bufferMutex_);
    auto it = buffers_.find(key);
    if (it == buffers_.end()) {
        throw std::runtime_error("FpgaDevice::getOutputBuffer: unknown buffer '" + bufferName + "'");
    }
    if (sizeBytes > it->second.size) {
        throw std::out_of_range(
            "FpgaDevice::getOutputBuffer: requested " + std::to_string(sizeBytes) +
            " bytes but buffer '" + bufferName + "' holds " +
            std::to_string(it->second.size));
    }
    if (it->second.mem) {
        // Device-memory mode: DMA the kernel's results back before copying out.
        it->second.mem->sync(::vrt::SyncType::DEVICE_TO_HOST);
        std::memcpy(data, it->second.mem->get(), sizeBytes);
    } else {
        window_->readAt(it->second.offset, data, sizeBytes);
    }
}

std::size_t FpgaDevice::bufferSize(const std::string& bufferName) const {
    const std::string key = normalizeBufferKey(bufferName);
    std::lock_guard<std::mutex> lk(bufferMutex_);
    auto it = buffers_.find(key);
    return (it == buffers_.end()) ? 0 : it->second.size;
}

std::unique_ptr<IDevicePlan> FpgaDevice::compilePlan(const DGraph& dg) {
    // Resolve each kernel buffer's m_axi memory region up front so later
    // allocation (which may happen at bridge-consumer time, before the kernel
    // is packed) lands where the kernel master can reach it.
    populateBufferRegions(dg);

    // -------------------------------------------------------------------
    // Validate node variants. The actual lowering happens in FpgaDevicePlan;
    // here we only reject the cases it cannot handle, with a clear diagnostic.
    // All other variants (bridge / reprogram / loop / conditional / signal /
    // wait) are lowered by the plan.
    // -------------------------------------------------------------------
    for (const CompiledNode& node : dg.nodes) {
        std::visit(
            [&](const auto& concrete) {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, CompiledKernelNode>) {
                    if (concrete.kernel.type != DeviceType::FPGA) {
                        throw std::logic_error(
                            std::string("FpgaDevice: kernel '") +
                            concrete.kernel.name +
                            "' has DeviceType::" + deviceTypeName(concrete.kernel.type) +
                            "; expected FPGA");
                    }
                } else if constexpr (std::is_same_v<T, CompiledBoundaryNode>) {
                    throw std::logic_error(
                        std::string("FpgaDevice: top-level graph-region boundaries are not "
                                    "yet supported, got '") + concrete.id + "'");
                }
            },
            node);
    }

    if (dg.nodes.empty()) {
        throw std::logic_error("FpgaDevice: DGraph has no nodes to compile");
    }
    return std::make_unique<FpgaDevicePlan>(*this,
                                            dg,
                                            dg.scalarValues,
                                            sentinelSlot_,
                                            sentinelValue_,
                                            waitTimeout_);

}

}  // namespace vrt::graph
