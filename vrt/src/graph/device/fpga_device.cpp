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
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/backend_resource_binding.hpp>
#include <vrt/graph/backend_runtime.hpp>
#include <vrt/graph/device/fpga/control_lowering.hpp>
#include <vrt/graph/device/fpga/rp1_lowering.hpp>
#include <vrt/graph/device/fpga/vbin_spec.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>
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
    alignUp(RP1_DEFAULT_TRACE_OFFSET +
                fpga::kDefaultTraceSize * sizeof(rp1_trace_entry_t),
            4096u);

std::mutex& rp1DiagnosticMutex() {
    static std::mutex mutex;
    return mutex;
}

std::mutex& poisonedDeviceQuarantineMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<FpgaDevice>>&
poisonedDeviceQuarantine() {
    // ponytail: quarantine grows until process exit; a future reset/recovery
    // acknowledgement API can release entries after hardware is known idle.
    static std::vector<std::shared_ptr<FpgaDevice>> devices;
    return devices;
}

class FpgaExecutionLease final : public IDeviceExecutionLease {
   public:
    FpgaExecutionLease(
        std::atomic_bool& leased,
        const std::atomic_bool& poisoned)
        : leased_(&leased), poisoned_(&poisoned) {}

    ~FpgaExecutionLease() override {
        if (!poisoned_->load(std::memory_order_acquire)) {
            leased_->store(false, std::memory_order_release);
        }
    }

   private:
    std::atomic_bool*       leased_;
    const std::atomic_bool* poisoned_;
};

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

bool scalarFitsSignalSlot(ScalarType t) {
    return scalarWidthInWords(t) == 1u;
}

std::string scalarTypeName(ScalarType t) {
    switch (t) {
        case ScalarType::U8:  return "U8";
        case ScalarType::U16: return "U16";
        case ScalarType::U32: return "U32";
        case ScalarType::U64: return "U64";
        case ScalarType::I8:  return "I8";
        case ScalarType::I16: return "I16";
        case ScalarType::I32: return "I32";
        case ScalarType::I64: return "I64";
        case ScalarType::F32: return "F32";
        case ScalarType::F64: return "F64";
    }
    return "?";
}

void requireSignalSlotScalar(const KernelDescriptor& kernel, const ScalarPort& port) {
    if (scalarFitsSignalSlot(port.type)) return;
    throw std::runtime_error(
        "FpgaDevice: output scalar '" + port.name + "' on kernel '" + kernel.name +
        "' has type " + scalarTypeName(port.type) +
        ", but RP1 scalar-read signal slots carry only 32-bit values");
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

std::string memoryRegionTag(const ::vrt::MemoryConfig& region) {
    switch (region.type) {
        case ::vrt::MemoryRangeType::HBM:
            return "HBM" + std::to_string(region.hbmPort.value_or(0));
        case ::vrt::MemoryRangeType::HBM_VNOC:
            return "HBM";
        case ::vrt::MemoryRangeType::DDR:
            return "DDR";
    }
    return "UNKNOWN";
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
    std::size_t gateNodeIndex = 0;
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

using DirectAuthoredIndex =
    std::map<NodeId, const AuthoredOperation*>;

void indexDirectAuthored(
    const AuthoredRegion& region, DirectAuthoredIndex& result) {
    for (const AuthoredOperation& operation : region.operations) {
        result[authoredNodeId(operation)] = &operation;
        if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
            if (loop->body) indexDirectAuthored(*loop->body, result);
        } else if (const auto* conditional =
                       std::get_if<AuthoredConditional>(&operation)) {
            if (conditional->thenRegion) {
                indexDirectAuthored(*conditional->thenRegion, result);
            }
            if (conditional->elseRegion) {
                indexDirectAuthored(*conditional->elseRegion, result);
            }
        }
    }
}

std::optional<std::uint32_t> directPhysical(
    const BackendLoweringContext& context, RendezvousId logical) {
    const BoundRendezvous* binding = context.resources.find(logical);
    return binding
               ? std::optional<std::uint32_t>(
                     static_cast<std::uint32_t>(
                         binding->physical.value()))
               : std::nullopt;
}

struct DirectControlProtocol {
    Rp1SplitRole          role = Rp1SplitRole::None;
    DeviceId                    owner;
    std::optional<std::uint32_t> value;
    std::optional<std::uint32_t> decision;
    std::optional<std::uint32_t> acknowledgement;
};

/*
 * Split controls use one physical slot triplet per authority/follower pair.
 * Non-participating queues keep role None; participating queues must see one
 * resource owner so later image lowering can reserve the triplet atomically.
 */
DirectControlProtocol directControlProtocol(
    const BackendLoweringContext& context, NodeId control) {
    for (const SplitControlProtocol& protocol :
         context.scheduled.splitControls()) {
        if (protocol.control != control) continue;
        const SplitControlFollowerProtocol* follower = nullptr;
        DirectControlProtocol result;
        if (context.queue.id == protocol.authorityQueue &&
            !protocol.followers.empty()) {
            follower = &protocol.followers.front();
            result.role = Rp1SplitRole::Authority;
        } else {
            auto match = std::find_if(
                protocol.followers.begin(), protocol.followers.end(),
                [&](const SplitControlFollowerProtocol& candidate) {
                    return candidate.queue == context.queue.id;
                });
            if (match != protocol.followers.end()) {
                follower = &*match;
                result.role = Rp1SplitRole::Follower;
            }
        }
        if (!follower) return result;
        result.value = directPhysical(context, follower->value);
        result.decision = directPhysical(context, follower->decision);
        result.acknowledgement =
            directPhysical(context, follower->acknowledgement);
        for (RendezvousId logical :
             {follower->value, follower->decision,
              follower->acknowledgement}) {
            const BoundRendezvous* binding =
                context.resources.find(logical);
            if (!binding) continue;
            if (result.owner.empty()) {
                result.owner = binding->owner;
            } else if (result.owner != binding->owner) {
                throw std::logic_error(
                    "Rp1Lowering: split-control resources have "
                    "multiple owners");
            }
        }
        return result;
    }
    return {};
}

}  // namespace

namespace {

/*
 * Translate one scheduled queue slice into typed RP1 commands.  This pass keeps
 * graph tokens and dependencies symbolic; BAR addresses, signal forwarding,
 * barriers, and packet layout belong to the later image-building pass.
 */
class Rp1QueueLowerer {
   public:
    explicit Rp1QueueLowerer(
        const BackendLoweringContext& context)
        : context_(context) {
        indexDirectAuthored(
            context.scheduled.routed()
                .placed().resolved().authored().root(),
            authored_);
        indexCopyTargets();
    }

    std::shared_ptr<Rp1QueueProgram> lower() {
        /*
         * Keep runtime scalar storage shared with sibling queue executables.
         * Leased physical scalar slots are copied only for this queue's owner.
         */
        program_ = std::make_shared<Rp1QueueProgram>();
        program_->device = context_.queue.device;
        program_->scalarValues =
            context_.runtimeState->scalarValues();
        program_->resourcesLeased = true;
        for (const auto& [logical, scalar] :
             context_.resources.scalars()) {
            (void)logical;
            if (scalar.owner == context_.queue.device) {
                program_->scalarResources[scalar.key] =
                    scalar.physical;
            }
        }

        /*
         * Emit typed commands before wiring edges: elided host events and
         * unsupported scheduled payloads may make a dependency indirect.
         */
        emitSteps();
        wireDependencies();
        return program_;
    }

   private:
    using EmittedStep =
        std::pair<std::string, std::string>;
    using CopyTargetKey = std::pair<NodeId, ValueId>;

    std::string authoredId(NodeId id) const {
        auto operation = authored_.find(id);
        return operation == authored_.end()
                   ? "node_" + std::to_string(id.value())
                   : authoredSourceId(*operation->second);
    }

    DeviceId valueDevice(ValueId value) const {
        const ValueReplica* replica =
            context_.scheduled.routed().placed().primaryReplica(value);
        return replica ? replica->memory.device
                       : context_.queue.device;
    }

    std::optional<ValueId> routeValue(
        const TransferRoute& route) const {
        const std::optional<ReplicaId> replica =
            transferReplica(route.requirement.signature.source);
        const ValueReplica* source =
            replica
                ? context_.scheduled.routed().findReplica(*replica)
                : nullptr;
        return source ? std::optional<ValueId>(source->value)
                      : std::nullopt;
    }

    /*
     * Host-mediated or isolated transfers need a distinct destination token.
     * Index every consumer reached by the route so kernel bindings point at
     * copied storage rather than accidentally aliasing the source replica.
     */
    void indexCopyTargets() {
        for (const TransferRoute& route :
             context_.scheduled.routed().routes()) {
            if (route.legs.empty() ||
                (route.legs.front().mechanism !=
                     TransferMechanism::HostMediatedDeviceCopy &&
                 !route.requirement.isolatedDestination)) {
                continue;
            }
            const std::optional<ValueId> valueId =
                routeValue(route);
            const std::optional<NodeId> destination =
                route.requirement.destinationAnchor.operation();
            const ResolvedValue* value =
                valueId
                    ? context_.scheduled.routed().placed()
                          .resolved().findValue(*valueId)
                    : nullptr;
            const GraphBuffer* source =
                value ? resolvedBufferToken(*value) : nullptr;
            if (!valueId || !destination || !source) continue;
            GraphBuffer target = ::vrt::graph::detail::makeGraphBuffer(
                source->type(),
                source->name() + "__route_" +
                    std::to_string(route.id.value()),
                source->scopeId(), source->maybeSizeScalar(),
                source->graphId());
            copyTargets_[{*destination, *valueId}] = target;
            for (const DependencyEdge& edge :
                 context_.scheduled.routed().dependencies()) {
                const auto* data =
                    std::get_if<ValueDependencyEdge>(&edge);
                const std::optional<NodeId> consumer =
                    dependencyConsumer(edge);
                if (!data || !consumer ||
                    dependencyRoute(edge) !=
                        std::optional<RouteId>(route.id)) {
                    continue;
                }
                const ValueReplica* replica =
                    context_.scheduled.routed().findReplica(
                        data->target);
                if (replica) {
                    copyTargets_[{*consumer, replica->value}] =
                        target;
                }
            }
        }
    }

    /*
     * A loop result publishes the backedge value into its parent token.
     * Record buffer aliases separately from scalar publications: buffers can
     * remain zero-copy, while scalars later move through RP1 signal slots.
     */
    void fillLoopOutputs(NodeId control, Rp1LoopCommand& node) const {
        const PlacedGraph& placed =
            context_.scheduled.routed().placed();
        for (const ResolvedControlResult& result :
             placed.resolved().controlResults()) {
            if (result.control != control) continue;
            const ResolvedValue* target =
                placed.resolved().findValue(result.result);
            auto incoming = std::find_if(
                result.incoming.begin(), result.incoming.end(),
                [](const ControlIncoming& value) {
                    return value.arm == ControlArm::LoopBackedge;
                });
            const ResolvedValue* source =
                incoming == result.incoming.end()
                    ? nullptr
                    : placed.resolved().findValue(incoming->value);
            if (!target || !source) continue;
            const std::string device =
                valueDevice(result.result).value();
            if (const GraphBuffer* targetToken =
                    resolvedBufferToken(*target)) {
                const GraphBuffer* sourceToken =
                    resolvedBufferToken(*source);
                if (!sourceToken) continue;
                node.outputBufferPlacements[scopedBufferKey(
                    targetToken->scopeId(),
                    targetToken->name())] = device;
                node.outputBufferPublications.push_back({
                    result.port.value(),
                    targetToken->name(), targetToken->scopeId(),
                    sourceToken->name(), sourceToken->scopeId(),
                    device});
            } else {
                const GraphScalar* scalarTarget =
                    resolvedScalarToken(*target);
                const GraphScalar* sourceToken =
                    resolvedScalarToken(*source);
                if (!scalarTarget || !sourceToken) continue;
                node.outputScalarPlacements[scopedScalarKey(
                    scalarTarget->scopeId(),
                    scalarTarget->varName())] = device;
                node.outputScalarPublications.push_back({
                    result.port.value(),
                    scalarTarget->varName(), scalarTarget->scopeId(),
                    sourceToken->varName(), sourceToken->scopeId(),
                    device});
            }
        }
    }

    /*
     * Conditional results have exactly one then and one else source.  Preserve
     * both scoped tokens here; branch selection happens only when the RP1 image
     * is built, after predicate placement and child programs are known.
     */
    void fillConditionalOutputs(
        NodeId control, Rp1ConditionalCommand& node) const {
        const PlacedGraph& placed =
            context_.scheduled.routed().placed();
        for (const ResolvedControlResult& result :
             placed.resolved().controlResults()) {
            if (result.control != control) continue;
            auto thenIncoming = std::find_if(
                result.incoming.begin(), result.incoming.end(),
                [](const ControlIncoming& value) {
                    return value.arm == ControlArm::ThenBranch;
                });
            auto elseIncoming = std::find_if(
                result.incoming.begin(), result.incoming.end(),
                [](const ControlIncoming& value) {
                    return value.arm == ControlArm::ElseBranch;
                });
            if (thenIncoming == result.incoming.end() ||
                elseIncoming == result.incoming.end()) {
                continue;
            }
            const ResolvedValue* target =
                placed.resolved().findValue(result.result);
            const ResolvedValue* thenValue =
                placed.resolved().findValue(thenIncoming->value);
            const ResolvedValue* elseValue =
                placed.resolved().findValue(elseIncoming->value);
            if (!target || !thenValue || !elseValue) continue;
            const std::string device =
                valueDevice(result.result).value();
            if (const GraphBuffer* targetToken =
                    resolvedBufferToken(*target)) {
                const GraphBuffer* thenToken =
                    resolvedBufferToken(*thenValue);
                const GraphBuffer* elseToken =
                    resolvedBufferToken(*elseValue);
                if (!thenToken || !elseToken) continue;
                node.outputBufferPlacements[scopedBufferKey(
                    targetToken->scopeId(),
                    targetToken->name())] = device;
                node.outputBufferPublications.push_back({
                    result.port.value(),
                    targetToken->name(), targetToken->scopeId(),
                    thenToken->name(), thenToken->scopeId(), device,
                    elseToken->name(), elseToken->scopeId(), device});
            } else {
                const GraphScalar* scalarTarget =
                    resolvedScalarToken(*target);
                const GraphScalar* thenToken =
                    resolvedScalarToken(*thenValue);
                const GraphScalar* elseToken =
                    resolvedScalarToken(*elseValue);
                if (!scalarTarget || !thenToken || !elseToken) continue;
                node.outputScalarPlacements[scopedScalarKey(
                    scalarTarget->scopeId(),
                    scalarTarget->varName())] = device;
                node.outputScalarPublications.push_back({
                    result.port.value(),
                    scalarTarget->varName(), scalarTarget->scopeId(),
                    thenToken->varName(), thenToken->scopeId(), device,
                    elseToken->varName(), elseToken->scopeId(), device});
            }
        }
    }

    /*
     * Authored operations become one typed command each.  Kernels may rebind
     * routed inputs, controls retain publication/broadcast metadata, and
     * operations with no RP1 representation are deliberately elided.
     */
    std::vector<Rp1Command> operationNodes(
        const ScheduledOperation& scheduled) const {
        const AuthoredOperation& operation =
            *authored_.at(scheduled.operation);
        return std::visit(
            [&](const auto& concrete) -> std::vector<Rp1Command> {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, AuthoredKernel>) {
                    Rp1KernelCommand node;
                    node.id = concrete.authoredId;
                    node.deviceId = context_.queue.device.value();
                    node.kernel = concrete.kernel;
                    node.ioMap = concrete.ioMap;
                    const ResolvedOperation* resolved =
                        context_.scheduled.routed().placed()
                            .resolved().findOperation(concrete.id);
                    if (resolved) {
                        for (const ResolvedBinding& binding :
                             resolved->bindings) {
                            auto target = copyTargets_.find(
                                {concrete.id, binding.value});
                            if (target != copyTargets_.end() &&
                                binding.access ==
                                    ValueAccess::Input) {
                                node.ioMap.rebindInputForCompiler(
                                    binding.localPort.value(),
                                    target->second);
                            } else if (
                                target != copyTargets_.end() &&
                                binding.access ==
                                    ValueAccess::InoutInput) {
                                node.ioMap
                                    .rebindInoutInputForCompiler(
                                        binding.localPort.value(),
                                        target->second);
                            }
                        }
                    }
                    return {std::move(node)};
                } else if constexpr (
                    std::is_same_v<T, AuthoredReprogram>) {
                    Rp1ReprogramCommand node;
                    node.id = concrete.authoredId;
                    node.deviceId = context_.queue.device.value();
                    node.imageId = concrete.imageId;
                    node.pdiPath = concrete.pdiPath;
                    node.timeoutCycles = concrete.timeoutCycles;
                    return {std::move(node)};
                } else if constexpr (
                    std::is_same_v<T, AuthoredLoop>) {
                    Rp1LoopCommand node;
                    node.id = concrete.authoredId;
                    node.deviceId = context_.queue.device.value();
                    node.loopKind =
                        concrete.kind == LoopKind::FixedCount
                            ? Rp1LoopKind::FixedCount
                            : Rp1LoopKind::WhileCondition;
                    node.tripCount = concrete.tripCount;
                    node.condition = concrete.condition;
                    const DirectControlProtocol protocol =
                        directControlProtocol(context_, concrete.id);
                    node.broadcastRole = protocol.role;
                    node.broadcastResourceOwner = protocol.owner;
                    node.conditionBroadcastSlot =
                        protocol.value.value_or(0);
                    node.broadcastReadySlot =
                        protocol.decision.value_or(0);
                    node.broadcastAckSlot =
                        protocol.acknowledgement.value_or(0);
                    fillLoopOutputs(concrete.id, node);
                    return {std::move(node)};
                } else if constexpr (
                    std::is_same_v<T, AuthoredConditional>) {
                    Rp1ConditionalCommand node;
                    node.id = concrete.authoredId;
                    node.deviceId = context_.queue.device.value();
                    node.condition = concrete.condition;
                    fillConditionalOutputs(concrete.id, node);
                    return {std::move(node)};
                } else {
                    return {};
                }
            },
            operation);
    }

    const LogicalRendezvous* logical(
        RendezvousId id) const {
        auto found = std::find_if(
            context_.scheduled.rendezvous().begin(),
            context_.scheduled.rendezvous().end(),
            [&](const LogicalRendezvous& candidate) {
                return candidate.id == id;
            });
        return found == context_.scheduled.rendezvous().end()
                   ? nullptr
                   : &*found;
    }

    /*
     * Device-owned rendezvous become SIGNAL or WAIT+clear command sequences.
     * Host events stay outside the image; pre-launch data waits are marked so
     * the host can wait before submitting an otherwise deadlocked RP1 graph.
     */
    std::vector<Rp1Command> eventNodes(
        const ScheduledStep& step) const {
        const auto* publish =
            std::get_if<ScheduledEventPublish>(&step.payload);
        const auto* wait =
            std::get_if<ScheduledEventWait>(&step.payload);
        if (!publish && !wait) return {};
        const RendezvousId id =
            publish ? publish->rendezvous : wait->rendezvous;
        const BoundRendezvous* binding =
            context_.resources.find(id);
        if (!binding ||
            binding->kind == PhysicalRendezvousKind::HostEvent) {
            return {};
        }
        const std::string stem =
            "__event_" + std::to_string(id.value()) +
            "_step_" + std::to_string(step.id.value());
        if (publish) {
            Rp1SignalCommand signal;
            signal.id = stem + "_set";
            signal.deviceId = context_.queue.device.value();
            signal.slot = static_cast<std::uint32_t>(
                binding->physical.value());
            signal.resourceOwner = binding->owner;
            signal.value = 1;
            signal.operation = RP1_SIGOP_SET;
            return {std::move(signal)};
        }
        Rp1WaitCommand waitNode;
        waitNode.id = stem + "_wait";
        waitNode.deviceId = context_.queue.device.value();
        waitNode.slot = static_cast<std::uint32_t>(
            binding->physical.value());
        waitNode.resourceOwner = binding->owner;
        waitNode.value = 1;
        waitNode.conditionOp = RP1_COP_AND_NZ;
        const LogicalRendezvous* rendezvous = logical(id);
        waitNode.preLaunch =
            rendezvous &&
            std::visit(
                [](const auto& payload) {
                    using T = std::decay_t<decltype(payload)>;
                    if constexpr (
                        std::is_same_v<T, DataReadyRendezvous> ||
                        std::is_same_v<T, DataConsumedRendezvous>) {
                        return payload.phase ==
                               TransferPhase::PreLaunch;
                    }
                    return false;
                },
                rendezvous->payload);
        Rp1SignalCommand clear;
        clear.id = stem + "_clear";
        clear.deviceId = context_.queue.device.value();
        clear.slot = waitNode.slot;
        clear.resourceOwner = binding->owner;
        clear.value = 0;
        clear.operation = RP1_SIGOP_SET;
        clear.dependsOn = {waitNode.id};
        return {std::move(waitNode), std::move(clear)};
    }

    /*
     * Region boundaries carry scoped token identity across control children.
     * They are metadata commands, not firmware packets: image lowering turns
     * buffer copies into aliases and scalar copies into carried-slot wiring.
     */
    std::vector<Rp1Command> boundaryNodes(
        const ScheduledBoundaryMaterialization& boundary) const {
        const AuthoredOperation* operation =
            authored_.at(boundary.boundary);
        const auto* authoredBoundary =
            std::get_if<AuthoredBoundary>(operation);
        if (!authoredBoundary) return {};
        Rp1BoundaryCommand node;
        node.id = authoredBoundary->authoredId;
        node.deviceId = context_.queue.device.value();
        node.side =
            authoredBoundary->side == BoundarySide::Start
                ? Rp1BoundaryCommand::Side::Start
                : Rp1BoundaryCommand::Side::End;
        const PlacedGraph& placed =
            context_.scheduled.routed().placed();
        for (const ScheduledBoundaryMapping& mapping :
             boundary.mappings) {
            const ValueReplica* sourceReplica =
                placed.findReplica(mapping.source);
            const ValueReplica* targetReplica =
                placed.findReplica(mapping.target);
            const ResolvedValue* source =
                sourceReplica
                    ? placed.resolved().findValue(
                          sourceReplica->value)
                    : nullptr;
            const ResolvedValue* target =
                targetReplica
                    ? placed.resolved().findValue(
                          targetReplica->value)
                    : nullptr;
            if (!source || !target) continue;
            if (const GraphBuffer* sourceToken =
                    resolvedBufferToken(*source)) {
                const GraphBuffer* targetToken =
                    resolvedBufferToken(*target);
                if (targetToken) {
                    node.bufferCopies.push_back({
                        sourceToken->name(), sourceToken->scopeId(),
                        targetToken->name(), targetToken->scopeId()});
                }
            } else if (const GraphScalar* sourceToken =
                           resolvedScalarToken(*source)) {
                const GraphScalar* targetToken =
                    resolvedScalarToken(*target);
                if (targetToken) {
                    node.scalarCopies.push_back({
                        sourceToken->varName(),
                        sourceToken->scopeId(),
                        targetToken->varName(),
                        targetToken->scopeId()});
                }
            }
        }
        return {std::move(node)};
    }

    std::vector<Rp1Command> nodesFor(
        const ScheduledStep& step) const {
        if (const auto* operation =
                std::get_if<ScheduledOperation>(&step.payload)) {
            return operationNodes(*operation);
        }
        if (std::holds_alternative<ScheduledEventPublish>(
                step.payload) ||
            std::holds_alternative<ScheduledEventWait>(
                step.payload)) {
            return eventNodes(step);
        }
        if (const auto* boundary =
                std::get_if<ScheduledBoundaryMaterialization>(
                    &step.payload)) {
            return boundaryNodes(*boundary);
        }
        return {};
    }

    /*
     * Remember the first and last concrete command emitted for each step.
     * Intra-step chains keep their own edges; scheduled successors depend on
     * the last command so a multi-command event remains indivisible.
     */
    void emitSteps() {
        for (ScheduleStepId stepId : context_.queue.steps) {
            const ScheduledStep& step =
                context_.scheduled.steps().at(stepId);
            std::vector<Rp1Command> nodes = nodesFor(step);
            if (nodes.empty()) continue;
            EmittedStep emitted{
                rp1CommandId(nodes.front()),
                rp1CommandId(nodes.back())};
            emitted_[stepId] = emitted;
            entries_[stepId] = emitted.first;
            emittedIds_[stepId].reserve(nodes.size());
            for (Rp1Command& node : nodes) {
                emittedIds_[stepId].push_back(
                    rp1CommandId(node));
                program_->commands.push_back(std::move(node));
            }
        }
    }

    std::vector<std::string> concreteDependencies(
        ScheduleStepId step, std::set<ScheduleStepId>& seen) const {
        if (!seen.insert(step).second) return {};
        auto emitted = emitted_.find(step);
        if (emitted != emitted_.end()) {
            return {emitted->second.second};
        }
        std::vector<std::string> result;
        for (ScheduleStepId dependency :
             context_.scheduled.steps().at(step).dependencies) {
            std::vector<std::string> nested =
                concreteDependencies(dependency, seen);
            result.insert(
                result.end(), nested.begin(), nested.end());
        }
        return result;
    }

    static void appendDependencies(
        Rp1Command& node, std::vector<std::string> dependencies) {
        std::visit(
            [&](auto& concrete) {
                concrete.dependsOn.insert(
                    concrete.dependsOn.end(),
                    dependencies.begin(), dependencies.end());
                std::sort(
                    concrete.dependsOn.begin(),
                    concrete.dependsOn.end());
                concrete.dependsOn.erase(
                    std::unique(
                        concrete.dependsOn.begin(),
                        concrete.dependsOn.end()),
                    concrete.dependsOn.end());
            },
            node);
    }

    /*
     * Dependencies may cross scheduled steps that emitted no RP1 command.
     * Walk through those steps to the nearest concrete predecessors, then
     * deduplicate IDs before attaching them to each emitted entry command.
     */
    void wireDependencies() {
        for (const auto& [stepId, entry] : entries_) {
            auto node = std::find_if(
                program_->commands.begin(), program_->commands.end(),
                [&](const Rp1Command& candidate) {
                    return rp1CommandId(candidate) == entry;
                });
            if (node == program_->commands.end()) continue;
            std::vector<std::string> dependencies;
            for (ScheduleStepId dependency :
                 context_.scheduled.steps().at(stepId).dependencies) {
                std::set<ScheduleStepId> seen;
                std::vector<std::string> concrete =
                    concreteDependencies(dependency, seen);
                dependencies.insert(
                    dependencies.end(),
                    concrete.begin(), concrete.end());
            }
            appendDependencies(*node, std::move(dependencies));
        }
    }

    const BackendLoweringContext& context_;
    DirectAuthoredIndex authored_;
    std::map<CopyTargetKey, GraphBuffer> copyTargets_;
    std::shared_ptr<Rp1QueueProgram> program_;
    std::map<ScheduleStepId, EmittedStep> emitted_;
    std::map<ScheduleStepId, std::string> entries_;
    std::map<ScheduleStepId, std::vector<std::string>> emittedIds_;
};

}  // namespace

Rp1Program Rp1Lowering::lower(
    const BackendLoweringContext& context) {
    return Rp1Program(
        context.queue.id, context.queue.device,
        context.queue.steps,
        Rp1QueueLowerer(context).lower());
}

// =========================================================================
// FpgaDevicePlan
// =========================================================================

/*
 * An executable owns the typed program, its immutable packet skeleton, and all
 * launch-time fixups.  Image construction allocates barriers and signal slots;
 * launch resolves mutable scalars, addresses, PDIs, and lifetime pins.
 */
class FpgaDevicePlan : public IBackendExecutable {
   public:
    /*
     * Scheduled-queue construction defers child attachment and image building
     * until finalize().  Resource bindings already belong to the compiler's
     * execution plan, so this executable must neither allocate nor free them.
     */
    FpgaDevicePlan(
        std::shared_ptr<FpgaDevice> device,
        const BackendLoweringContext& context)
        : device_(std::move(device)),
          submitter_(device_ ? device_->submitter_ : nullptr),
          runtimeState_(context.runtimeState),
          scalarValues_(
              runtimeState_ ? runtimeState_->scalarValues()
                            : nullptr),
          sentinelSlot_(
              device_ ? device_->sentinelSlot_
                      : kDefaultSentinelSlot),
          sentinelValue_(
              device_ ? device_->sentinelValue_
                      : kDefaultSentinelValue),
          timeout_(
              device_ ? device_->waitTimeout_
                      : kDefaultFpgaWaitTimeout),
          resourcesLeased_(true),
          queue_(context.queue.id) {
        if (!device_ || !submitter_ || !runtimeState_) {
            throw std::invalid_argument(
                "FpgaDevicePlan: device, submitter, and runtime state "
                "must not be null");
        }
        Rp1Program program = Rp1Lowering::lower(context);
        directProgram_ = std::move(program.program_);
        DirectAuthoredIndex authored;
        indexDirectAuthored(
            context.scheduled.routed().placed().resolved()
                .authored().root(),
            authored);
        for (const auto& [id, operation] : authored) {
            authoredControlIds_[id] =
                authoredSourceId(*operation);
        }
        for (const auto& [logical, scalar] :
             context.resources.scalars()) {
            (void)logical;
            if (scalar.owner == context.queue.device) {
                boundScalarSlots_[scalar.key] = scalar.physical;
            }
        }
    }

    /*
     * Direct Rp1QueueProgram construction is self-contained: discover
     * pre-launch waits and build the final mainline or control image now.
     * The optional execution lease serializes this legacy entry path.
     */
    FpgaDevicePlan(std::shared_ptr<FpgaDevice>                     device,
                   const Rp1QueueProgram&                                   dg,
                   std::shared_ptr<std::map<std::string, std::uint64_t>> scalarValues,
                   std::uint32_t                                   sentinelSlot,
                   std::uint32_t                                   sentinelValue,
                   std::chrono::milliseconds                       timeout,
                   std::unique_ptr<IDeviceExecutionLease>          executionLease)
        : device_(std::move(device)),
          executionLease_(std::move(executionLease)),
          submitter_(device_ ? device_->submitter_ : nullptr),
          runtimeState_(
              std::make_shared<BackendRuntimeState>(
                  std::move(scalarValues))),
          scalarValues_(runtimeState_->scalarValues()),
          sentinelSlot_(sentinelSlot),
          sentinelValue_(sentinelValue),
          timeout_(timeout),
          boundScalarSlots_(dg.scalarResources),
          resourcesLeased_(dg.resourcesLeased) {
        if (!device_ || !submitter_) {
            throw std::invalid_argument(
                "FpgaDevicePlan: device and submitter must not be null");
        }
        for (const Rp1Command& node : dg.commands) {
            if (const auto* wait = std::get_if<Rp1WaitCommand>(&node);
                wait && wait->preLaunch) {
                preLaunchWaitSlots_.push_back(wait->slot);
            }
        }
        image_ = programHasControl(dg) ? buildControlImage(dg) : buildMainlineImage(dg);
    }

    static bool programHasControl(const Rp1QueueProgram& dg) {
        for (const Rp1Command& node : dg.commands) {
            if (std::holds_alternative<Rp1LoopCommand>(node) ||
                std::holds_alternative<Rp1ConditionalCommand>(node)) {
                return true;
            }
        }
        return false;
    }

    ~FpgaDevicePlan() override {
        /*
         * Destruction joins the worker before releasing locally owned slots.
         * A poisoned device keeps every slot and DMA object pinned because RP1
         * may still be observing them after an indeterminate timeout.
         */
        try { wait(); } catch (...) { /* swallow */ }
        if (device_ && !device_->executionPoisoned()) {
            std::lock_guard<std::mutex> lock(device_->scalarMutex_);
            for (std::uint32_t slot : ownedSignalSlots_) {
                device_->scalarSlotAlloc_.release(slot);
            }
        }
    }

    QueueId queue() const override { return queue_; }
    DeviceId device() const override {
        return DeviceId(device_ ? device_->id() : "");
    }

    /*
     * Attach queue-local child programs by authored control ID and role.
     * Children stay typed until finalize() so all devices can be connected
     * before a loop or conditional is flattened into one autonomous image.
     */
    void connectControlChildren(
        ControlExecutableHandle control,
        ControlChildRole role,
        std::vector<QueueExecutableHandle> children) override {
        if (!directProgram_) return;
        Rp1ChildProgram child;
        child.parentCommandId = authoredControlId(control.control);
        child.role = rp1ChildRole(role);
        for (QueueExecutableHandle handle : children) {
            auto* executable =
                dynamic_cast<FpgaDevicePlan*>(handle.executable);
            if (!executable || !executable->directProgram_) {
                throw std::logic_error(
                    "Rp1Lowering: autonomous FPGA control child is "
                    "not an FPGA queue executable");
            }
            child.programs.push_back(executable->directProgram_);
        }
        if (!child.programs.empty()) {
            directProgram_->children.push_back(std::move(child));
        }
    }

    void finalize() override { ensureDirectImage(); }

    static const char* opcodeName(std::uint16_t op) {
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
    }

    static void dumpImage(const fpga::Rp1GraphImage& image) {
        std::cerr << "[rp1-dump] " << image.nodes.size() << " nodes\n";
        for (std::size_t i = 0; i < image.nodes.size(); ++i) {
            const rp1_node_t& n = image.nodes[i];
            std::cerr << "[rp1-dump] #" << i << " " << opcodeName(n.opcode)
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

    static void dumpResolvedKernelArgs(const fpga::Rp1GraphImage& image) {
        for (std::size_t nodeIndex = 0; nodeIndex < image.nodes.size(); ++nodeIndex) {
            const rp1_node_t& node = image.nodes[nodeIndex];
            if (node.opcode != RP1_OP_KERNEL_DISPATCH) continue;
            const auto& dispatch = node.payload.kernel_dispatch;
            std::size_t cursor =
                dispatch.arg_buffer_offset / sizeof(std::uint32_t);
            std::cerr << "[rp1-args] node=" << nodeIndex
                      << " kernel=0x" << std::hex
                      << dispatch.kernel_base_addr << std::dec
                      << " count=" << dispatch.arg_count;
            for (std::uint16_t i = 0; i < dispatch.arg_count; ++i) {
                if (cursor + 1 >= image.arg_buf.size()) break;
                std::cerr << " [0x" << std::hex << image.arg_buf[cursor]
                          << "=0x" << image.arg_buf[cursor + 1] << std::dec << ']';
                cursor += 2;
            }
            std::cerr << std::endl;
        }
    }

    static const char* traceEventName(std::uint16_t event) {
        switch (event) {
            case RP1_TRACE_GRAPH_START:    return "GRAPH_START";
            case RP1_TRACE_NODE_ACTIVATE:  return "NODE_ACTIVATE";
            case RP1_TRACE_KERNEL_LAUNCH:  return "KERNEL_LAUNCH";
            case RP1_TRACE_KERNEL_DONE:    return "KERNEL_DONE";
            case RP1_TRACE_KERNEL_TIMEOUT: return "KERNEL_TIMEOUT";
            case RP1_TRACE_LOOP_ITER:      return "LOOP_ITER";
            case RP1_TRACE_COND_EVAL:      return "COND_EVAL";
            case RP1_TRACE_WAIT_PARK:      return "WAIT_PARK";
            case RP1_TRACE_WAIT_WAKE:      return "WAIT_WAKE";
            case RP1_TRACE_PDI_LOAD:       return "PDI_LOAD";
            case RP1_TRACE_IMAGE_MISMATCH: return "IMAGE_MISMATCH";
            case RP1_TRACE_GRAPH_DONE:     return "GRAPH_DONE";
        }
        return "UNKNOWN";
    }

    static void dumpTrace(const fpga::Rp1TraceCapture& trace) {
        std::lock_guard<std::mutex> lock(rp1DiagnosticMutex());
        std::cerr << "[rp1-trace] written=" << trace.written
                  << " entries=" << trace.entries.size()
                  << (trace.overflow ? " overflow" : "") << "\n";
        for (std::size_t i = 0; i < trace.entries.size(); ++i) {
            const rp1_trace_entry_t& e = trace.entries[i];
            std::cerr << "  trace[" << i << "]"
                      << " t=" << e.timestamp
                      << " event=" << traceEventName(e.event)
                      << "(" << e.event << ")"
                      << " node=" << e.node_index
                      << " aux0=0x" << std::hex << e.aux0
                      << " aux1=0x" << e.aux1 << std::dec << "\n";
        }
    }

    static void dumpCq(const fpga::Rp1GraphImage& image,
                       const std::vector<rp1_cq_entry_t>& completions) {
        std::lock_guard<std::mutex> lock(rp1DiagnosticMutex());
        auto statusName = [](std::uint32_t status) -> const char* {
            switch (status) {
                case RP1_CQ_OK:      return "OK";
                case RP1_CQ_ERROR:   return "ERROR";
                case RP1_CQ_TIMEOUT: return "TIMEOUT";
                default:             return "UNKNOWN";
            }
        };

        std::cerr << "[rp1-cq] entries=" << completions.size() << "\n";
        for (std::size_t i = 0; i < completions.size(); ++i) {
            const rp1_cq_entry_t& completion = completions[i];
            const char* opcode =
                completion.node_index < image.nodes.size()
                    ? opcodeName(image.nodes[completion.node_index].opcode)
                    : "OUT_OF_RANGE";
            std::cerr << "  cq[" << i << "]"
                      << " node=" << completion.node_index
                      << " opcode=" << opcode
                      << " status=" << statusName(completion.status)
                      << "(" << completion.status << ")"
                      << " detail=0x" << std::hex << completion.error_detail
                      << std::dec
                      << " timestamp=" << completion.timestamp << "\n";
        }
    }

    static void clearHandshakeSlots(fpga::Rp1GraphImage& image) {
        /*
         * Clear externally visible handshake and predicate slots before reuse.
         * SCALAR_COPY sources are carried data, not handshakes, and must retain
         * their producer value across launches and loop iterations.
         */
        std::set<std::uint32_t> carried;
        for (const rp1_node_t& n : image.nodes) {
            if (n.opcode == RP1_OP_SCALAR_COPY) {
                carried.insert(n.payload.scalar_copy.source_slot);
            }
        }

        std::set<std::uint32_t> toClear;
        for (const rp1_node_t& n : image.nodes) {
            if (n.opcode == RP1_OP_LOOP) {
                toClear.insert(n.payload.loop.condition_signal);
            } else if (n.opcode == RP1_OP_SIGNAL) {
                toClear.insert(n.payload.signal.target_slot);
            } else if (n.opcode == RP1_OP_WAIT) {
                toClear.insert(n.payload.wait.condition_signal);
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

    /*
     * prepareLaunch() is an optional scheduler phase: join the prior launch,
     * verify the device, then clear handshakes before peers begin publishing.
     * launch() detects this and suppresses the submitter's duplicate clear.
     */
    void prepareLaunch() override {
        ensureDirectImage();
        wait();
        device_->requireExecutionUsable(
            "FpgaDevicePlan::prepareLaunch");
        if (std::getenv("VRT_FPGA_BUFFER_TRACE")) {
            std::cerr << "[fpga-buffer] clear signal slots:";
            for (std::uint32_t slot : image_.clear_signal_slots) {
                std::cerr << ' ' << slot;
            }
            std::cerr << std::endl;
        }
        submitter_->clearSignalSlots(image_.clear_signal_slots);
        if (std::getenv("VRT_FPGA_BUFFER_TRACE") && device_ && device_->window_) {
            for (std::uint32_t slot : image_.clear_signal_slots) {
                rp1_signal_slot_t value{};
                device_->window_->readSignal(slot, value);
                std::cerr << "[fpga-buffer] cleared slot " << slot
                          << " value=" << value.value << std::endl;
            }
        }
        signalsPrepared_ = true;
    }

    /*
     * Launch resolves every host-dependent field before submission, then drains
     * completion evidence even when submission reports an error.  Image state
     * is reconciled before errors are rethrown; sentinel success is checked last.
     */
    void launch() override {
        ensureDirectImage();
        wait();
        device_->requireExecutionUsable(
            "FpgaDevicePlan::launch");
        if (std::getenv("VRT_RP1_DUMP")) dumpImage(image_);
        workerEx_ = nullptr;
        worker_ = std::thread([this] {
            detail::BackendWorkerScope workerScope;
            try {
                /*
                 * Resolve values in dependency order.  Pre-launch bridge waits
                 * must precede aliases; aliases must precede address allocation
                 * so every deferred pointer resolves to its canonical backing.
                 */
                lastCq_.clear();
                resolveDeferredScalars();
                resolveDeferredLoopTripCounts();
                stageDeferredPdis();
                waitForPreLaunchSignals();
                resolveDeferredBufferAliases();
                resolveDeferredBufferAddresses();
                if (std::getenv("VRT_FPGA_BUFFER_TRACE")) {
                    dumpResolvedKernelArgs(image_);
                }

                /*
                 * Submit a copy so per-launch trace and clear policy do not
                 * mutate the reusable image skeleton.  The serial distinguishes
                 * a failed pre-submit validation from an in-flight failure.
                 */
                const bool signalsPrepared = signalsPrepared_;
                signalsPrepared_ = false;
                fpga::Rp1GraphImage submitImage = image_;
                if (signalsPrepared) submitImage.clear_signal_slots.clear();
                if (std::getenv("VRT_RP1_TRACE")) submitImage.trace_enable = true;
                const std::uint64_t submissionBefore =
                    submitter_->submissionSerial();
                std::exception_ptr submitError;
                try {
                    submitter_->submitAndWait(
                        submitImage, timeout_);
                } catch (...) {
                    submitError = std::current_exception();
                }
                const bool submitted =
                    submitter_->submissionSerial() !=
                    submissionBefore;

                /*
                 * Once RP1 accepted the image, CQ evidence is needed even when
                 * submitAndWait failed.  PDI side effects are reconciled from
                 * that evidence before transport or firmware errors escape.
                 */
                std::exception_ptr drainError;
                if (submitted) {
                    try {
                        lastCq_ = submitter_->drainCqRaw();
                    } catch (...) {
                        drainError = std::current_exception();
                    }
                }
                reconcileImageSideEffects(
                    lastCq_, submitted, submitError,
                    !drainError);
                if (std::getenv("VRT_RP1_CQ")) {
                    dumpCq(image_, lastCq_);
                }

                /*
                 * Validate indices before status so diagnostics never index
                 * outside the submitted image.  Preserve error precedence:
                 * submission, CQ drain, then firmware completion status.
                 */
                std::exception_ptr cqError;
                if (!drainError) {
                    try {
                        for (const rp1_cq_entry_t& completion :
                             lastCq_) {
                            if (completion.node_index >=
                                image_.nodes.size()) {
                                throw std::runtime_error(
                                    "FpgaDevicePlan: completion "
                                    "references out-of-range node " +
                                    std::to_string(
                                        completion.node_index));
                            }
                        }
                        fpga::Rp1Submitter::validateCq(lastCq_);
                    } catch (...) {
                        cqError = std::current_exception();
                    }
                }
                if (submitError) {
                    std::rethrow_exception(submitError);
                }
                if (drainError) {
                    std::rethrow_exception(drainError);
                }
                if (cqError) {
                    std::rethrow_exception(cqError);
                }

                /*
                 * A clean CQ is not sufficient: the trailing sentinel proves
                 * every graph leaf joined and the final SIGNAL actually ran.
                 */
                const std::uint32_t sentinel =
                    submitter_->readSignalValue(sentinelSlot_);
                if (sentinel != sentinelValue_) {
                    throw std::runtime_error(
                        "FpgaDevicePlan: RP1 reported graph completion "
                        "without the lifecycle sentinel (slot=" +
                        std::to_string(sentinelSlot_) + ", expected=" +
                        std::to_string(sentinelValue_) + ", actual=" +
                        std::to_string(sentinel) + ")");
                }
                if (submitImage.trace_enable) dumpTrace(submitter_->drainTrace());
            } catch (...) {
                /*
                 * A submitter timeout with unknown hardware state poisons the
                 * device.  Keep the original worker exception for wait(), while
                 * poisonExecution() arranges process-lifetime quarantine.
                 */
                workerEx_ = std::current_exception();
                if (submitter_->poisoned()) {
                    device_->poisonExecution();
                }
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

    /*
     * wait() is the ownership handoff point for launch pins and worker errors.
     * Known-idle launches release pins; poisoned launches transfer them to the
     * device quarantine before rethrowing the original asynchronous failure.
     */
    void wait() override {
        if (worker_.joinable()) worker_.join();
        if (device_ && device_->executionPoisoned()) {
            device_->quarantineLaunchPins(
                std::move(launchBufferPins_),
                std::move(launchPdiPins_));
        } else {
            launchBufferPins_.clear();
            launchPdiPins_.clear();
        }
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
    std::unique_lock<std::mutex> lockScalarValues() const {
        return runtimeState_
                   ? std::unique_lock<std::mutex>(
                         runtimeState_->scalarMutex())
                   : std::unique_lock<std::mutex>();
    }

    std::string authoredControlId(NodeId control) const {
        auto operation = authoredControlIds_.find(control);
        return operation == authoredControlIds_.end()
                   ? "node_" + std::to_string(control.value())
                   : operation->second;
    }

    static Rp1ChildRole rp1ChildRole(
        ControlChildRole role) {
        switch (role) {
            case ControlChildRole::LoopBody:
                return Rp1ChildRole::LoopBody;
            case ControlChildRole::ConditionalThen:
                return Rp1ChildRole::ConditionalThen;
            case ControlChildRole::ConditionalElse:
                return Rp1ChildRole::ConditionalElse;
        }
        return Rp1ChildRole::LoopBody;
    }

    void ensureDirectImage() {
        /*
         * Child programs and region metadata must be complete before packets
         * are built.  Build exactly once because barrier and slot allocation
         * also records deferred fixups consumed by every subsequent launch.
         */
        if (!directProgram_ || directImageBuilt_) return;
        device_->populateBufferRegions(*directProgram_);
        for (const Rp1Command& node : directProgram_->commands) {
            if (const auto* wait =
                    std::get_if<Rp1WaitCommand>(&node);
                wait && wait->preLaunch) {
                preLaunchWaitSlots_.push_back(wait->slot);
            }
        }
        image_ = programHasControl(*directProgram_)
                     ? buildControlImage(*directProgram_)
                     : buildMainlineImage(*directProgram_);
        directImageBuilt_ = true;
    }

    /*
     * Direct programs reserve slots on the device allocator themselves.
     * Compiler-leased programs already own their namespace and only mirror
     * reservations in the image-local allocator.
     */
    void reserveDeviceSignalSlot(std::uint32_t slot) {
        if (!device_ || resourcesLeased_) return;
        std::lock_guard<std::mutex> lk(device_->scalarMutex_);
        if (ownedSignalSlots_.insert(slot).second) {
            device_->scalarSlotAlloc_.reserve(slot);
        }
    }

    std::uint32_t acquireScalarSlot(
        const std::string& key,
        fpga::SignalSlotAllocator& slotAlloc) {
        /*
         * Prefer the execution plan's stable physical binding.  Legacy direct
         * programs may allocate a device slot, but must remember ownership so
         * destruction can release it only after hardware is known idle.
         */
        auto bound = boundScalarSlots_.find(key);
        if (bound != boundScalarSlots_.end()) {
            const std::uint32_t slot =
                static_cast<std::uint32_t>(bound->second.value());
            slotAlloc.reserve(slot);
            return slot;
        }
        if (resourcesLeased_) {
            throw std::logic_error(
                "FpgaDevice: plan has no leased scalar slot for '" +
                key + "'");
        }
        std::lock_guard<std::mutex> lock(device_->scalarMutex_);
        const std::uint32_t slot =
            device_->scalarSlotAlloc_.alloc();
        ownedSignalSlots_.insert(slot);
        slotAlloc.reserve(slot);
        return slot;
    }

    void reserveSignalSlot(std::uint32_t slot, fpga::SignalSlotAllocator& slotAlloc) {
        if (slot == sentinelSlot_) {
            throw std::logic_error(
                "FpgaDevice: signal slot " + std::to_string(slot) +
                " collides with the lifecycle sentinel reservation");
        }
        slotAlloc.reserve(slot);
        reserveDeviceSignalSlot(slot);
    }

    /*
     * Reserve every externally assigned slot before allocating output scalars.
     * Recursing through control children prevents autonomous body slots from
     * colliding with the sentinel or a top-level rendezvous.
     */
    void reserveReferencedSignalSlots(const Rp1QueueProgram& dg,
                                      fpga::SignalSlotAllocator& slotAlloc) {
        for (const Rp1Command& node : dg.commands) {
            if (const auto* sg = std::get_if<Rp1SignalCommand>(&node)) {
                reserveSignalSlot(sg->slot, slotAlloc);
            } else if (const auto* wt = std::get_if<Rp1WaitCommand>(&node)) {
                reserveSignalSlot(wt->slot, slotAlloc);
            } else if (const auto* loop = std::get_if<Rp1LoopCommand>(&node)) {
                if (loop->broadcastRole != Rp1SplitRole::None) {
                    reserveSignalSlot(loop->conditionBroadcastSlot, slotAlloc);
                    reserveSignalSlot(loop->broadcastReadySlot, slotAlloc);
                    reserveSignalSlot(loop->broadcastAckSlot, slotAlloc);
                }
            }
        }
        for (const Rp1ChildProgram& child : dg.children) {
            for (const auto& childDg : child.programs) {
                if (childDg) reserveReferencedSignalSlots(*childDg, slotAlloc);
            }
        }
    }

    static bool isTimeoutError(
        const std::exception_ptr& error) {
        if (!error) return false;
        try {
            std::rethrow_exception(error);
        } catch (const fpga::Rp1TimeoutError&) {
            return true;
        } catch (...) {
            return false;
        }
    }

    /*
     * PDI_LOAD changes host image state only when its CQ entry proves success.
     * A failed PDI makes the image unknown; incomplete CQ evidence or a timeout
     * does likewise because later dispatch guards must not trust stale state.
     */
    void reconcileImageSideEffects(
        const std::vector<rp1_cq_entry_t>& completions,
        bool submitted, const std::exception_ptr& submitError,
        bool evidenceComplete) {
        if (!device_ || !submitted || pdiImagesByNode_.empty()) {
            return;
        }
        bool terminalEvidence = false;
        bool transportIndeterminate = !evidenceComplete;
        for (const rp1_cq_entry_t& completion : completions) {
            if (completion.node_index >= image_.nodes.size()) {
                transportIndeterminate = true;
                continue;
            }
            if (completion.status != RP1_CQ_OK) {
                terminalEvidence = true;
            }
            auto pdi = pdiImagesByNode_.find(
                completion.node_index);
            if (pdi == pdiImagesByNode_.end()) continue;
            if (completion.status == RP1_CQ_OK) {
                device_->setActiveImage(pdi->second);
            } else {
                device_->markActiveImageUnknown();
            }
        }
        if (submitError &&
            (isTimeoutError(submitError) ||
             !terminalEvidence)) {
            transportIndeterminate = true;
        }
        if (transportIndeterminate) {
            device_->markActiveImageUnknown();
        }
    }

    /*
     * Scalar arguments are snapshots taken at launch, not compile time.
     * Scoped keys win; the bare-name fallback exists only for root scope.
     * Patch value words while preserving the interleaved register offsets.
     */
    void resolveDeferredScalars() {
        if (deferred_.empty()) return;
        if (!scalarValues_) {
            throw std::runtime_error(
                "FpgaDevicePlan: deferred scalar resolution requires a scalar map "
                "(Rp1QueueProgram::scalarValues was null at compile time)");
        }
        auto scalarLock = lockScalarValues();
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

    /*
     * Fixed-count loop values are also launch-time scalars.  Zero is encoded
     * as one firmware iteration with an inverted body gate, because RP1 uses
     * max_iterations == 0 for predicate-governed, not zero-trip, loops.
     */
    void resolveDeferredLoopTripCounts() {
        if (deferredTripCounts_.empty()) return;
        if (!scalarValues_) {
            throw std::runtime_error(
                "FpgaDevicePlan: deferred loop trip-count resolution requires a scalar map");
        }
        auto scalarLock = lockScalarValues();
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
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error(
                    "FpgaDevicePlan: loop trip count '" + d.diagnostic +
                    "' must be in the range 0..UINT32_MAX");
            }
            if (d.nodeIndex >= image_.nodes.size() ||
                image_.nodes[d.nodeIndex].opcode != RP1_OP_LOOP) {
                throw std::logic_error(
                    "FpgaDevicePlan: deferred trip count points at a non-LOOP node");
            }
            if (d.gateNodeIndex >= image_.nodes.size() ||
                image_.nodes[d.gateNodeIndex].opcode != RP1_OP_COND) {
                throw std::logic_error(
                    "FpgaDevicePlan: deferred trip count points at a non-COND body gate");
            }
            auto& loop = image_.nodes[d.nodeIndex].payload.loop;
            loop.max_iterations =
                value == 0 ? 1u : static_cast<std::uint32_t>(value);
            loop.condition_value = fpga::kNeverValue;
            loop.condition_op =
                value == 0
                    ? fpga::invertRp1Op(fpga::kNeverOp)
                    : fpga::kNeverOp;
            auto& gate =
                image_.nodes[d.gateNodeIndex].payload.cond;
            gate.condition_op =
                fpga::invertRp1Op(
                    static_cast<rp1_condop_t>(
                        loop.condition_op));
        }
    }

    /*
     * Resolve boundary and RW aliases to one canonical allocation.  Existing
     * storage wins in either direction; unresolved root inputs wait for bridge
     * staging, while wholly internal chains receive one zero-filled seed.
     */
    void resolveDeferredBufferAliases() {
        if (deferredBufferAliases_.empty()) return;
        if (!device_) {
            throw std::runtime_error(
                "FpgaDevicePlan: deferred buffer aliases require an FpgaDevice");
        }

        std::vector<const DeferredBufferAlias*> pending;
        pending.reserve(deferredBufferAliases_.size());
        for (const DeferredBufferAlias& alias : deferredBufferAliases_) {
            pending.push_back(&alias);
        }

        const auto stagingDeadline =
            std::chrono::steady_clock::now() + kBridgeWaitTimeout;
        while (!pending.empty()) {
            /*
             * Alias chains may arrive out of order, so repeat passes while any
             * edge resolves.  This is a fixed-point walk over a small launch-
             * local set, not a data copy.
             */
            std::vector<const DeferredBufferAlias*> unresolved;
            bool madeProgress = false;
            for (const DeferredBufferAlias* alias : pending) {
                if (std::getenv("VRT_FPGA_BUFFER_TRACE")) {
                    std::cerr << "[fpga-buffer] resolve alias "
                              << alias->targetKey << " <- " << alias->sourceKey
                              << " source=" << device_->hasBuffer(alias->sourceKey)
                              << " target=" << device_->hasBuffer(alias->targetKey)
                              << std::endl;
                }
                if (device_->hasBuffer(alias->sourceKey)) {
                    device_->aliasBufferKey(alias->targetKey, alias->sourceKey);
                    madeProgress = true;
                } else if (device_->hasBuffer(alias->targetKey)) {
                    // Loop-carried outputs are unallocated before the first
                    // iteration, while their initial parent input is already
                    // staged. Preserve that seed by making the output alias it.
                    device_->aliasBufferKey(alias->sourceKey, alias->targetKey);
                    madeProgress = true;
                } else {
                    unresolved.push_back(alias);
                }
            }
            if (unresolved.empty()) break;
            if (!madeProgress) {
                /*
                 * A root-scope source can still be owned by an asynchronous
                 * CPU-to-FPGA bridge.  Do not allocate a competing backing
                 * while that producer may publish the real one.
                 */
                const bool awaitingRootInput = std::any_of(
                    unresolved.begin(), unresolved.end(),
                    [&](const DeferredBufferAlias* alias) {
                        return alias->sourceKey.rfind("scope:0:", 0) == 0 &&
                               alias->targetKey.rfind("scope:0:", 0) != 0 &&
                               !device_->hasBuffer(alias->sourceKey);
                    });
                if (awaitingRootInput) {
                    if (std::chrono::steady_clock::now() > stagingDeadline) {
                        throw std::runtime_error(
                            "FpgaDevicePlan: timed out waiting for a root input "
                            "buffer to be staged before resolving loop aliases");
                    }
                    std::this_thread::yield();
                    continue;
                }

                /*
                 * No external producer can make progress now.  Allocate one
                 * sized backing per unresolved chain and alias both names to it.
                 */
                for (const DeferredBufferAlias* alias : unresolved) {
                    std::size_t bytes = 0;
                    {
                        auto scalarLock = lockScalarValues();
                        bytes = resolvedBufferSizeBytes(
                            alias->sizeToken, scalarValues_,
                            "FpgaDevicePlan");
                    }
                    device_->setInputBuffer(alias->sourceKey, nullptr, bytes);
                    device_->aliasBufferKey(alias->targetKey, alias->sourceKey);
                }
                break;
            }
            pending = std::move(unresolved);
        }
    }

    /*
     * Pre-launch waits are host-polled because submitting RP1 first could park
     * the sole device execution behind data that the scheduler has not staged.
     * One shared deadline bounds the complete bridge-input rendezvous phase.
     */
    void waitForPreLaunchSignals() {
        if (preLaunchWaitSlots_.empty()) return;
        if (!device_ || !device_->window_) {
            throw std::runtime_error(
                "FpgaDevicePlan: pre-launch input waits require an FPGA BAR window");
        }
        const auto deadline =
            std::chrono::steady_clock::now() + kBridgeWaitTimeout;
        for (std::uint32_t slot : preLaunchWaitSlots_) {
            rp1_signal_slot_t signal{};
            for (;;) {
                device_->window_->readSignal(slot, signal);
                if (signal.value != 0) break;
                if (std::chrono::steady_clock::now() > deadline) {
                    throw std::runtime_error(
                        "FpgaDevicePlan: timed out waiting for CPU-to-FPGA "
                        "input staging signal slot " + std::to_string(slot));
                }
                std::this_thread::yield();
            }
        }
    }

    /*
     * Buffer addresses are late because sizes and aliases can change at launch.
     * Allocation chooses HBM/DDR when region metadata is known, otherwise the
     * BAR arena; device-memory records are pinned through completion.
     */
    void resolveDeferredBufferAddresses() {
        if (deferredBufferAddresses_.empty()) return;
        if (!device_) {
            throw std::runtime_error(
                "FpgaDevicePlan: deferred buffer address resolution requires an FpgaDevice");
        }
        for (const DeferredBufferAddress& d : deferredBufferAddresses_) {
            std::size_t bytes = 0;
            {
                auto scalarLock = lockScalarValues();
                bytes = resolvedBufferSizeBytes(
                    d.buffer, scalarValues_, "FpgaDevicePlan");
            }
            std::shared_ptr<::vrt::Buffer<std::uint8_t>> pin;
            const std::uint64_t addr =
                device_->bufferDeviceAddress(
                    d.buffer, bytes, pin);
            if (pin) {
                launchBufferPins_.push_back(std::move(pin));
            }
            if (std::getenv("VRT_FPGA_BUFFER_TRACE")) {
                std::cerr << "[fpga-buffer] arg " << d.diagnostic
                          << " key=" << scopedBufferKey(
                                 d.buffer.scopeId(), d.buffer.name())
                          << " addr=0x" << std::hex << addr << std::dec
                          << " size=" << bytes << std::endl;
            }
            std::uint32_t words[2] = {0u, 0u};
            writeU64ToArgWords(addr, words);
            image_.arg_buf[d.arg_word_offset] = words[0];
            image_.arg_buf[d.arg_word_offset + 2u] = words[1];
        }
    }

    /*
     * Stage each distinct image/path once per launch, patch every matching
     * PDI_LOAD, and retain the DMA allocation until completion.  The device-
     * level cache may reuse storage across launches, but launch pins still
     * protect against teardown during an in-flight reconfiguration.
     */
    void stageDeferredPdis() {
        if (deferredPdis_.empty()) return;
        struct StagedPdi {
            std::uint64_t address = 0;
            std::shared_ptr<::vrt::Buffer<std::uint8_t>> pin;
        };
        std::map<std::string, StagedPdi> stagedByPath;
        for (const DeferredPdi& d : deferredPdis_) {
            const std::string key = d.imageId + "\n" + d.pdiPath;
            auto it = stagedByPath.find(key);
            if (it == stagedByPath.end()) {
                Rp1ReprogramCommand node;
                node.imageId = d.imageId;
                node.pdiPath = d.pdiPath;
                StagedPdi staged;
                staged.address =
                    stagePdi(node, staged.pin);
                if (staged.pin) {
                    launchPdiPins_.push_back(staged.pin);
                }
                it = stagedByPath.emplace(
                    key, std::move(staged)).first;
            }
            if (d.nodeIndex >= image_.nodes.size() ||
                image_.nodes[d.nodeIndex].opcode != RP1_OP_PDI_LOAD) {
                throw std::logic_error(
                    "FpgaDevicePlan: deferred PDI fixup points at a non-PDI_LOAD node");
            }
            auto& payload = image_.nodes[d.nodeIndex].payload.pdi_load;
            payload.pdi_addr_lo = static_cast<std::uint32_t>(
                it->second.address & 0xFFFFFFFFull);
            payload.pdi_addr_hi = static_cast<std::uint32_t>(
                (it->second.address >> 32) & 0xFFFFFFFFull);
        }
    }

    std::uint64_t scalarBits(const GraphScalar& gs, const std::string& diagnostic) const {
        if (!scalarValues_) {
            throw std::runtime_error(
                "FpgaDevicePlan: scalar bound to port '" + diagnostic +
                "' requires a scalar map");
        }
        auto scalarLock = lockScalarValues();
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
        auto scalarLock = lockScalarValues();
        return resolvedBufferSizeBytes(buffer, scalarValues_, "FpgaDevicePlan");
    }

    std::map<std::string, std::uint32_t> inputScalarRegOffsets(
        const KernelDescriptor& kernel) const {
        ArgLayout layout(device_->kernelArgOffsets(kernel), kernel.name);
        std::map<std::string, std::uint32_t> out;
        for (const ScalarPort& port : kernel.ioType.inputScalars) {
            out[port.name] = layout.take(port.name, scalarWidthInWords(port.type));
        }
        return out;
    }

    void appendBufferAddress(fpga::Rp1GraphImage& image,
                             const Rp1KernelCommand& node,
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

    /*
     * Pack RP1's (register offset, value) ABI in descriptor order: input
     * scalars, input buffers, output buffers, then one pointer per RW pair.
     * Scalars and addresses remain zero placeholders until launch-time fixups.
     */
    std::uint32_t packKernelArgs(fpga::Rp1GraphImage& image,
                                 const Rp1KernelCommand& node,
                                 const std::set<std::string>& skipInputScalars = {}) {
        std::uint32_t cursor_words = static_cast<std::uint32_t>(image.arg_buf.size());
        std::uint32_t arg_count = 0;

        ArgLayout layout(device_->kernelArgOffsets(node.kernel), node.kernel.name);

        /*
         * Static scalar inputs reserve their exact AXI-Lite words now.  A
         * loop-carried input is omitted because SCALAR_COPY writes its register
         * on every iteration after the previous value becomes ready.
         */
        for (const ScalarPort& port : node.kernel.ioType.inputScalars) {
            const std::uint32_t width = scalarWidthInWords(port.type);
            const std::uint32_t base = layout.take(port.name, width);
            // A loop-carried scalar input is fed each iteration by a SCALAR_COPY
            // from its signal slot into this register, not by a static arg.
            if (skipInputScalars.count(port.name)) continue;
            auto it = node.ioMap.inputScalars().find(port.name);
            if (it == node.ioMap.inputScalars().end()) {
                throw std::runtime_error(
                    "FpgaDevice: kernel '" + node.kernel.name +
                    "' input scalar port '" + port.name + "' has no detail::PortBindings binding");
            }
            std::uint32_t words[2] = {0u, 0u};
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

        /*
         * Output scalars are not dispatch arguments.  The kernel writes its
         * AXI-Lite output registers, and a post-dispatch SCALAR_READ publishes
         * each result to the signal namespace.
         */

        /*
         * Read-only and write-only buffers each carry a 64-bit device address.
         * Keep both categories distinct because their graph tokens can have
         * different scope, size, and region metadata.
         */
        for (const BufferPort& port : node.kernel.ioType.inputs) {
            auto it = node.ioMap.inputs().find(port.name);
            if (it == node.ioMap.inputs().end()) {
                throw std::runtime_error(
                    "FpgaDevice: kernel '" + node.kernel.name +
                    "' input buffer port '" + port.name + "' has no detail::PortBindings binding");
            }
            appendBufferAddress(image, node, layout, port.name, it->second,
                                0, cursor_words, arg_count);
        }

        for (const BufferPort& port : node.kernel.ioType.outputs) {
            auto it = node.ioMap.outputs().find(port.name);
            if (it == node.ioMap.outputs().end()) {
                throw std::runtime_error(
                    "FpgaDevice: kernel '" + node.kernel.name +
                    "' output buffer port '" + port.name + "' has no detail::PortBindings binding");
            }
            appendBufferAddress(image, node, layout, port.name, it->second,
                                0, cursor_words, arg_count);
        }

        /*
         * RP1 currently exposes one pointer register for an RW pair.  Pack the
         * input address and defer a zero-copy output alias so later consumers
         * observe the same allocation rather than a second pointer argument.
         */
        for (const RWBufferPort& port : node.kernel.ioType.inouts) {
            auto it = std::find_if(node.ioMap.inouts().begin(),
                                   node.ioMap.inouts().end(),
                                   [&](const detail::PortBindings::InoutBinding& binding) {
                                       return binding.inPort == port.in.name &&
                                              binding.outPort == port.out.name;
                                   });
            if (it == node.ioMap.inouts().end()) {
                throw std::runtime_error(
                    "FpgaDevice: kernel '" + node.kernel.name +
                    "' RW buffer ports '" + port.in.name + "'/'" +
                    port.out.name + "' have no detail::PortBindings binding");
            }
            appendBufferAddress(image, node, layout, port.in.name, it->in,
                                0, cursor_words, arg_count);
            deferredBufferAliases_.push_back(DeferredBufferAlias{
                scopedBufferKey(it->in.scopeId(), it->in.name()),
                scopedBufferKey(it->out.scopeId(), it->out.name()),
                it->in,
                node.id + "." + port.out.name});
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

    std::uint64_t stagePdi(
        const Rp1ReprogramCommand& node,
        std::shared_ptr<::vrt::Buffer<std::uint8_t>>& pin) {
        if (device_->vbinSpec_ && device_->vbinSpec_->hasImage(node.imageId)) {
            const auto& imageSpec = device_->vbinSpec_->image(node.imageId);
            return device_->stagePdiBytes(
                imageSpec.id, imageSpec.pdiBytes, pin);
        }
        return device_->stagePdiFile(node.pdiPath, pin);
    }

    /*
     * Merge a control role's per-device children into start boundaries,
     * topologically ordered work, and end boundaries.  Imports must establish
     * carried aliases before packet packing; exports publish only after all
     * producers.  A cyclic work set keeps stable source order for diagnostics.
     */
    static std::vector<const Rp1Command*> collectControlBody(
        const Rp1QueueProgram& dg, const std::string& controlId, Rp1ChildRole role) {
        std::vector<const Rp1Command*> starts, mids, ends;
        for (const Rp1ChildProgram& child : dg.children) {
            if (child.parentCommandId != controlId || child.role != role) continue;
            for (const auto& body : child.programs) {
                if (!body) continue;
                for (const Rp1Command& n : body->commands) {
                    if (const auto* b = std::get_if<Rp1BoundaryCommand>(&n)) {
                        (b->side == Rp1BoundaryCommand::Side::Start ? starts : ends).push_back(&n);
                    } else {
                        mids.push_back(&n);
                    }
                }
            }
        }
        {
            std::unordered_map<std::string, std::size_t> idxById;
            for (std::size_t i = 0; i < mids.size(); ++i) {
                idxById[rp1CommandId(*mids[i])] = i;
            }
            std::vector<int> indeg(mids.size(), 0);
            std::vector<std::vector<std::size_t>> succ(mids.size());
            for (std::size_t i = 0; i < mids.size(); ++i) {
                for (const std::string& d : rp1CommandDependsOn(*mids[i])) {
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
            std::vector<const Rp1Command*> sorted;
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
        std::vector<const Rp1Command*> out;
        out.reserve(starts.size() + mids.size() + ends.size());
        out.insert(out.end(), starts.begin(), starts.end());
        out.insert(out.end(), mids.begin(), mids.end());
        out.insert(out.end(), ends.begin(), ends.end());
        return out;
    }

    /*
     * Append arguments before the dispatch so its byte offset remains stable.
     * The expected-image guard is enabled only for an explicit kernel image;
     * deferred values do not alter packet or argument-buffer shape.
     */
    std::size_t emitKernelPacket(fpga::Rp1GraphImage& image, const Rp1KernelCommand& k,
                                 std::uint8_t awBucket, std::uint32_t awMask,
                                 std::uint8_t setBucket, std::uint32_t setMask,
                                 const std::set<std::string>& skipInputScalars = {},
                                 std::optional<FpgaKernelLocation> resolvedLocation = std::nullopt) {
        const FpgaKernelLocation loc = resolvedLocation
                                           ? *resolvedLocation
                                           : device_->resolveKernelLocation(k.kernel);
        const std::uint32_t argOffset =
            static_cast<std::uint32_t>(image.arg_buf.size()) * sizeof(std::uint32_t);
        const std::uint32_t argCount = packKernelArgs(image, k, skipInputScalars);
        rp1_node_t pkt{};
        pkt.status               = RP1_NODE_PENDING;
        pkt.opcode               = RP1_OP_KERNEL_DISPATCH;
        pkt.flags                = RP1_FLAG_HALT_ON_ERROR;
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

    /*
     * PDI bytes cannot be addressed until launch, so emit a guarded PDI_LOAD
     * skeleton and remember both its staging fixup and its image-state effect.
     */
    std::size_t emitReprogramPacket(fpga::Rp1GraphImage& image, const Rp1ReprogramCommand& r,
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
        pdiImagesByNode_[idx] = r.imageId;
        return idx;
    }

    struct MainlineScalarReady {
        std::uint32_t slot;
        std::uint8_t bucket;
        std::uint32_t mask;
    };

    /*
     * Lower acyclic mainline commands to packets with one completion bit each.
     * Produced scalars add SCALAR_READ nodes; later scalar consumers add
     * SCALAR_COPY nodes whose barriers join producer readiness and graph deps.
     * Reprogram, SIGNAL, and WAIT commands map one-to-one; controls are invalid.
     */
    template <class BucketFn, class BitFn, class ResolveFn>
    void emitMainlineCommands(
        const Rp1QueueProgram& dg, std::size_t N,
        const std::unordered_map<std::string, std::size_t>& posOf,
        fpga::SignalSlotAllocator& slotAlloc,
        fpga::Rp1GraphImage& image,
        std::unordered_map<std::string, MainlineScalarReady>& scalarReady,
        std::map<std::uint8_t, std::uint32_t>& extraLeafGroups,
        std::size_t& nextExtraPos, BucketFn nodeBucketOf,
        BitFn nodeBitOf, ResolveFn resolveAwait) {
        for (std::size_t p = 0; p < N; ++p) {
            const Rp1Command& n = dg.commands[p];
            std::map<std::uint8_t, std::uint32_t> predGroups;
            for (const std::string& depId : rp1CommandDependsOn(n)) {
                auto it = posOf.find(depId);
                if (it != posOf.end()) predGroups[nodeBucketOf(it->second)] |= nodeBitOf(it->second);
            }
            const std::uint8_t setBucket = nodeBucketOf(p);
            const std::uint32_t setMask = nodeBitOf(p);

            if (const auto* k = std::get_if<Rp1KernelCommand>(&n)) {
                /*
                 * Forward only scalars already produced in this image.
                 * Copies become additional predecessors, while graph/runtime
                 * scalars remain deferred values in the argument buffer.
                 */
                std::set<std::string> forwardedInputs;
                std::map<std::uint8_t, std::uint32_t> kernelGroups = predGroups;
                std::optional<FpgaKernelLocation> loc;
                std::optional<std::map<std::string, std::uint32_t>> inputOffsets;
                for (const ScalarPort& sp : k->kernel.ioType.inputScalars) {
                    auto bindIt = k->ioMap.inputScalars().find(sp.name);
                    if (bindIt == k->ioMap.inputScalars().end()) continue;
                    const GraphScalar& gs = bindIt->second;
                    const std::string key = scopedScalarKey(gs.scopeId(), gs.varName());
                    auto readyIt = scalarReady.find(key);
                    if (readyIt == scalarReady.end()) continue;

                    if (!loc) {
                        loc = device_->resolveKernelLocation(k->kernel);
                        inputOffsets = inputScalarRegOffsets(k->kernel);
                    }
                    forwardedInputs.insert(sp.name);
                    std::map<std::uint8_t, std::uint32_t> copyGroups = predGroups;
                    copyGroups[readyIt->second.bucket] |= readyIt->second.mask;
                    const auto [copyAwBucket, copyAwMask] = resolveAwait(copyGroups);
                    const std::size_t cpos = nextExtraPos++;
                    const std::uint8_t cbucket = nodeBucketOf(cpos);
                    const std::uint32_t cmask = nodeBitOf(cpos);

                    rp1_node_t cp{};
                    cp.opcode = RP1_OP_SCALAR_COPY;
                    cp.status = RP1_NODE_PENDING;
                    cp.barrier_await_bucket = copyAwBucket;
                    cp.barrier_await_mask = copyAwMask;
                    cp.barrier_set_bucket = cbucket;
                    cp.barrier_set_mask = cmask;
                    cp.payload.scalar_copy.source_slot = readyIt->second.slot;
                    cp.payload.scalar_copy.dest_addr =
                        loc->r5_base_addr + inputOffsets->at(sp.name);
                    image.nodes.push_back(cp);
                    kernelGroups[cbucket] |= cmask;
                }

                const auto [kernelAwBucket, kernelAwMask] = resolveAwait(kernelGroups);
                emitKernelPacket(image, *k, kernelAwBucket, kernelAwMask,
                                 setBucket, setMask, forwardedInputs, loc);

                /*
                 * Serialize output reads after dispatch and after each prior
                 * read.  The final read, not the dispatch, is this kernel's
                 * externally visible leaf when output scalars exist.
                 */
                std::uint8_t lastBucket = setBucket;
                std::uint32_t lastMask = setMask;
                for (const ScalarPort& sp : k->kernel.ioType.outputScalars) {
                    requireSignalSlotScalar(k->kernel, sp);
                    const std::size_t rpos = nextExtraPos++;
                    const std::uint8_t rbucket = nodeBucketOf(rpos);
                    const std::uint32_t rmask = nodeBitOf(rpos);
                    auto bindIt =
                        k->ioMap.outputScalars().find(sp.name);
                    const std::string key =
                        bindIt == k->ioMap.outputScalars().end()
                            ? std::string{}
                            : scopedScalarKey(
                                  bindIt->second.scopeId(),
                                  bindIt->second.varName());
                    const std::uint32_t slot =
                        acquireScalarSlot(key, slotAlloc);
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
                    if (bindIt != k->ioMap.outputScalars().end()) {
                        scalarSlots_[key] = slot;
                        scalarReady[key] = MainlineScalarReady{slot, rbucket, rmask};
                    }
                    extraLeafGroups[rbucket] |= rmask;
                    lastBucket = rbucket;
                    lastMask = rmask;
                }
                continue;
            }

            const auto [awBucket, awMask] = resolveAwait(predGroups);
            if (const auto* r = std::get_if<Rp1ReprogramCommand>(&n)) {
                emitReprogramPacket(image, *r, awBucket, awMask, setBucket, setMask);
            } else if (const auto* sg = std::get_if<Rp1SignalCommand>(&n)) {
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
            } else if (const auto* wt = std::get_if<Rp1WaitCommand>(&n)) {
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
                    "FpgaDevice: non-RP1 node '" + rp1CommandId(n) +
                    "' reached main-line FPGA image lowering");
            }
        }

    }

    /*
     * Build a non-control image from an arbitrary DAG.  Work bits occupy 31
     * bits per bucket; cross-bucket predecessor sets are folded through silent
     * NOP collectors because one RP1 node can await only one bucket.
     */
    fpga::Rp1GraphImage buildMainlineImage(const Rp1QueueProgram& dg) {
        if (dg.commands.empty()) throw std::logic_error("FpgaDevice: empty FPGA Rp1QueueProgram");

        /*
         * Reserve all named slots first, then count implicit scalar copies and
         * reads.  Barrier positions must include those nodes before any packet
         * is emitted or later positions would shift.
         */
        const std::size_t N = dg.commands.size();
        fpga::SignalSlotAllocator slotAlloc;
        slotAlloc.reserve(sentinelSlot_);
        reserveReferencedSignalSlots(dg, slotAlloc);
        std::size_t totalWork = N;
        std::vector<bool> hasOutputScalarReads(N, false);
        std::set<std::string> availableScalarKeys;
        for (std::size_t p = 0; p < N; ++p) {
            if (const auto* k = std::get_if<Rp1KernelCommand>(&dg.commands[p])) {
                for (const ScalarPort& sp : k->kernel.ioType.inputScalars) {
                    auto bindIt = k->ioMap.inputScalars().find(sp.name);
                    if (bindIt == k->ioMap.inputScalars().end()) continue;
                    const GraphScalar& gs = bindIt->second;
                    if (availableScalarKeys.count(
                            scopedScalarKey(gs.scopeId(), gs.varName())) != 0) {
                        ++totalWork;
                    }
                }
                totalWork += k->kernel.ioType.outputScalars.size();
                hasOutputScalarReads[p] = !k->kernel.ioType.outputScalars.empty();
                for (const ScalarPort& sp : k->kernel.ioType.outputScalars) {
                    auto bindIt = k->ioMap.outputScalars().find(sp.name);
                    if (bindIt == k->ioMap.outputScalars().end()) continue;
                    const GraphScalar& gs = bindIt->second;
                    availableScalarKeys.insert(
                        scopedScalarKey(gs.scopeId(), gs.varName()));
                }
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

        /*
         * Index symbolic dependencies and identify true leaves.  Extra scalar
         * reads maintain their own leaf set because they extend a kernel beyond
         * its typed command's completion bit.
         */
        std::unordered_map<std::string, std::size_t> posOf;
        for (std::size_t p = 0; p < N; ++p) posOf[rp1CommandId(dg.commands[p])] = p;
        std::size_t nextExtraPos = N;

        std::vector<bool> isLeaf(N, true);
        std::map<std::uint8_t, std::uint32_t> extraLeafGroups;
        for (std::size_t p = 0; p < N; ++p) {
            for (const std::string& depId : rp1CommandDependsOn(dg.commands[p])) {
                auto it = posOf.find(depId);
                if (it != posOf.end()) isLeaf[it->second] = false;
            }
        }

        fpga::Rp1GraphImage image;
        std::vector<rp1_node_t> aggregators;

        std::unordered_map<std::string, MainlineScalarReady> scalarReady;
        std::uint8_t  joinBucket = static_cast<std::uint8_t>(nodeBuckets);
        std::uint32_t joinBit = 0;

        /*
         * A direct await is possible only within one bucket.  For a multi-
         * bucket join, emit one silent collector per source bucket and recurse
         * until the resulting bits share a bucket.
         */
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

        /*
         * Emit typed work first so every deferred fixup points at final packet
         * and argument indices.  Collector packets are appended afterwards;
         * barrier semantics, not vector order, govern activation.
         */
        emitMainlineCommands(
            dg, N, posOf, slotAlloc, image, scalarReady,
            extraLeafGroups, nextExtraPos, nodeBucketOf,
            nodeBitOf, resolveAwait);

        /*
         * Join every unconsumed command or scalar-read leaf into the sentinel.
         * The sentinel owns reserved bit 31 and is also cleared between launches.
         */
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

    /*
     * Finish a control image by joining unconsumed bucket-0 events.  Control
     * helpers mark every dependency they absorb, so only true mainline leaves
     * gate the lifecycle sentinel; body reset-domain bits never escape here.
     */
    void finishControlImage(
        fpga::Rp1GraphImage& image,
        const std::unordered_map<std::string, std::uint32_t>& mainBit,
        const std::vector<std::string>& consumed) const {
        std::uint32_t sentinelMask = 0;
        for (const auto& [id, bit] : mainBit) {
            if (std::find(consumed.begin(), consumed.end(), id) ==
                consumed.end()) {
                sentinelMask |= bit;
            }
        }
        rp1_node_t sentinel{};
        sentinel.opcode = RP1_OP_SIGNAL;
        sentinel.status = RP1_NODE_PENDING;
        sentinel.barrier_await_bucket = kSentinelBucket;
        sentinel.barrier_await_mask = sentinelMask;
        sentinel.barrier_set_bucket = kSentinelBucket;
        sentinel.barrier_set_mask = kSentinelBit;
        sentinel.payload.signal.target_slot = sentinelSlot_;
        sentinel.payload.signal.value = sentinelValue_;
        sentinel.payload.signal.operation = RP1_SIGOP_SET;
        image.nodes.push_back(sentinel);
        image.clear_signal_slots.push_back(sentinelSlot_);
        clearHandshakeSlots(image);
        if (image.nodes.size() > RP1_MAX_NODES) {
            throw std::logic_error(
                "FpgaDevice: control-flow image exceeds RP1_MAX_NODES");
        }
    }

    /*
     * Lower autonomous loops and conditionals into one image.  Mainline
     * completion bits live in bucket 0; each body owns reset-domain buckets.
     * Fixed/while/follower loops, predicate branches, SIGNAL, WAIT, PDI, and
     * kernels are supported; nested control and top-level boundaries reject.
     */
    fpga::Rp1GraphImage buildControlImage(const Rp1QueueProgram& dg) {
        /*
         * Reserve signal resources before scalar outputs, and reserve bucket 0
         * for mainline/control completion.  Body domains allocate upward from
         * bucket 1 so LOOP/COND can clear them independently.
         */
        fpga::Rp1GraphImage image;
        fpga::LoopIdAllocator loopIds;
        fpga::SignalSlotAllocator slotAlloc;
        slotAlloc.reserve(sentinelSlot_);
        reserveReferencedSignalSlots(dg, slotAlloc);

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

        /*
         * Emit commands in typed-program order while preserving symbolic DAG
         * awaits.  Kernel output reads replace the dispatch as the visible leaf;
         * control helpers publish one bucket-0 completion bit per construct.
         */
        for (const Rp1Command& node : dg.commands) {
            if (const auto* k = std::get_if<Rp1KernelCommand>(&node)) {
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
                    requireSignalSlotScalar(k->kernel, sp);
                    auto bindIt =
                        k->ioMap.outputScalars().find(sp.name);
                    const std::string key =
                        bindIt == k->ioMap.outputScalars().end()
                            ? std::string{}
                            : scopedScalarKey(
                                  bindIt->second.scopeId(),
                                  bindIt->second.varName());
                    const std::uint32_t slot =
                        acquireScalarSlot(key, slotAlloc);
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
                    if (bindIt != k->ioMap.outputScalars().end()) {
                        scalarSlots_[key] = slot;
                    }
                    consumedMain.push_back(lastId);
                    lastId  = k->id + ".sread." + sp.name;
                    lastBit = rbit;
                    mainBit[lastId] = rbit;
                }
            } else if (const auto* r = std::get_if<Rp1ReprogramCommand>(&node)) {
                const std::uint32_t aw  = awaitMaskFor(r->dependsOn);
                const std::uint32_t bit = allocMainBit();
                emitReprogramPacket(image, *r, 0, aw, 0, bit);
                mainBit[r->id] = bit;
            } else if (const auto* loop = std::get_if<Rp1LoopCommand>(&node)) {
                lowerLoop(image, dg, *loop, loopIds, slotAlloc, mainBit, consumedMain,
                          awaitMaskFor, allocMainBit, nextBodyBkt);
            } else if (const auto* cnd = std::get_if<Rp1ConditionalCommand>(&node)) {
                lowerConditional(image, dg, *cnd, slotAlloc, mainBit, consumedMain,
                                 awaitMaskFor, allocMainBit, nextBodyBkt);
            } else if (const auto* sg = std::get_if<Rp1SignalCommand>(&node)) {
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
            } else if (const auto* wt = std::get_if<Rp1WaitCommand>(&node)) {
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
            } else if (std::holds_alternative<Rp1BoundaryCommand>(node)) {
                throw std::logic_error(
                    "FpgaDevice: top-level region boundary in a control Rp1QueueProgram is "
                    "not supported (node '" + rp1CommandId(node) + "')");
            } else {
                throw std::logic_error(
                    "FpgaDevice: unexpected node in control Rp1QueueProgram '" +
                    rp1CommandId(node) + "'");
            }
        }

        finishControlImage(image, mainBit, consumedMain);
        return image;
    }

    struct BarrierRef {
        std::uint8_t bucket = 0;
        std::uint32_t mask = 0;
    };

    /*
     * Allocate barriers for one resettable control body.  Events may span
     * contiguous buckets; collector NOPs collapse cross-bucket awaits, and the
     * recorded range tells LOOP which buckets to clear before each iteration.
     */
    class ResetDomainEmitter {
       public:
        ResetDomainEmitter(fpga::Rp1GraphImage& image, std::uint8_t& nextBucket)
            : image_(image), nextBucket_(nextBucket) {}

        BarrierRef define(const std::string& id) {
            BarrierRef ref = allocBit();
            events_[id] = ref;
            return ref;
        }

        void gateRoots(const std::string& id) {
            rootGate_ = id;
        }

        std::vector<BarrierRef> refsFor(const std::vector<std::string>& deps) {
            std::vector<BarrierRef> refs;
            for (const auto& dep : deps) {
                auto it = events_.find(dep);
                if (it == events_.end()) continue;
                refs.push_back(it->second);
                consumed_.insert(dep);
            }
            if (refs.empty() && rootGate_) {
                auto gate = events_.find(*rootGate_);
                if (gate != events_.end()) {
                    refs.push_back(gate->second);
                    consumed_.insert(*rootGate_);
                }
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

            /*
             * RP1 await fields name one bucket.  Project each source bucket to
             * a fresh collector bit, then recurse in case collector allocation
             * itself crossed a bucket boundary.
             */
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
        std::optional<std::string> rootGate_;
        std::uint8_t currentBucket_ = 0;
        std::uint32_t nextBit_ = 0;
        std::uint8_t startBucket_ = 0;
        std::uint8_t endBucket_ = 0;
        bool haveBucket_ = false;
    };

    using LoopBufferTokens =
        std::unordered_map<std::string, GraphBuffer>;
    using LoopScalarAliases =
        std::unordered_map<std::string, std::string>;

    /*
     * Followers terminate exclusively from the authority broadcast.  Local
     * loops instead require either an RP1-evaluable while predicate or a
     * launch-resolved fixed trip count; no other duration encoding is valid.
     */
    static void validateLoopCommand(
        const Rp1LoopCommand& loop, bool follower, bool whileLoop) {
        if (follower) return;
        if (whileLoop) {
            if (!loop.condition ||
                !fpga::isRp1EvaluableCondition(*loop.condition)) {
                throw std::logic_error(
                    "FpgaDevice: while-loop '" + loop.id +
                    "' requires an RP1-evaluable condition");
            }
            return;
        }
        if (loop.loopKind != Rp1LoopKind::FixedCount ||
            !loop.tripCount) {
            throw std::logic_error(
                "FpgaDevice: autonomous fixed-count loop '" +
                loop.id + "' is missing its scalar trip count");
        }
    }

    /*
     * Boundary aliases need one sized token even though boundaries carry only
     * names and scopes.  Collect all body kernel buffers so either side of an
     * import/export edge can supply the allocation size.
     */
    static LoopBufferTokens collectLoopBufferTokens(
        const std::vector<const Rp1Command*>& commands) {
        LoopBufferTokens tokens;
        auto note = [&](const GraphBuffer& buffer) {
            if (buffer.valid() && buffer.hasSizeScalar()) {
                tokens.emplace(
                    scopedBufferKey(
                        buffer.scopeId(), buffer.name()),
                    buffer);
            }
        };
        for (const Rp1Command* command : commands) {
            const auto* kernel =
                std::get_if<Rp1KernelCommand>(command);
            if (!kernel) continue;
            for (const auto& [port, buffer] :
                 kernel->ioMap.inputs()) {
                (void)port;
                note(buffer);
            }
            for (const auto& [port, buffer] :
                 kernel->ioMap.outputs()) {
                (void)port;
                note(buffer);
            }
            for (const auto& binding : kernel->ioMap.inouts()) {
                note(binding.in);
                note(binding.out);
            }
        }
        return tokens;
    }

    /*
     * Normalize scalar boundaries as local-to-parent maps.  Imports feed a
     * kernel register from the parent's carried slot; exports direct a kernel
     * result back into that same persistent namespace.
     */
    static std::pair<LoopScalarAliases, LoopScalarAliases>
    collectLoopScalarAliases(
        const std::vector<const Rp1Command*>& commands) {
        LoopScalarAliases imports;
        LoopScalarAliases exports;
        for (const Rp1Command* command : commands) {
            const auto* boundary =
                std::get_if<Rp1BoundaryCommand>(command);
            if (!boundary) continue;
            const bool start =
                boundary->side == Rp1BoundaryCommand::Side::Start;
            for (const Rp1ScalarBoundaryCopy& copy :
                 boundary->scalarCopies) {
                const std::string parent = scopedScalarKey(
                    start ? copy.sourceScopeId : copy.targetScopeId,
                    start ? copy.sourceName : copy.targetName);
                const std::string local = scopedScalarKey(
                    start ? copy.targetScopeId : copy.sourceScopeId,
                    start ? copy.targetName : copy.sourceName);
                (start ? imports : exports)[local] = parent;
            }
        }
        return {std::move(imports), std::move(exports)};
    }

    /*
     * Emit one loop-body kernel with carried scalar plumbing.  Imported values
     * become pre-dispatch SCALAR_COPY predecessors; output SCALAR_READ packets
     * either update exported parent slots or allocate body-local result slots.
     */
    void emitLoopKernel(
        fpga::Rp1GraphImage& image, const Rp1LoopCommand& loop,
        const Rp1KernelCommand& kernel,
        ResetDomainEmitter& domain,
        fpga::SignalSlotAllocator& slotAlloc,
        const LoopScalarAliases& imports,
        const LoopScalarAliases& exports,
        const std::function<std::uint32_t(const std::string&)>&
            carriedSlot) {
        if (kernel.kernel.type != DeviceType::FPGA) {
            throw std::logic_error(
                "FpgaDevice: loop '" + loop.id +
                "' contains a non-FPGA kernel");
        }
        std::set<std::string> carriedInputs;
        std::vector<BarrierRef> awaits =
            domain.refsFor(kernel.dependsOn);
        const FpgaKernelLocation location =
            device_->resolveKernelLocation(kernel.kernel);
        const auto offsets = inputScalarRegOffsets(kernel.kernel);

        /*
         * Every carried copy awaits the kernel's graph predecessors and the
         * dispatch then awaits all copies, preventing stale iteration values
         * from reaching AXI-Lite argument registers.
         */
        for (const ScalarPort& port :
             kernel.kernel.ioType.inputScalars) {
            auto binding = kernel.ioMap.inputScalars().find(port.name);
            if (binding == kernel.ioMap.inputScalars().end()) continue;
            auto imported = imports.find(scopedScalarKey(
                binding->second.scopeId(),
                binding->second.varName()));
            if (imported == imports.end()) continue;
            carriedInputs.insert(port.name);
            const BarrierRef await = domain.awaitFor(awaits);
            const BarrierRef done =
                domain.define(kernel.id + ".scopy." + port.name);
            rp1_node_t packet{};
            packet.opcode = RP1_OP_SCALAR_COPY;
            packet.status = RP1_NODE_PENDING;
            packet.barrier_await_bucket = await.bucket;
            packet.barrier_await_mask = await.mask;
            packet.barrier_set_bucket = done.bucket;
            packet.barrier_set_mask = done.mask;
            packet.payload.scalar_copy.source_slot =
                carriedSlot(imported->second);
            packet.payload.scalar_copy.dest_addr =
                location.r5_base_addr + offsets.at(port.name);
            image.nodes.push_back(packet);
            awaits.push_back(done);
        }
        const BarrierRef kernelAwait = domain.awaitFor(awaits);
        BarrierRef lastDone = domain.define(kernel.id);
        emitKernelPacket(
            image, kernel, kernelAwait.bucket, kernelAwait.mask,
            lastDone.bucket, lastDone.mask, carriedInputs);

        /*
         * Chain reads so the final scalar publication is the kernel leaf.
         * Exported results reuse the parent slot, which is intentionally not
         * cleared with per-iteration handshake state.
         */
        for (const ScalarPort& port :
             kernel.kernel.ioType.outputScalars) {
            requireSignalSlotScalar(kernel.kernel, port);
            auto binding =
                kernel.ioMap.outputScalars().find(port.name);
            const std::string localKey =
                binding == kernel.ioMap.outputScalars().end()
                    ? std::string{}
                    : scopedScalarKey(
                          binding->second.scopeId(),
                          binding->second.varName());
            auto exported = exports.find(localKey);
            const std::uint32_t slot =
                !localKey.empty() && exported != exports.end()
                    ? carriedSlot(exported->second)
                    : acquireScalarSlot(localKey, slotAlloc);
            const BarrierRef readDone =
                domain.define(kernel.id + ".sread." + port.name);
            rp1_node_t packet{};
            packet.opcode = RP1_OP_SCALAR_READ;
            packet.status = RP1_NODE_PENDING;
            packet.barrier_await_bucket = lastDone.bucket;
            packet.barrier_await_mask = lastDone.mask;
            packet.barrier_set_bucket = readDone.bucket;
            packet.barrier_set_mask = readDone.mask;
            packet.payload.scalar_read.source_addr =
                location.r5_base_addr +
                device_->outputScalarRegOffset(
                    kernel.kernel, port.name);
            packet.payload.scalar_read.target_slot = slot;
            image.nodes.push_back(packet);
            if (!localKey.empty()) scalarSlots_[localKey] = slot;
            lastDone = readDone;
        }
    }

    /*
     * Buffer boundaries are zero-copy publications, not RP1 packets.  Defer the
     * alias until launch because the parent input may be staged asynchronously
     * and either boundary endpoint may be the first token with known storage.
     */
    void emitLoopBoundary(
        const Rp1BoundaryCommand& boundary,
        const LoopBufferTokens& tokens) {
        for (const Rp1BufferBoundaryCopy& copy :
             boundary.bufferCopies) {
            const std::string source = scopedBufferKey(
                copy.sourceScopeId, copy.sourceName);
            const std::string target = scopedBufferKey(
                copy.targetScopeId, copy.targetName);
            auto token = tokens.find(target);
            if (token == tokens.end()) token = tokens.find(source);
            if (token == tokens.end()) {
                throw std::runtime_error(
                    "FpgaDevice: boundary alias has no sized token");
            }
            deferredBufferAliases_.push_back(DeferredBufferAlias{
                source, target, token->second, boundary.id});
        }
    }

    static void emitLoopSignal(
        fpga::Rp1GraphImage& image, const Rp1SignalCommand& signal,
        ResetDomainEmitter& domain) {
        const BarrierRef await =
            domain.awaitFor(domain.refsFor(signal.dependsOn));
        const BarrierRef done = domain.define(signal.id);
        rp1_node_t packet{};
        packet.opcode = RP1_OP_SIGNAL;
        packet.status = RP1_NODE_PENDING;
        packet.barrier_await_bucket = await.bucket;
        packet.barrier_await_mask = await.mask;
        packet.barrier_set_bucket = done.bucket;
        packet.barrier_set_mask = done.mask;
        packet.payload.signal.target_slot = signal.slot;
        packet.payload.signal.value = signal.value;
        packet.payload.signal.operation = signal.operation;
        image.nodes.push_back(packet);
    }

    static void emitLoopWait(
        fpga::Rp1GraphImage& image, const Rp1WaitCommand& wait,
        ResetDomainEmitter& domain) {
        const BarrierRef await =
            domain.awaitFor(domain.refsFor(wait.dependsOn));
        const BarrierRef done = domain.define(wait.id);
        rp1_node_t packet{};
        packet.opcode = RP1_OP_WAIT;
        packet.status = RP1_NODE_PENDING;
        packet.barrier_await_bucket = await.bucket;
        packet.barrier_await_mask = await.mask;
        packet.barrier_set_bucket = done.bucket;
        packet.barrier_set_mask = done.mask;
        packet.payload.wait.condition_signal = wait.slot;
        packet.payload.wait.condition_value = wait.value;
        packet.payload.wait.condition_op = wait.conditionOp;
        image.nodes.push_back(packet);
    }

    /*
     * Before a follower enters LOOP, consume one authority-ready generation:
     * wait for ready, clear it, then acknowledge.  Serial bucket-0 bits prevent
     * a stale ready value from satisfying the next generation.
     */
    template <class AllocFn>
    static std::uint32_t emitFollowerInitialHandshake(
        fpga::Rp1GraphImage& image, const Rp1LoopCommand& loop,
        std::uint32_t loopAwait, AllocFn allocMainBit) {
        if (loop.broadcastRole != Rp1SplitRole::Follower) {
            return loopAwait;
        }

        const std::uint32_t waited = allocMainBit();
        rp1_node_t wait{};
        wait.opcode = RP1_OP_WAIT;
        wait.status = RP1_NODE_PENDING;
        wait.barrier_await_bucket = 0;
        wait.barrier_await_mask = loopAwait;
        wait.barrier_set_bucket = 0;
        wait.barrier_set_mask = waited;
        wait.payload.wait.condition_signal =
            loop.broadcastReadySlot;
        wait.payload.wait.condition_op = RP1_COP_AND_NZ;
        wait.payload.wait.condition_value = 1;
        image.nodes.push_back(wait);

        const std::uint32_t cleared = allocMainBit();
        rp1_node_t clear{};
        clear.opcode = RP1_OP_SIGNAL;
        clear.status = RP1_NODE_PENDING;
        clear.barrier_await_bucket = 0;
        clear.barrier_await_mask = waited;
        clear.barrier_set_bucket = 0;
        clear.barrier_set_mask = cleared;
        clear.payload.signal.target_slot =
            loop.broadcastReadySlot;
        clear.payload.signal.value = 0;
        clear.payload.signal.operation = RP1_SIGOP_SET;
        image.nodes.push_back(clear);

        const std::uint32_t acknowledged = allocMainBit();
        rp1_node_t acknowledge{};
        acknowledge.opcode = RP1_OP_SIGNAL;
        acknowledge.status = RP1_NODE_PENDING;
        acknowledge.barrier_await_bucket = 0;
        acknowledge.barrier_await_mask = cleared;
        acknowledge.barrier_set_bucket = 0;
        acknowledge.barrier_set_mask = acknowledged;
        acknowledge.payload.signal.target_slot =
            loop.broadcastAckSlot;
        acknowledge.payload.signal.value = 1;
        acknowledge.payload.signal.operation = RP1_SIGOP_SET;
        image.nodes.push_back(acknowledge);
        return acknowledged;
    }

    /*
     * Repeat the follower handshake after each body iteration.  RERUN must
     * await the acknowledgement so authority and follower cannot advance their
     * shared predicate generation at different rates.
     */
    static BarrierRef emitFollowerHandshake(
        fpga::Rp1GraphImage& image, const Rp1LoopCommand& loop,
        ResetDomainEmitter& domain, BarrierRef bodyLeaves) {
        if (loop.broadcastRole != Rp1SplitRole::Follower) {
            return bodyLeaves;
        }
        const BarrierRef waited =
            domain.define(loop.id + ".broadcast_wait");
        rp1_node_t wait{};
        wait.opcode = RP1_OP_WAIT;
        wait.status = RP1_NODE_PENDING;
        wait.barrier_await_bucket = bodyLeaves.bucket;
        wait.barrier_await_mask = bodyLeaves.mask;
        wait.barrier_set_bucket = waited.bucket;
        wait.barrier_set_mask = waited.mask;
        wait.payload.wait.condition_signal =
            loop.broadcastReadySlot;
        wait.payload.wait.condition_op = RP1_COP_AND_NZ;
        wait.payload.wait.condition_value = 1;
        image.nodes.push_back(wait);

        const BarrierRef cleared =
            domain.define(loop.id + ".broadcast_clear");
        rp1_node_t clear{};
        clear.opcode = RP1_OP_SIGNAL;
        clear.status = RP1_NODE_PENDING;
        clear.barrier_await_bucket = waited.bucket;
        clear.barrier_await_mask = waited.mask;
        clear.barrier_set_bucket = cleared.bucket;
        clear.barrier_set_mask = cleared.mask;
        clear.payload.signal.target_slot =
            loop.broadcastReadySlot;
        clear.payload.signal.value = 0;
        clear.payload.signal.operation = RP1_SIGOP_SET;
        image.nodes.push_back(clear);

        const BarrierRef acknowledged =
            domain.define(loop.id + ".broadcast_ack");
        rp1_node_t acknowledge{};
        acknowledge.opcode = RP1_OP_SIGNAL;
        acknowledge.status = RP1_NODE_PENDING;
        acknowledge.barrier_await_bucket = cleared.bucket;
        acknowledge.barrier_await_mask = cleared.mask;
        acknowledge.barrier_set_bucket = acknowledged.bucket;
        acknowledge.barrier_set_mask = acknowledged.mask;
        acknowledge.payload.signal.target_slot =
            loop.broadcastAckSlot;
        acknowledge.payload.signal.value = 1;
        acknowledge.payload.signal.operation = RP1_SIGOP_SET;
        image.nodes.push_back(acknowledge);
        return acknowledged;
    }

    /*
     * Body bounds and termination fields are unknowable until emission ends.
     * Followers use the authority slot, while-loops invert their continue
     * predicate for RP1's exit test, and fixed loops await launch-time count.
     */
    void backpatchLoopPacket(
        fpga::Rp1GraphImage& image, const Rp1LoopCommand& loop,
        std::size_t loopIndex, std::size_t rerunIndex,
        bool whileLoop, const ResetDomainEmitter& domain,
        fpga::LoopIdAllocator& loopIds) {
        auto& payload = image.nodes[loopIndex].payload.loop;
        payload.body_start =
            static_cast<std::uint32_t>(loopIndex + 1);
        payload.body_end =
            static_cast<std::uint32_t>(rerunIndex);
        if (loop.broadcastRole == Rp1SplitRole::Follower) {
            // RP1 uses zero for predicate-governed duration. The authority's
            // broadcast is the only termination source; a local cap would let
            // the follower leave the shared handshake prematurely.
            payload.max_iterations = 0;
            payload.condition_signal =
                loop.conditionBroadcastSlot;
            payload.condition_value = 1;
            payload.condition_op = RP1_COP_AND_NZ;
        } else if (whileLoop) {
            const fpga::Rp1Compare condition =
                fpga::mapRp1Condition(*loop.condition);
            const std::string key = scopedScalarKey(
                condition.scalarScopeId, condition.scalarName);
            auto slot = scalarSlots_.find(key);
            if (slot == scalarSlots_.end()) {
                throw std::logic_error(
                    "FpgaDevice: loop predicate scalar is not "
                    "produced by the body");
            }
            payload.max_iterations = 0;
            payload.condition_signal = slot->second;
            payload.condition_value = condition.value;
            payload.condition_op =
                fpga::invertRp1Op(condition.op);
        } else {
            payload.max_iterations = 1;
            payload.condition_signal = 0;
            payload.condition_value = fpga::kNeverValue;
            payload.condition_op = fpga::kNeverOp;
        }
        payload.bucket_clear_start = domain.clearStart();
        payload.bucket_clear_end = domain.clearEnd();
        payload.loop_id = loopIds.alloc();
    }

    /*
     * Flatten fixed, while, or split-follower loops as LOOP, a gated reset
     * domain, and RERUN.  Fixed counts are launch-patched; while predicates
     * come from body SCALAR_READ; followers use authority handshakes.  Buffer
     * boundaries become aliases and scalar boundaries become carried slots.
     */
    template <class AwaitFn, class AllocFn>
    void lowerLoop(fpga::Rp1GraphImage& image, const Rp1QueueProgram& dg,
                   const Rp1LoopCommand& loop, fpga::LoopIdAllocator& loopIds,
                   fpga::SignalSlotAllocator& slotAlloc,
                   std::unordered_map<std::string, std::uint32_t>& mainBit,
                   std::vector<std::string>& consumedMain,
                   AwaitFn awaitMaskFor, AllocFn allocMainBit,
                   std::uint8_t& nextBodyBkt) {
        const bool isFollower =
            loop.broadcastRole == Rp1SplitRole::Follower;
        const bool isWhile =
            !isFollower && loop.loopKind == Rp1LoopKind::WhileCondition;
        validateLoopCommand(loop, isFollower, isWhile);

        /*
         * Gather and validate the complete role before allocating barriers.
         * An empty or off-device body cannot be represented by an autonomous
         * LOOP packet and must remain on the host control path.
         */
        const std::vector<const Rp1Command*> bodyNodes =
            collectControlBody(dg, loop.id, Rp1ChildRole::LoopBody);
        if (bodyNodes.empty()) {
            throw std::logic_error(
                "FpgaDevice: loop '" + loop.id +
                "' has no FPGA body nodes (a loop body running entirely on another "
                "device is not yet supported)");
        }

        /*
         * The LOOP completion bit stays in bucket 0; all body bits belong to a
         * reset domain.  A pure-boolean COND raises the body gate, allowing a
         * launch-patched zero trip count to suppress the first iteration.
         */
        const std::uint32_t loopAwait = awaitMaskFor(loop.dependsOn);
        const std::uint32_t initialLoopAwait =
            emitFollowerInitialHandshake(
                image, loop, loopAwait, allocMainBit);
        const std::uint32_t exitBit   = allocMainBit();
        ResetDomainEmitter bodyDomain(image, nextBodyBkt);
        const std::string gateId = loop.id + ".body_gate";
        const BarrierRef bodyGate = bodyDomain.define(gateId);
        bodyDomain.gateRoots(gateId);

        // LOOP packet (body_start/end backpatched after the body is emitted).
        rp1_node_t loopPkt{};
        loopPkt.opcode               = RP1_OP_LOOP;
        loopPkt.status               = RP1_NODE_PENDING;
        loopPkt.barrier_await_bucket = 0;
        loopPkt.barrier_await_mask   = initialLoopAwait;
        loopPkt.barrier_set_bucket   = 0;
        loopPkt.barrier_set_mask     = exitBit;
        const std::size_t loopIdx = image.nodes.size();
        image.nodes.push_back(loopPkt);
        rp1_node_t gate{};
        gate.opcode = RP1_OP_COND;
        gate.status = RP1_NODE_PENDING;
        gate.barrier_set_bucket = 0;
        gate.barrier_set_mask = 0;
        gate.payload.cond.body_start = 1;
        gate.payload.cond.body_end = 0;
        gate.payload.cond.bucket_clear_start = 1;
        gate.payload.cond.bucket_clear_end = 0;
        gate.payload.cond.done_bucket = bodyGate.bucket;
        gate.payload.cond.done_mask = bodyGate.mask;
        const std::size_t gateIdx = image.nodes.size();
        image.nodes.push_back(gate);

        /*
         * Only local fixed loops need a deferred count.  While and follower
         * termination is already encoded by a signal predicate.
         */
        if (!isFollower && !isWhile) {
            deferredTripCounts_.push_back(DeferredLoopTripCount{
                loopIdx, gateIdx,
                scopedScalarKey(
                    loop.tripCount->scopeId(),
                    loop.tripCount->name()),
                loop.tripCount->name(),
                loop.tripCount->scopeId() == 0,
                loop.tripCount->type(),
                loop.id + "." + loop.tripCount->name()});
        }

        /*
         * Discover carried data before packet emission.  One parent scalar slot
         * is shared by all import/export aliases, while buffer aliases retain a
         * sized token for launch-time allocation.
         */

        const LoopBufferTokens tokenBuffers =
            collectLoopBufferTokens(bodyNodes);
        auto aliases = collectLoopScalarAliases(bodyNodes);
        const LoopScalarAliases& scalarImport = aliases.first;
        const LoopScalarAliases& scalarExport = aliases.second;
        auto carriedSlot = [&](const std::string& parentKey) {
            auto existing = scalarSlots_.find(parentKey);
            if (existing != scalarSlots_.end()) return existing->second;
            const std::uint32_t slot =
                acquireScalarSlot(parentKey, slotAlloc);
            scalarSlots_[parentKey] = slot;
            return slot;
        };

        /*
         * Lower body commands into the reset domain.  Kernels, PDI loads,
         * signals, and waits receive barriers; boundaries only record aliases.
         * Nested control cannot share this loop's reset contract.
         */
        for (const Rp1Command* command : bodyNodes) {
            if (const auto* kernel =
                    std::get_if<Rp1KernelCommand>(command)) {
                emitLoopKernel(
                    image, loop, *kernel, bodyDomain, slotAlloc,
                    scalarImport, scalarExport, carriedSlot);
            } else if (const auto* reprogram =
                           std::get_if<Rp1ReprogramCommand>(command)) {
                const BarrierRef await = bodyDomain.awaitFor(
                    bodyDomain.refsFor(reprogram->dependsOn));
                const BarrierRef done = bodyDomain.define(reprogram->id);
                emitReprogramPacket(
                    image, *reprogram, await.bucket, await.mask,
                    done.bucket, done.mask);
            } else if (const auto* boundary =
                           std::get_if<Rp1BoundaryCommand>(command)) {
                emitLoopBoundary(*boundary, tokenBuffers);
            } else if (const auto* signal =
                           std::get_if<Rp1SignalCommand>(command)) {
                emitLoopSignal(image, *signal, bodyDomain);
            } else if (const auto* wait =
                           std::get_if<Rp1WaitCommand>(command)) {
                emitLoopWait(image, *wait, bodyDomain);
            } else {
                throw std::logic_error(
                    "FpgaDevice: autonomous loop body contains nested control");
            }
        }

        /*
         * RERUN joins every body leaf, including the follower acknowledgement,
         * and re-arms LOOP.  Its bit is inside the cleared range so no prior
         * iteration can satisfy a later one.
         */
        BarrierRef bodyLeaves = emitFollowerHandshake(
            image, loop, bodyDomain, bodyDomain.mutableLeafAwait());

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

        /*
         * Patch the final body range, reset range, and termination predicate.
         * Mirror that exit predicate into the body gate with opposite polarity,
         * then expose LOOP's bucket-0 exit bit to downstream mainline work.
         */
        backpatchLoopPacket(
            image, loop, loopIdx, rerunIdx, isWhile,
            bodyDomain, loopIds);
        const auto& loopPayload =
            image.nodes[loopIdx].payload.loop;
        auto& gatePayload = image.nodes[gateIdx].payload.cond;
        gatePayload.condition_signal =
            loopPayload.condition_signal;
        gatePayload.condition_value =
            loopPayload.condition_value;
        gatePayload.condition_op =
            fpga::invertRp1Op(
                static_cast<rp1_condop_t>(
                    loopPayload.condition_op));
        mainBit[loop.id] = exitBit;
    }

    /*
     * Lower an autonomous conditional as two pure-boolean CONDs.  Each taken
     * non-empty branch raises a private go bit that gates its roots; empty
     * branches resolve directly.  Both paths set one bucket-0 OR-join bit, so
     * downstream work is independent of which predicate arm ran.
     */
    template <class AwaitFn, class AllocFn>
    void lowerConditional(fpga::Rp1GraphImage& image, const Rp1QueueProgram& dg,
                          const Rp1ConditionalCommand& cond,
                          fpga::SignalSlotAllocator& slotAlloc,
                          std::unordered_map<std::string, std::uint32_t>& mainBit,
                          std::vector<std::string>& consumedMain,
                          AwaitFn awaitMaskFor, AllocFn allocMainBit,
                          std::uint8_t& nextBodyBkt) {
        (void)slotAlloc;

        /*
         * RP1 can compare one integer signal against a constant.  The signal
         * must already have been captured by a mainline SCALAR_READ so branch
         * predicates cannot race a producer still inside either branch.
         */
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

        /*
         * Exactly one arm sets this shared completion bit.  It stays in bucket
         * 0 because branch reset-domain bits are private gating state.
         */
        const std::uint32_t condDone = allocMainBit();

        /*
         * Emit one arm with the appropriate predicate polarity.  A body gets a
         * private go bit and leaf join; an empty arm points its COND directly at
         * condDone.  The return value distinguishes two empty arms from one.
         */
        auto emitBranch = [&](Rp1ChildRole role, bool runWhenTrue) -> bool {
            const std::vector<const Rp1Command*> bodyNodes =
                collectControlBody(dg, cond.id, role);
            const bool hasBody = !bodyNodes.empty();

            /*
             * mapRp1Condition() encodes predicate truth.  Keep it for then and
             * invert it for else so only one COND can raise its arm's done bit.
             */
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

            /*
             * A root with no intra-branch predecessor must still await goBit.
             * Non-roots keep their graph dependencies, whose roots already
             * establish the branch gate transitively.
             */
            auto branchAwait = [&](const std::vector<std::string>& deps) -> BarrierRef {
                auto refs = branchDomain.refsFor(deps);
                if (refs.empty()) refs.push_back(goBit);
                return branchDomain.awaitFor(refs);
            };

            /*
             * Branches currently admit FPGA kernels, PDI loads, and empty
             * metadata boundaries.  Data-carrying boundaries and nested event
             * or control nodes require publication semantics not yet encoded.
             */
            for (const Rp1Command* bnp : bodyNodes) {
                const Rp1Command& bn = *bnp;
                if (const auto* k = std::get_if<Rp1KernelCommand>(&bn)) {
                    if (k->kernel.type != DeviceType::FPGA) {
                        throw std::logic_error(
                            "FpgaDevice: conditional '" + cond.id + "' branch kernel '" +
                            k->kernel.name + "' is not an FPGA kernel");
                    }
                    const BarrierRef aw = branchAwait(k->dependsOn);
                    const BarrierRef done = branchDomain.define(k->id);
                    emitKernelPacket(image, *k, aw.bucket, aw.mask, done.bucket, done.mask);
                } else if (const auto* r = std::get_if<Rp1ReprogramCommand>(&bn)) {
                    const BarrierRef aw = branchAwait(r->dependsOn);
                    const BarrierRef done = branchDomain.define(r->id);
                    emitReprogramPacket(image, *r, aw.bucket, aw.mask, done.bucket, done.mask);
                } else if (const auto* bb = std::get_if<Rp1BoundaryCommand>(&bn)) {
                    if (!bb->scalarCopies.empty() || !bb->bufferCopies.empty()) {
                        throw std::logic_error(
                            "FpgaDevice: conditional '" + cond.id + "' branch boundary '" +
                            bb->id + "' carries data; autonomous conditional outputs are a "
                            "future phase");
                    }
                } else {
                    throw std::logic_error(
                        "FpgaDevice: conditional '" + cond.id + "' branch node '" +
                        rp1CommandId(bn) + "' is unsupported (only FPGA kernels/reprograms)");
                }
            }

            /*
             * Collapse all branch leaves and project them onto condDone.
             * The untaken branch never raises its go bit, so its join stays
             * dormant and cannot double-complete the OR join.
             */
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

        const bool thenBody = emitBranch(Rp1ChildRole::ConditionalThen, /*runWhenTrue=*/true);
        const bool elseBody = emitBranch(Rp1ChildRole::ConditionalElse, /*runWhenTrue=*/false);
        if (!thenBody && !elseBody) {
            throw std::logic_error(
                "FpgaDevice: conditional '" + cond.id + "' has no FPGA branch bodies");
        }
        mainBit[cond.id] = condDone;
    }

    std::shared_ptr<FpgaDevice>                                device_;
    std::unique_ptr<IDeviceExecutionLease>                     executionLease_;
    std::shared_ptr<fpga::Rp1Submitter>                       submitter_;
    fpga::Rp1GraphImage                                        image_;
    std::vector<DeferredScalar>                                deferred_;
    std::vector<DeferredLoopTripCount>                         deferredTripCounts_;
    std::vector<DeferredPdi>                                   deferredPdis_;
    std::map<std::size_t, std::string>                         pdiImagesByNode_;
    std::vector<DeferredBufferAddress>                         deferredBufferAddresses_;
    std::vector<DeferredBufferAlias>                           deferredBufferAliases_;
    std::shared_ptr<BackendRuntimeState>                       runtimeState_;
    std::shared_ptr<std::map<std::string, std::uint64_t>>      scalarValues_;
    std::uint32_t                                              sentinelSlot_;
    std::uint32_t                                              sentinelValue_;
    std::chrono::milliseconds                                  timeout_;
    std::thread                                                worker_;
    std::exception_ptr                                         workerEx_;
    std::vector<rp1_cq_entry_t>                                lastCq_;
    bool                                                       signalsPrepared_ = false;
    std::map<std::string, BackendScalarId>                      boundScalarSlots_;
    std::set<std::uint32_t>                                     ownedSignalSlots_;
    bool                                                       resourcesLeased_ = false;
    std::vector<std::uint32_t>                                 preLaunchWaitSlots_;
    // Output scalars captured into RP1 signal slots by SCALAR_READ, keyed by
    // scoped name for downstream SCALAR_COPY inputs and control predicates.
    std::unordered_map<std::string, std::uint32_t> scalarSlots_;
    std::shared_ptr<Rp1QueueProgram> directProgram_;
    std::map<NodeId, std::string> authoredControlIds_;
    QueueId queue_;
    bool directImageBuilt_ = false;
    std::vector<std::shared_ptr<::vrt::Buffer<std::uint8_t>>>
        launchBufferPins_;
    std::vector<std::shared_ptr<::vrt::Buffer<std::uint8_t>>>
        launchPdiPins_;

   public:
    const std::unordered_map<std::string, std::uint32_t>& scalarSlots() const noexcept {
        return scalarSlots_;
    }
};

// =========================================================================
// FpgaDevice
// =========================================================================

/*
 * The lookup constructor is the metadata-free mock/bring-up path.  Kernel
 * addresses come from the caller, argument registers use the fallback layout,
 * and buffers remain in the BAR arena unless region metadata appears elsewhere.
 */
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
    scalarSlotAlloc_.reserve(sentinelSlot_);
    submitter_ = std::make_shared<fpga::Rp1Submitter>(*window_, cq_size);
}

/*
 * The vbin constructor enables image guards, exact register offsets, and
 * m_axi-region allocation.  An empty active image is valid: explicit PDI_LOAD
 * commands establish the first image before guarded dispatches can execute.
 */
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
    /*
     * A declared initial image must be resolvable now.  Empty intentionally
     * means unknown/unprogrammed and forces the graph to establish an image.
     */
    if (!activeImageId_.empty() && !vbinSpec_->hasImage(activeImageId_)) {
        throw std::invalid_argument(
            "FpgaDevice: initial image '" + activeImageId_ + "' is not in the vbin spec");
    }
    scalarSlotAlloc_.reserve(sentinelSlot_);
    submitter_ = std::make_shared<fpga::Rp1Submitter>(*window_, cq_size);
}

FpgaDevice::~FpgaDevice() = default;

void FpgaDevice::requireExecutionUsable(
    const char* method) const {
    if (executionPoisoned()) {
        throw std::runtime_error(
            std::string(method) +
            ": device is poisoned after an indeterminate RP1 timeout; "
            "reset/recover the device and create a new FpgaDevice");
    }
}

/*
 * Poison once and retain the entire device through process exit.  An
 * indeterminate timeout means RP1 may still dereference device-owned buffers;
 * failure to establish quarantine is therefore fatal, not recoverable cleanup.
 */
void FpgaDevice::poisonExecution() noexcept {
    bool expected = false;
    if (!executionPoisoned_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(
            poisonedDeviceQuarantineMutex());
        poisonedDeviceQuarantine().push_back(
            shared_from_this());
    } catch (...) {
        // Continuing could release DMA/PDI storage while RP1 still owns it.
        std::terminate();
    }
}

/*
 * Move per-launch HBM/DDR and PDI pins into device-lifetime storage after a
 * poison event.  Releasing either class could recycle memory while stale
 * firmware commands still hold its physical address.
 */
void FpgaDevice::quarantineLaunchPins(
    std::vector<std::shared_ptr<::vrt::Buffer<std::uint8_t>>>
        bufferPins,
    std::vector<std::shared_ptr<::vrt::Buffer<std::uint8_t>>>
        pdiPins) {
    std::lock_guard<std::mutex> lock(quarantineMutex_);
    for (auto& pin : bufferPins) {
        quarantinedBufferPins_.push_back(std::move(pin));
    }
    for (auto& pin : pdiPins) {
        quarantinedPdiPins_.push_back(std::move(pin));
    }
}

/*
 * The submitter is single-flight, so expose one execution lease per device.
 * A poisoned submitter first poisons the device; successful lease destruction
 * reopens execution only when hardware state remained determinate.
 */
std::unique_ptr<IDeviceExecutionLease> FpgaDevice::leaseExecution() {
    if (submitter_ && submitter_->poisoned()) {
        poisonExecution();
    }
    requireExecutionUsable("FpgaDevice::leaseExecution");
    bool expected = false;
    if (!executionLeased_.compare_exchange_strong(
            expected, true, std::memory_order_acquire,
            std::memory_order_relaxed)) {
        return nullptr;
    }
    try {
        return std::make_unique<FpgaExecutionLease>(
            executionLeased_, executionPoisoned_);
    } catch (...) {
        executionLeased_.store(
            false, std::memory_order_release);
        throw;
    }
}

/*
 * One lease owns a disjoint set of signal slots for rendezvous and scalar
 * resources.  Logical IDs remain separate types, but both share RP1's physical
 * namespace and therefore one allocator and one release lock.
 */
class FpgaRendezvousLease final : public IDeviceResourceLease {
   public:
    FpgaRendezvousLease(
        FpgaDevice& device,
        std::map<RendezvousId, BackendResourceId> rendezvous,
        std::map<ScalarResourceId, BackendScalarId> scalars)
        : device_(&device),
          rendezvous_(std::move(rendezvous)),
          scalars_(std::move(scalars)) {}

    ~FpgaRendezvousLease() override {
        /*
         * Poisoned executions deliberately leak their reservations: hardware
         * may still read or write those slots, so reuse would cross-contaminate
         * a future graph even if host ownership has ended.
         */
        if (!device_) return;
        if (device_->executionPoisoned()) return;
        std::lock_guard<std::mutex> lock(device_->scalarMutex_);
        for (const auto& [logical, slot] : rendezvous_) {
            (void)logical;
            device_->scalarSlotAlloc_.release(
                static_cast<std::uint32_t>(slot.value()));
        }
        for (const auto& [logical, slot] : scalars_) {
            (void)logical;
            device_->scalarSlotAlloc_.release(
                static_cast<std::uint32_t>(slot.value()));
        }
    }

    BackendResourceId rendezvousResource(
        RendezvousId logical) const override {
        auto it = rendezvous_.find(logical);
        if (it == rendezvous_.end()) {
            throw std::out_of_range(
                "FpgaRendezvousLease: unknown logical rendezvous");
        }
        return it->second;
    }

    BackendScalarId scalarResource(
        ScalarResourceId logical) const override {
        auto it = scalars_.find(logical);
        if (it == scalars_.end()) {
            throw std::out_of_range(
                "FpgaRendezvousLease: unknown logical scalar");
        }
        return it->second;
    }

   private:
    FpgaDevice* device_ = nullptr;
    std::map<RendezvousId, BackendResourceId> rendezvous_;
    std::map<ScalarResourceId, BackendScalarId> scalars_;
};

/*
 * Resource access is a typed host view over the shared RP1 signal array.
 * Scalars are physically 32-bit despite the generic 64-bit interface; lowering
 * rejects wider scalar publications before they reach this boundary.
 */
class FpgaResourceAccess final : public IDeviceResourceAccess {
   public:
    explicit FpgaResourceAccess(
        std::shared_ptr<fpga::Rp1BarWindow> window)
        : window_(std::move(window)) {}

    std::uint32_t readRendezvous(
        BackendResourceId resource) const override {
        rp1_signal_slot_t slot{};
        window_->readSignal(index(resource), slot);
        return slot.value;
    }

    void writeRendezvous(
        BackendResourceId resource,
        std::uint32_t value) const override {
        write(index(resource), value);
    }

    std::uint64_t readScalar(
        BackendScalarId scalar) const override {
        rp1_signal_slot_t slot{};
        window_->readSignal(index(scalar), slot);
        return slot.value;
    }

    void writeScalar(
        BackendScalarId scalar,
        std::uint64_t value) const override {
        write(index(scalar), static_cast<std::uint32_t>(value));
    }

   private:
    template <class Id>
    static std::uint32_t index(Id id) {
        return static_cast<std::uint32_t>(id.value());
    }

    void write(std::uint32_t slot, std::uint32_t value) const {
        window_->writeU32(static_cast<std::uint32_t>(
                              RP1_DEFAULT_SIG_ARRAY_OFFSET +
                              slot * sizeof(rp1_signal_slot_t) +
                              offsetof(rp1_signal_slot_t, value)),
                          value);
    }

    std::shared_ptr<fpga::Rp1BarWindow> window_;
};

void FpgaDevice::setSentinelSlot(std::uint32_t slot) {
    if (slot >= RP1_MAX_SIGNALS) {
        throw std::invalid_argument(
            "FpgaDevice::setSentinelSlot: slot " + std::to_string(slot) +
            " out of range [0, " + std::to_string(RP1_MAX_SIGNALS) + ")");
    }
    if (slot == sentinelSlot_) return;
    if (sentinelConfigLocked_) {
        throw std::logic_error(
            "FpgaDevice::setSentinelSlot: sentinel reservation is locked "
            "after executable lowering");
    }
    std::lock_guard<std::mutex> lock(scalarMutex_);
    if (scalarSlotAlloc_.isReserved(slot)) {
        throw std::invalid_argument(
            "FpgaDevice::setSentinelSlot: slot " + std::to_string(slot) +
            " is already reserved");
    }
    scalarSlotAlloc_.release(sentinelSlot_);
    sentinelSlot_ = slot;
    scalarSlotAlloc_.reserve(slot);
}

void FpgaDevice::setSentinelValue(std::uint32_t value) {
    if (value == 0u) {
        throw std::invalid_argument(
            "FpgaDevice::setSentinelValue: zero is reserved for "
            "not-completed");
    }
    if (value == sentinelValue_) return;
    if (sentinelConfigLocked_) {
        throw std::logic_error(
            "FpgaDevice::setSentinelValue: sentinel configuration is locked "
            "after executable lowering");
    }
    sentinelValue_ = value;
}

void FpgaDevice::setWaitTimeout(std::chrono::milliseconds t) {
    waitTimeout_ = t;
}

void FpgaDevice::setPdiStagingDevice(::vrt::Device device) {
    requireExecutionUsable(
        "FpgaDevice::setPdiStagingDevice");
    std::lock_guard<std::mutex> lk(pdiMutex_);
    pdiStagingDevice_ = std::make_shared<::vrt::Device>(std::move(device));
    stagedPdis_.clear();
}

DeviceCapabilities FpgaDevice::compilerCapabilities() const {
    DeviceCapabilities result = IDevice::compilerCapabilities();
    result.ownsRendezvousNamespace = true;
    return result;
}

/*
 * Autonomous control requires one non-nested FPGA-local body.  Fixed loops need
 * no device predicate; while loops need a body-produced RP1 comparison.
 * Conditionals additionally reject data boundaries and require a prior
 * FPGA-visible predicate because branch publication is not yet autonomous.
 */
CapabilityDecision FpgaDevice::evaluateControlCapability(
    const ControlCapabilityRequest& request) const {
    auto reject = [&](std::string reason) {
        return CapabilityDecision::reject(DeviceId(id_), std::move(reason));
    };
    if (!request.childHasWork) {
        return reject("control body has no executable work");
    }
    if (request.childHasNestedControl) {
        return reject("nested control is not autonomous on RP1");
    }
    if (request.childDevices.size() != 1 ||
        request.childDevices.front() != DeviceId(id_)) {
        return reject("control body is not confined to this device");
    }

    if (request.kind == ControlKind::Loop) {
        if (!request.loopKind) {
            return reject("loop kind is missing");
        }
        if (*request.loopKind == LoopKind::FixedCount) {
            return CapabilityDecision::accept();
        }
        if (!request.condition ||
            !fpga::isRp1EvaluableCondition(*request.condition)) {
            return reject("loop predicate is not representable by RP1");
        }
        if (!request.predicateAvailableOnCandidate) {
            return reject("loop predicate is not produced by the FPGA body");
        }
        return CapabilityDecision::accept();
    }

    if (!request.condition ||
        !fpga::isRp1EvaluableCondition(*request.condition)) {
        return reject("conditional predicate is not representable by RP1");
    }
    if (request.childHasDataBoundaries) {
        return reject("conditional branches contain data-carrying boundaries");
    }
    if (!request.predicateAvailableOnCandidate) {
        return reject("conditional predicate is not available on the FPGA queue");
    }
    return CapabilityDecision::accept();
}

/*
 * Allocate rendezvous and scalar slots under one lock so the lease is atomic.
 * On exhaustion, roll back every slot acquired in this attempt; on success the
 * returned lease owns release, except when device poisoning freezes resources.
 */
std::unique_ptr<IDeviceResourceLease>
FpgaDevice::leaseResources(
    const std::vector<RendezvousId>& rendezvous,
    const std::vector<ScalarResourceId>& scalars) {
    requireExecutionUsable(
        "FpgaDevice::leaseResources");
    std::map<RendezvousId, BackendResourceId> rendezvousSlots;
    std::map<ScalarResourceId, BackendScalarId> scalarSlots;
    std::vector<std::uint32_t> allocated;
    std::lock_guard<std::mutex> lock(scalarMutex_);
    try {
        for (RendezvousId id : rendezvous) {
            const std::uint32_t slot = scalarSlotAlloc_.alloc();
            rendezvousSlots.emplace(id, BackendResourceId(slot));
            allocated.push_back(slot);
        }
        for (ScalarResourceId id : scalars) {
            const std::uint32_t slot = scalarSlotAlloc_.alloc();
            scalarSlots.emplace(id, BackendScalarId(slot));
            allocated.push_back(slot);
        }
    } catch (...) {
        for (std::uint32_t slot : allocated) {
            scalarSlotAlloc_.release(slot);
        }
        throw;
    }
    return std::make_unique<FpgaRendezvousLease>(
        *this, std::move(rendezvousSlots),
        std::move(scalarSlots));
}

std::shared_ptr<IDeviceResourceAccess>
FpgaDevice::resourceAccess() const {
    return std::make_shared<FpgaResourceAccess>(window_);
}

std::optional<std::string>
FpgaDevice::resolveMemoryRegion(const KernelDescriptor& kernel,
                                const std::string& portName) const {
    auto region = resolveBufferRegion(kernel, portName);
    if (!region) return std::nullopt;
    return memoryRegionTag(*region);
}

/*
 * Distinct HBM/DDR regions cannot alias one physical buffer.  Materialize the
 * source through host memory, then stage the target through its normal region-
 * aware path; get/set perform the required QDMA synchronization on each side.
 */
std::function<void()> FpgaDevice::makeDeviceCopyAction(
    const GraphBuffer& source, const GraphBuffer& target, BufferType /*type*/,
    const std::string& sourceRegion, const std::string& targetRegion) {
    std::shared_ptr<FpgaDevice> self = sharedSelf();
    const std::string sourceKey = scopedBufferKey(source.scopeId(), source.name());
    const std::string targetKey = scopedBufferKey(target.scopeId(), target.name());
    return [self = std::move(self), sourceKey, targetKey,
            sourceRegion, targetRegion]() {
        const std::size_t bytes = self->bufferSize(sourceKey);
        std::vector<std::uint8_t> staging(bytes);
        if (bytes > 0) {
            self->getOutputBuffer(
                sourceKey, staging.data(), staging.size());
        }
        self->setInputBuffer(
            targetKey,
            staging.empty() ? nullptr : staging.data(),
            staging.size());
        if (std::getenv("VRT_FPGA_BUFFER_TRACE")) {
            std::cerr << "[FpgaDevice] cross-region copy '" << sourceKey
                      << "' -> '" << targetKey << "' "
                      << sourceRegion << " -> " << targetRegion
                      << " (" << bytes << " bytes, host/QDMA fallback)" << std::endl;
        }
    };
}

std::string FpgaDevice::normalizeBufferKey(const std::string& bufferName) {
    if (bufferName.rfind("scope:", 0) == 0) return bufferName;
    return scopedBufferKey(0, bufferName);
}

/*
 * Follow alias indirections to the sole owning BufferRecord.  Detect cycles
 * defensively because allocation, resizing, and region propagation all assume
 * canonicalization terminates while bufferMutex_ is held.
 */
std::string FpgaDevice::canonicalBufferKey(const std::string& key) const {
    std::string current = key;
    std::set<std::string> seen;
    while (true) {
        if (!seen.insert(current).second) {
            throw std::runtime_error(
                "FpgaDevice: cycle detected in buffer aliases for '" + key + "'");
        }
        auto it = bufferAliases_.find(current);
        if (it == bufferAliases_.end()) return current;
        current = it->second;
    }
}

/*
 * Alias only already-backed compatible regions.  Store an indirection rather
 * than copying BufferRecord so growth, QDMA backing, and logical size remain
 * shared by loop boundaries and RW output names.
 */
void FpgaDevice::aliasBufferKey(const std::string& targetName,
                                const std::string& sourceName) {
    const std::string target = normalizeBufferKey(targetName);
    const std::string source = normalizeBufferKey(sourceName);
    if (target == source) return;
    std::lock_guard<std::mutex> lk(bufferMutex_);
    const std::string canonicalSource = canonicalBufferKey(source);
    const std::string canonicalTarget = canonicalBufferKey(target);
    if (canonicalTarget == canonicalSource) return;
    if (std::getenv("VRT_FPGA_BUFFER_TRACE")) {
        std::cerr << "[fpga-buffer] alias " << target
                  << " (" << canonicalTarget << ") <- " << source
                  << " (" << canonicalSource << ")" << std::endl;
    }
    auto it = buffers_.find(canonicalSource);
    if (it == buffers_.end()) {
        throw std::runtime_error(
            "FpgaDevice: cannot alias buffer '" + targetName + "' to unallocated source '" +
            sourceName + "' (carried-buffer boundary expects the source staged first)");
    }

    /*
     * Region identity is part of alias compatibility: HBM banks and DDR are
     * not interchangeable even if both expose a physical address.
     */
    auto sourceRegion = bufferRegion_.find(canonicalSource);
    auto targetRegion = bufferRegion_.find(canonicalTarget);
    if (sourceRegion != bufferRegion_.end() &&
        targetRegion != bufferRegion_.end() &&
        (sourceRegion->second.type != targetRegion->second.type ||
         sourceRegion->second.hbmPort != targetRegion->second.hbmPort)) {
        throw std::logic_error(
            "FpgaDevice: cannot alias buffers '" + targetName + "' and '" +
            sourceName + "' across incompatible memory regions");
    }

    /*
     * Propagate whichever endpoint knows the region.  Control boundaries can
     * expose metadata first on either the parent or body-local token.
     */
    bufferAliases_[canonicalTarget] = canonicalSource;
    if (sourceRegion != bufferRegion_.end()) {
        bufferRegion_[canonicalTarget] = sourceRegion->second;
    } else if (targetRegion != bufferRegion_.end()) {
        bufferRegion_[canonicalSource] = targetRegion->second;
    }
}

/*
 * Reuse capacity only when storage mode still matches current metadata.
 * Otherwise allocate fixed-size HBM/DDR memory in the kernel-visible region,
 * or carve an aligned BAR range for mock/unmapped operation.  Caller holds
 * bufferMutex_ so alias and arena state change together.
 */
FpgaDevice::BufferRecord FpgaDevice::ensureBufferByKey(const std::string& key,
                                                       BufferType type,
                                                       std::size_t sizeBytes,
                                                       const std::shared_ptr<::vrt::Device>&
                                                           stagingDevice) {
    const std::string canonicalKey = canonicalBufferKey(key);
    auto requestedRegion = bufferRegion_.find(key);
    if (requestedRegion != bufferRegion_.end()) {
        bufferRegion_[canonicalKey] = requestedRegion->second;
    }
    auto regionIt = bufferRegion_.find(canonicalKey);
    const bool deviceMode = (stagingDevice != nullptr) &&
                            (regionIt != bufferRegion_.end());

    auto it = buffers_.find(canonicalKey);
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
        /*
         * VRT buffers are fixed-size.  Keep a one-byte allocation for logical
         * zero length and replace it only when later launches outgrow capacity.
         */
        const std::size_t allocBytes = std::max<std::size_t>(sizeBytes, 1u);
        rec.mem = std::make_shared<::vrt::Buffer<std::uint8_t>>(
            *stagingDevice, allocBytes, regionIt->second);
        rec.capacity = allocBytes;
    } else {
        const std::uint32_t alignedOffset = alignUp(nextBufferOffset_, 64u);
        const std::uint64_t end = static_cast<std::uint64_t>(alignedOffset) +
                                  static_cast<std::uint64_t>(sizeBytes);
        if (end > fpga::Rp1BarWindow::kWindowSize) {
            throw std::out_of_range(
                "FpgaDevice: BAR-backed buffer arena exhausted while allocating '" +
                canonicalKey + "' (" + std::to_string(sizeBytes) + " bytes)");
        }
        rec.offset = alignedOffset;
        nextBufferOffset_ = static_cast<std::uint32_t>(end);
    }

    buffers_[canonicalKey] = rec;
    return rec;
}

FpgaDevice::BufferRecord FpgaDevice::ensureBuffer(const GraphBuffer& buffer,
                                                  std::size_t sizeBytes) {
    const std::string key = scopedBufferKey(buffer.scopeId(), buffer.name());
    std::shared_ptr<::vrt::Device> stagingDevice;
    {
        std::lock_guard<std::mutex> lk(pdiMutex_);
        stagingDevice = pdiStagingDevice_;
    }
    std::lock_guard<std::mutex> lk(bufferMutex_);
    return ensureBufferByKey(
        key, buffer.type(), sizeBytes, stagingDevice);
}

std::uint64_t FpgaDevice::bufferDeviceAddress(
    const GraphBuffer& buffer, std::size_t sizeBytes,
    std::shared_ptr<::vrt::Buffer<std::uint8_t>>& pin) {
    const BufferRecord rec = ensureBuffer(buffer, sizeBytes);
    if (rec.mem) {
        pin = rec.mem;
        return rec.mem->getPhysAddr();
    }
    pin.reset();
    return RP1_CTRL_PHYS_ADDR + static_cast<std::uint64_t>(rec.offset);
}

/*
 * Discover every kernel port's reachable HBM/DDR region before any bridge can
 * allocate its token.  Boundary aliases propagate that region across control
 * scopes; conflicting producers/consumers reject instead of creating a buffer
 * that one kernel cannot address.
 */
void FpgaDevice::populateBufferRegions(const Rp1QueueProgram& dg) {
    std::map<std::string, ::vrt::MemoryConfig> resolvedRegions;
    std::vector<std::pair<std::string, std::string>> boundaryAliases;
    auto record = [&](const KernelDescriptor& kernel, const std::string& portName,
                      const GraphBuffer& buffer) {
        auto region = resolveBufferRegion(kernel, portName);
        if (!region) return;
        const std::string key = scopedBufferKey(buffer.scopeId(), buffer.name());
        if (std::getenv("VRT_FPGA_PORT_TRACE")) {
            std::cerr << "[FpgaDevice] port-region kernel='" << kernel.name
                      << "' port='" << portName << "' buffer='" << key
                      << "' region=" << memoryRegionTag(*region) << std::endl;
        }
        auto [it, inserted] = resolvedRegions.emplace(key, *region);
        if (!inserted &&
            (it->second.type != region->type ||
             it->second.hbmPort != region->hbmPort)) {
            throw std::logic_error(
                "FpgaDevice: buffer '" + key +
                "' is bound to incompatible memory regions; kernel '" +
                kernel.name + "' port '" + portName +
                "' must use the same region as every other producer and consumer");
        }
    };

    /*
     * Walk top-level and attached child programs, collecting direct port
     * constraints and boundary alias edges.  Commands without buffers do not
     * participate in placement.
     */
    std::function<void(const Rp1QueueProgram&)> walk = [&](const Rp1QueueProgram& g) {
        for (const Rp1Command& node : g.commands) {
            if (const auto* boundary = std::get_if<Rp1BoundaryCommand>(&node)) {
                for (const Rp1BufferBoundaryCopy& copy : boundary->bufferCopies) {
                    boundaryAliases.emplace_back(
                        scopedBufferKey(copy.sourceScopeId, copy.sourceName),
                        scopedBufferKey(copy.targetScopeId, copy.targetName));
                }
                continue;
            }
            const auto* k = std::get_if<Rp1KernelCommand>(&node);
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
                for (const detail::PortBindings::InoutBinding& binding : k->ioMap.inouts()) {
                    if (binding.inPort == port.in.name && binding.outPort == port.out.name) {
                        record(k->kernel, port.in.name, binding.in);
                        record(k->kernel, port.out.name, binding.out);
                    }
                }
            }
        }
        for (const Rp1ChildProgram& child : g.children) {
            for (const auto& slice : child.programs) {
                if (slice) walk(*slice);
            }
        }
    };
    walk(dg);

    /*
     * Propagate known regions to a fixed point over boundary aliases.  If both
     * endpoints are known they must identify the same memory type and HBM bank.
     */
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [source, target] : boundaryAliases) {
            auto sourceIt = resolvedRegions.find(source);
            auto targetIt = resolvedRegions.find(target);
            if (sourceIt != resolvedRegions.end() &&
                targetIt != resolvedRegions.end()) {
                if (sourceIt->second.type != targetIt->second.type ||
                    sourceIt->second.hbmPort != targetIt->second.hbmPort) {
                    throw std::logic_error(
                        "FpgaDevice: boundary alias '" + source + "' -> '" +
                        target + "' spans incompatible memory regions");
                }
            } else if (sourceIt != resolvedRegions.end()) {
                resolvedRegions[target] = sourceIt->second;
                changed = true;
            } else if (targetIt != resolvedRegions.end()) {
                resolvedRegions[source] = targetIt->second;
                changed = true;
            }
        }
    }

    /*
     * Publish the complete map atomically with respect to allocation.  This
     * ordering prevents a bridge from choosing BAR storage between discovery
     * and alias-region propagation.
     */
    std::lock_guard<std::mutex> lk(bufferMutex_);
    for (const auto& [key, region] : resolvedRegions) {
        bufferRegion_[key] = region;
    }
}

/*
 * Resolve against explicit kernel image metadata when present, otherwise the
 * currently reconciled image.  The lookup path supports tests/bring-up but must
 * return a non-zero R5 base; explicit image guards are enforced by firmware.
 */
FpgaKernelLocation FpgaDevice::resolveKernelLocation(const KernelDescriptor& kernel) const {
    if (vbinSpec_) {
        const std::string active = activeImageId();
        // Explicit metadata describes the image selected by the graph's
        // reprogram gates. The current host-side image is only the fallback
        // for implicit descriptors; firmware enforces explicit image guards.
        const std::string imageId = kernel.image ? *kernel.image : active;
        if (imageId.empty()) {
            throw std::runtime_error(
                "FpgaDevice: no active image available for kernel '" + kernel.name + "'");
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

/*
 * Relate graph-facing descriptor names to system_map argument names.  Match by
 * ABI category and declaration order because descriptors may rename ports and
 * system_map access flags describe host register access, not buffer data flow.
 */
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
    // and split scalar vs buffer primarily by arg type. A readable-only pointer
    // with no m_axi port is ambiguous: HLS uses that shape for AXI-Lite scalar
    // outputs, while old metadata fixtures may omit the port on output buffers.
    // The descriptor's scalar count tells us how many such pointers to promote.
    auto isBufferArg = [](const fpga::FpgaKernelArgSpec& arg) {
        std::string t = arg.type;
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return t == "buffer" || t.find('*') != std::string::npos;
    };

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

    std::size_t specScalarCount = 0;
    for (const fpga::FpgaKernelArgSpec& arg : kit->second.args) {
        if (!isBufferArg(arg)) ++specScalarCount;
    }
    std::size_t pointerOutputsToPromote =
        descScalars.size() > specScalarCount
            ? descScalars.size() - specScalarCount
            : 0u;

    std::vector<std::string> specScalars;
    std::vector<std::string> specBuffers;
    for (const fpga::FpgaKernelArgSpec& arg : kit->second.args) {
        bool buffer = isBufferArg(arg);
        if (buffer && pointerOutputsToPromote > 0 &&
            arg.readable && !arg.writable && arg.port.empty()) {
            buffer = false;
            --pointerOutputsToPromote;
        }
        (buffer ? specBuffers : specScalars).push_back(arg.name);
    }

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
    if (vbinSpec_) {
        throw std::runtime_error(
            "FpgaDevice: output scalar port '" + portName + "' on kernel '" +
            kernel.name + "' has no s_axilite register offset in the system_map");
    }
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
            if (std::getenv("VRT_FPGA_PORT_TRACE")) {
                std::cerr << "[FpgaDevice] arg-offset kernel='" << kernel.name
                          << "' graph-port='" << descPort << "' hls-arg='"
                          << argName << "' offset=0x" << std::hex << a->second
                          << std::dec << std::endl;
            }
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

/*
 * Prefer cached DDR/QDMA staging, whose physical allocation is pinned for the
 * launch.  Without a staging device, fall back to the RP1 BAR arena and return
 * its control physical address; release pdiMutex_ before taking bufferMutex_ to
 * preserve the global lock order.
 */
std::uint64_t FpgaDevice::stagePdiBytes(
    const std::string& cacheKey,
    const std::vector<std::uint8_t>& bytes,
    std::shared_ptr<::vrt::Buffer<std::uint8_t>>& pin) {
    std::unique_lock<std::mutex> lk(pdiMutex_);
    if (pdiStagingDevice_) {
        auto cached = stagedPdis_.find(cacheKey);
        if (cached != stagedPdis_.end()) {
            std::cerr << "[FpgaDevice] stagePdiBytes: reusing DDR/QDMA staged PDI '"
                      << cacheKey << "' (" << cached->second.size
                      << " bytes) @ 0x" << std::hex << cached->second.physAddr
                      << std::dec << std::endl;
            pin = cached->second.buffer;
            return cached->second.physAddr;
        }

        std::cerr << "[FpgaDevice] stagePdiBytes: allocating DDR/QDMA buffer for '"
                  << cacheKey << "' (" << bytes.size() << " bytes)" << std::endl;
        auto buffer = std::make_shared<::vrt::Buffer<std::uint8_t>>(
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
        pin = stagedPdis_.at(cacheKey).buffer;
        return phys;
    }

    /*
     * BAR staging is the metadata-free fallback.  It has no external DMA object
     * to pin, but the device-owned arena record keeps its address stable.
     */
    lk.unlock();
    pin.reset();

    const std::string name = "__pdi_" + std::to_string(std::hash<std::string>{}(cacheKey));
    GraphBuffer token = ::vrt::graph::detail::makeGraphBuffer(BufferType::U8, name, 0);
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

/*
 * File-backed reprogram commands read a stable byte snapshot first, then share
 * the same cache, QDMA, pinning, and BAR fallback path as embedded vbin PDIs.
 */
std::uint64_t FpgaDevice::stagePdiFile(
    const std::string& pdiPath,
    std::shared_ptr<::vrt::Buffer<std::uint8_t>>& pin) {
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

    return stagePdiBytes(pdiPath, bytes, pin);
}

void FpgaDevice::setActiveImage(std::string imageId) {
    if (vbinSpec_ && !vbinSpec_->hasImage(imageId)) {
        throw std::runtime_error("FpgaDevice: cannot activate unknown image '" + imageId + "'");
    }
    std::lock_guard<std::mutex> lk(imageMutex_);
    activeImageId_ = std::move(imageId);
}

void FpgaDevice::markActiveImageUnknown() {
    std::lock_guard<std::mutex> lk(imageMutex_);
    activeImageId_.clear();
}

std::string FpgaDevice::activeImageId() const {
    std::lock_guard<std::mutex> lk(imageMutex_);
    return activeImageId_;
}

std::shared_ptr<FpgaDevice> FpgaDevice::sharedSelf() {
    try {
        return shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        throw std::runtime_error(
            "FpgaDevice: executable plans require the device to be "
            "owned by std::shared_ptr");
    }
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

/*
 * Resolve aliases before allocation, preserving an existing token type.
 * Device-backed buffers copy through the host mapping then sync to HBM/DDR;
 * BAR-backed buffers write or zero the RP1 window directly.
 */
void FpgaDevice::setInputBuffer(const std::string& bufferName,
                                const void*        data,
                                std::size_t        sizeBytes) {
    requireExecutionUsable(
        "FpgaDevice::setInputBuffer");
    const std::string key = normalizeBufferKey(bufferName);
    std::shared_ptr<::vrt::Device> stagingDevice;
    {
        std::lock_guard<std::mutex> lk(pdiMutex_);
        stagingDevice = pdiStagingDevice_;
    }
    std::lock_guard<std::mutex> lk(bufferMutex_);
    const std::string canonicalKey = canonicalBufferKey(key);

    BufferType type = BufferType::U8;
    if (auto existing = buffers_.find(canonicalKey); existing != buffers_.end()) {
        type = existing->second.type;
    }
    const BufferRecord rec = ensureBufferByKey(
        key, type, sizeBytes, stagingDevice);
    if (std::getenv("VRT_FPGA_BUFFER_TRACE")) {
        std::cerr << "[fpga-buffer] set " << key
                  << " canonical=" << canonicalKey
                  << " size=" << sizeBytes
                  << " device=" << static_cast<bool>(rec.mem)
                  << std::endl;
    }

    if (sizeBytes == 0) return;
    if (rec.mem) {
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

/*
 * Read from the canonical backing and reject requests beyond the logical size.
 * HBM/DDR results must synchronize device-to-host before copying; BAR results
 * are already visible through the RP1 window.
 */
void FpgaDevice::getOutputBuffer(const std::string& bufferName,
                                 void*              data,
                                 std::size_t        sizeBytes) const {
    if (sizeBytes == 0) return;
    if (!data) {
        throw std::invalid_argument("FpgaDevice::getOutputBuffer: data must not be null");
    }

    const std::string key = normalizeBufferKey(bufferName);
    std::lock_guard<std::mutex> lk(bufferMutex_);
    const std::string canonicalKey = canonicalBufferKey(key);
    auto it = buffers_.find(canonicalKey);
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
        it->second.mem->sync(::vrt::SyncType::DEVICE_TO_HOST);
        std::memcpy(data, it->second.mem->get(), sizeBytes);
    } else {
        window_->readAt(it->second.offset, data, sizeBytes);
    }
}

std::size_t FpgaDevice::bufferSize(const std::string& bufferName) const {
    const std::string key = normalizeBufferKey(bufferName);
    std::lock_guard<std::mutex> lk(bufferMutex_);
    const std::string canonicalKey = canonicalBufferKey(key);
    auto it = buffers_.find(canonicalKey);
    return (it == buffers_.end()) ? 0 : it->second.size;
}

bool FpgaDevice::hasBuffer(const std::string& bufferName) const {
    const std::string key = normalizeBufferKey(bufferName);
    std::lock_guard<std::mutex> lk(bufferMutex_);
    const std::string canonicalKey = canonicalBufferKey(key);
    const bool found = buffers_.find(canonicalKey) != buffers_.end();
    if (std::getenv("VRT_FPGA_BUFFER_TRACE")) {
        std::cerr << "[fpga-buffer] has " << key
                  << " canonical=" << canonicalKey
                  << " -> " << found << std::endl;
    }
    return found;
}

/*
 * The scheduled path builds a typed program first and receives compiler-owned
 * execution/resource leases externally.  Child controls remain attachable
 * until finalize(), so only sentinel configuration is frozen here.
 */
std::unique_ptr<IBackendExecutable> FpgaDevice::lowerQueue(
    const BackendLoweringContext& context) {
    requireExecutionUsable(
        "FpgaDevice::lowerQueue");
    sentinelConfigLocked_ = true;
    return std::make_unique<FpgaDevicePlan>(
        sharedSelf(), context);
}

/*
 * The direct-program path acquires exclusive execution itself, discovers
 * regions before bridge allocation, validates top-level variants, then builds
 * an immediately usable image.  The lease moves into the plan on success.
 */
std::unique_ptr<IBackendExecutable> FpgaDevice::compileProgram(
    const Rp1QueueProgram& dg) {
    sentinelConfigLocked_ = true;
    std::shared_ptr<FpgaDevice> self = sharedSelf();
    std::unique_ptr<IDeviceExecutionLease> executionLease =
        leaseExecution();
    if (!executionLease) {
        throw std::runtime_error(
            "FpgaDevice::compileProgram: device already has a live execution");
    }

    /*
     * Region discovery precedes validation and plan construction because a
     * bridge consumer may allocate before this queue's arguments are packed.
     */
    populateBufferRegions(dg);

    /*
     * Validate only direct-program constraints here.  The plan handles kernel,
     * reprogram, control, signal, and wait variants; a top-level boundary has
     * no enclosing control scope in which its publication could be resolved.
     */
    for (const Rp1Command& node : dg.commands) {
        std::visit(
            [&](const auto& concrete) {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, Rp1KernelCommand>) {
                    if (concrete.kernel.type != DeviceType::FPGA) {
                        throw std::logic_error(
                            std::string("FpgaDevice: kernel '") +
                            concrete.kernel.name +
                            "' has DeviceType::" + deviceTypeName(concrete.kernel.type) +
                            "; expected FPGA");
                    }
                } else if constexpr (std::is_same_v<T, Rp1BoundaryCommand>) {
                    throw std::logic_error(
                        std::string("FpgaDevice: top-level graph-region boundaries are not "
                                    "yet supported, got '") + concrete.id + "'");
                }
            },
            node);
    }

    if (dg.commands.empty()) {
        throw std::logic_error("FpgaDevice: Rp1QueueProgram has no nodes to compile");
    }
    return std::make_unique<FpgaDevicePlan>(std::move(self),
                                            dg,
                                            dg.scalarValues,
                                            sentinelSlot_,
                                            sentinelValue_,
                                            waitTimeout_,
                                            std::move(executionLease));

}

/*
 * Projection uses the same region discovery and image builder as execution but
 * intentionally holds no execution lease and performs no submission.  It still
 * freezes sentinel configuration because packet/barrier layout embeds it.
 */
fpga::Rp1GraphImage FpgaDevice::projectProgram(
    const Rp1QueueProgram& program) {
    requireExecutionUsable(
        "FpgaDevice::projectProgram");
    sentinelConfigLocked_ = true;
    std::shared_ptr<FpgaDevice> self = sharedSelf();
    populateBufferRegions(program);
    FpgaDevicePlan plan(
        std::move(self), program, program.scalarValues,
        sentinelSlot_, sentinelValue_, waitTimeout_, nullptr);
    return plan.image();
}

}  // namespace vrt::graph
