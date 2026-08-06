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
 * @file cpu_device.cpp
 * @brief CpuDevice implementation — naive single-core CPU device.
 */

#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/backend_runtime.hpp>
#include <vrt/graph/device/cpu/cpu_lowering.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace vrt::graph {

namespace {

constexpr std::chrono::seconds kBridgeWaitTimeout{35};

template <typename T>
T bitsAs(uint64_t bits) {
    T value{};
    std::memcpy(&value, &bits, sizeof(T));
    return value;
}

int64_t signedScalarValue(ScalarType type, uint64_t bits) {
    switch (type) {
        case ScalarType::I8:  return bitsAs<int8_t>(bits);
        case ScalarType::I16: return bitsAs<int16_t>(bits);
        case ScalarType::I32: return bitsAs<int32_t>(bits);
        case ScalarType::I64: return bitsAs<int64_t>(bits);
        default:
            throw std::invalid_argument("CpuDevice: scalar type is not signed integer");
    }
}

uint64_t unsignedScalarValue(ScalarType type, uint64_t bits) {
    switch (type) {
        case ScalarType::U8:  return bitsAs<uint8_t>(bits);
        case ScalarType::U16: return bitsAs<uint16_t>(bits);
        case ScalarType::U32: return bitsAs<uint32_t>(bits);
        case ScalarType::U64: return bitsAs<uint64_t>(bits);
        default:
            throw std::invalid_argument("CpuDevice: scalar type is not unsigned integer");
    }
}

long double floatingScalarValue(ScalarType type, uint64_t bits) {
    switch (type) {
        case ScalarType::F32: return bitsAs<float>(bits);
        case ScalarType::F64: return bitsAs<double>(bits);
        default:
            throw std::invalid_argument("CpuDevice: scalar type is not floating point");
    }
}

template <typename T>
bool compareValues(CompareOp op, T lhs, T rhs) {
    switch (op) {
        case CompareOp::LT: return lhs < rhs;
        case CompareOp::LE: return lhs <= rhs;
        case CompareOp::EQ: return lhs == rhs;
        case CompareOp::GT: return lhs > rhs;
        case CompareOp::GE: return lhs >= rhs;
        case CompareOp::NE: return lhs != rhs;
        default:
            throw std::runtime_error("CpuDevice: unsupported comparison operator");
    }
}

}  // namespace

/*
 * CpuDevicePlan owns one immutable queue program and its worker thread.
 * Shared device/runtime pins outlive the worker; child executable handles are
 * non-owning references into the same Execution's executable vector and are
 * valid until this plan has joined.
 */
class CpuDevicePlan : public IBackendExecutable {
    using NodeKind = CpuProgramNodeKind;
    using NodeRuntime = CpuProgramNode;

   public:
    CpuDevicePlan(
        std::shared_ptr<CpuDevice> device,
        const BackendLoweringContext& context)
        : device_(std::move(device)),
          runtimeState_(context.runtimeState),
          scalarValues_(
              runtimeState_ ? runtimeState_->scalarValues()
                            : nullptr),
          queue_(context.queue.id),
          runtime_(CpuLowering::lower(context).nodes) {
        if (!device_ || !runtimeState_) {
            throw std::invalid_argument(
                "CpuDevicePlan: device and runtime state must not be null");
        }
    }

    ~CpuDevicePlan() override {
        /*
         * Destruction must retire the worker even during exception unwinding;
         * the public Execution boundary is responsible for reporting errors.
         */
        try {
            wait();
        } catch (...) {
        }
    }

    QueueId queue() const override { return queue_; }
    DeviceId device() const override { return DeviceId(device_->id()); }

    void connectControlChildren(
        ControlExecutableHandle control,
        ControlChildRole role,
        std::vector<QueueExecutableHandle> children) override {
        childExecutables_[control.step][role] = std::move(children);
    }

    void finalize() override {
        for (const auto& [step, roles] : childExecutables_) {
            (void)step;
            for (const auto& [role, children] : roles) {
                (void)role;
                for (QueueExecutableHandle child : children) {
                    child.executable->finalize();
                }
            }
        }
    }

    /*
     * Re-launch first joins the previous run, then starts a fresh worker with
     * a cleared exception slot. Execution pins this plan until wait() joins
     * the thread, so capturing this is safe for the worker lifetime.
     */
    void launch() override {
        wait();
        workerException_ = nullptr;
        worker_ = std::thread([this] {
            detail::BackendWorkerScope workerScope;
            try {
                runOnce();
            } catch (...) {
                workerException_ = std::current_exception();
            }
        });
    }

    void wait() override {
        if (worker_.joinable()) worker_.join();
        if (workerException_) {
            std::exception_ptr ex = workerException_;
            workerException_ = nullptr;
            std::rethrow_exception(ex);
        }
    }

   private:
    /*
     * Scheduler invariants:
     * - initialUnmet counts queue-local predecessors not yet completed;
     * - ordinary nodes enter the FIFO as soon as that count reaches zero;
     * - consumer and Wait nodes enter a non-blocking poll set instead;
     * - each node runs once and alone releases its successors;
     * - lack of progress reports every still-pending probe or Wait id.
     */
    void runOnce() {
        /* Rebuild dependency counts because one CpuProgram supports re-runs. */
        std::vector<size_t> unmetCounts;
        unmetCounts.reserve(runtime_.size());
        for (const auto& rt : runtime_) {
            unmetCounts.push_back(rt.initialUnmet);
        }

        std::deque<size_t> readyKP;
        std::vector<size_t> pendingCons;

        /*
         * Promotion separates work that can run now from runtime conditions.
         * Polling blocked work keeps the single CPU worker available to make
         * progress on independent producer and kernel nodes.
         */
        auto promote = [&](size_t idx) {
            // Consumer bridges and cross-queue WAITs block until a runtime
            // condition holds, so they are polled rather than run immediately.
            if (runtime_[idx].kind == NodeKind::ConsumerOp ||
                runtime_[idx].kind == NodeKind::Wait) {
                pendingCons.push_back(idx);
            } else {
                readyKP.push_back(idx);
            }
        };

        for (size_t i = 0; i < runtime_.size(); ++i) {
            if (unmetCounts[i] == 0) promote(i);
        }

        static const bool kTrace = std::getenv("VRT_CPU_TRACE") != nullptr;
        /*
         * Dispatch one ready node, then release successors whose final local
         * predecessor just completed. Wait nodes clear their consumed slot so
         * a later loop iteration cannot reuse an old edge.
         */
        auto runIndex = [&](size_t idx) {
            NodeRuntime& rt = runtime_[idx];
            if (kTrace) {
                std::cerr << "[cpu-trace] run kind=" << static_cast<int>(rt.kind)
                          << " id=" << rt.diagnosticId << std::endl;
            }
            switch (rt.kind) {
                case NodeKind::Kernel:
                    executeKernel(rt.kernel);
                    break;
                case NodeKind::ProducerOp:
                case NodeKind::ConsumerOp:
                    rt.action();
                    break;
                case NodeKind::Noop:
                    break;
                case NodeKind::Boundary:
                    executeBoundary(rt.boundary);
                    break;
                case NodeKind::Loop:
                    executeLoop(rt);
                    break;
                case NodeKind::Conditional:
                    executeConditional(rt);
                    break;
                case NodeKind::Signal:
                    executeSignal(rt);
                    break;
                case NodeKind::Wait:
                    // A rendezvous is edge-triggered at graph level. Consume
                    // the physical slot so a later loop iteration cannot
                    // observe the previous publication.
                    runtimeState_->access(rt.resourceOwner)
                        .writeRendezvous(rt.signalResource, 0u);
                    break;
            }
            for (size_t successor : rt.successors) {
                if (--unmetCounts[successor] == 0) promote(successor);
            }
        };

        auto consumerReady = [&](size_t idx) -> bool {
            NodeRuntime& rt = runtime_[idx];
            if (rt.kind == NodeKind::Wait) return waitSatisfied(rt);
            return rt.tryReady && rt.tryReady();
        };

        /*
         * Drain all immediately runnable work before round-robin polling.
         * Any completed node resets the watchdog; yielding avoids monopolizing
         * the host while all remaining progress depends on a peer device.
         */
        size_t rrCursor = 0;
        auto idleSince = std::chrono::steady_clock::now();
        for (;;) {
            while (!readyKP.empty()) {
                size_t idx = readyKP.front();
                readyKP.pop_front();
                runIndex(idx);
                idleSince = std::chrono::steady_clock::now();
            }
            if (pendingCons.empty()) break;

            bool fired = false;
            for (size_t step = 0; step < pendingCons.size(); ++step) {
                if (rrCursor >= pendingCons.size()) rrCursor = 0;
                size_t idx = pendingCons[rrCursor];
                if (consumerReady(idx)) {
                    pendingCons.erase(pendingCons.begin() +
                                      static_cast<std::ptrdiff_t>(rrCursor));
                    runIndex(idx);
                    fired = true;
                    idleSince = std::chrono::steady_clock::now();
                    break;
                }
                ++rrCursor;
            }
            if (!fired) {
                if (std::chrono::steady_clock::now() - idleSince > kBridgeWaitTimeout) {
                    std::string pending;
                    for (size_t idx : pendingCons) {
                        if (!pending.empty()) pending += ", ";
                        pending += runtime_[idx].diagnosticId;
                    }
                    throw std::runtime_error(
                        "CpuDevice: timed out waiting for bridge consumer(s): " + pending);
                }
                std::this_thread::yield();
            }
        }
    }

    uint64_t scalarBits(const std::string& name, uint64_t scopeId) const {
        std::lock_guard<std::mutex> lock(
            runtimeState_->scalarMutex());
        auto it = scalarValues_->find(scopedScalarKey(scopeId, name));
        if (it == scalarValues_->end() && scopeId == 0) {
            it = scalarValues_->find(name);
        }
        if (it == scalarValues_->end()) {
            throw std::runtime_error(
                "CpuDevice: scalar '" + name + "' is not set before control-flow evaluation");
        }
        return it->second;
    }

    uint64_t operandBits(const ConditionOperand& operand) const {
        if (operand.isConstant()) return operand.constantBits();
        return scalarBits(operand.name(), operand.scopeId());
    }

    uint64_t evaluateTripCount(const LoopTripCount& tripCount) const {
        const uint64_t bits = scalarBits(tripCount.name(), tripCount.scopeId());
        if (isSignedIntegerScalarType(tripCount.type())) {
            const int64_t value = signedScalarValue(tripCount.type(), bits);
            if (value < 0) {
                throw std::runtime_error("CpuDevice: loop trip count cannot be negative");
            }
            return static_cast<uint64_t>(value);
        }
        return unsignedScalarValue(tripCount.type(), bits);
    }

    /*
     * Conditions have four semantic cases:
     * - AlwaysTrue/AlwaysFalse need no operands;
     * - epsilon equality/inequality compares floating-point distance;
     * - ordinary floating-point comparisons preserve floating semantics;
     * - integer comparisons decode signedness before applying the operator.
     * Missing operands or incompatible scalar kinds are runtime errors.
     */
    bool evaluateCondition(const Condition& condition) const {
        switch (condition.op()) {
            case CompareOp::AlwaysTrue:
                return true;
            case CompareOp::AlwaysFalse:
                return false;
            default:
                break;
        }

        if (!condition.lhs() || !condition.rhs()) {
            throw std::runtime_error("CpuDevice: condition is missing comparison operands");
        }

        const ScalarType type = condition.lhs()->type();
        const uint64_t lhsBits = operandBits(*condition.lhs());
        const uint64_t rhsBits = operandBits(*condition.rhs());

        if (condition.isEpsilonCompare()) {
            if (!condition.epsilon()) {
                throw std::runtime_error("CpuDevice: epsilon comparison is missing epsilon");
            }
            const long double lhs = floatingScalarValue(type, lhsBits);
            const long double rhs = floatingScalarValue(type, rhsBits);
            const long double epsilon = floatingScalarValue(type, operandBits(*condition.epsilon()));
            const bool equal = std::fabs(lhs - rhs) <= epsilon;
            return condition.op() == CompareOp::EQE ? equal : !equal;
        }

        if (isFloatingScalarType(type)) {
            return compareValues(condition.op(), floatingScalarValue(type, lhsBits),
                                 floatingScalarValue(type, rhsBits));
        }
        if (isSignedIntegerScalarType(type)) {
            return compareValues(condition.op(), signedScalarValue(type, lhsBits),
                                 signedScalarValue(type, rhsBits));
        }
        return compareValues(condition.op(), unsignedScalarValue(type, lhsBits),
                             unsignedScalarValue(type, rhsBits));
    }

    /*
     * Child launch is all-or-join:
     * - prepare and launch in order until the first failure;
     * - wait every child whose launch returned, even after an error;
     * - retain the first exception and rethrow only after all joins.
     * This keeps sibling queues from surviving their parent control step.
     */
    void runChildExecutables(
        ScheduleStepId control, ControlChildRole role) {
        auto parent = childExecutables_.find(control);
        if (parent == childExecutables_.end()) {
            throw std::runtime_error(
                "CpuDevice: direct control step is missing child queues");
        }
        auto children = parent->second.find(role);
        if (children == parent->second.end()) {
            throw std::runtime_error(
                "CpuDevice: direct control step is missing child queues");
        }
        std::exception_ptr firstException;
        std::size_t launched = 0;
        for (QueueExecutableHandle child : children->second) {
            try {
                child.executable->prepareLaunch();
                child.executable->launch();
                ++launched;
            } catch (...) {
                firstException = std::current_exception();
                break;
            }
        }
        for (std::size_t i = 0; i < launched; ++i) {
            try {
                children->second[i].executable->wait();
            } catch (...) {
                if (!firstException) {
                    firstException = std::current_exception();
                }
            }
        }
        if (firstException) std::rethrow_exception(firstException);
    }

    void runChildren(
        const NodeRuntime& control, ControlChildRole role) {
        runChildExecutables(control.step, role);
    }

    /*
     * A boundary publishes a value into a new scoped token:
     * - scalars copy raw bits under the shared scalar lock;
     * - buffers deep-copy storage under the device buffer lock.
     * Root-scope fallback accepts legacy unscoped producer keys, but every
     * destination is written under its canonical scoped key.
     */
    void executeBoundary(const CpuBoundaryCommand& boundary) {
        /* Publish scalar snapshots before taking the independent buffer lock. */
        {
            std::lock_guard<std::mutex> lock(
                runtimeState_->scalarMutex());
            for (const auto& copy : boundary.scalarCopies) {
                const std::string sourceKey = scopedScalarKey(
                    copy.source.scopeId(), copy.source.varName());
                auto sourceIt = scalarValues_->find(sourceKey);
                if (sourceIt == scalarValues_->end() &&
                    copy.source.scopeId() == 0) {
                    sourceIt = scalarValues_->find(
                        copy.source.varName());
                }
                if (sourceIt == scalarValues_->end()) {
                    throw std::runtime_error(
                        "CpuDevice: boundary scalar source '" +
                        copy.source.varName() +
                        "' was not set before boundary execution");
                }
                (*scalarValues_)[scopedScalarKey(
                    copy.target.scopeId(), copy.target.varName())] =
                        sourceIt->second;
            }
        }
        /* Deep copies keep child and parent buffer tokens from aliasing. */
        {
            std::lock_guard<std::mutex> lock(device_->bufferMutex_);
            for (const auto& copy : boundary.bufferCopies) {
                const std::string sourceKey = scopedBufferKey(
                    copy.source.scopeId(), copy.source.name());
                auto sourceIt = device_->buffers_.find(sourceKey);
                if (sourceIt == device_->buffers_.end() &&
                    copy.source.scopeId() == 0) {
                    sourceIt = device_->buffers_.find(copy.source.name());
                }
                if (sourceIt == device_->buffers_.end()) {
                    throw std::runtime_error(
                        "CpuDevice: boundary buffer source '" +
                        copy.source.name() +
                        "' was not produced before boundary execution");
                }
                device_->buffers_[scopedBufferKey(
                    copy.target.scopeId(), copy.target.name())] =
                        std::make_shared<std::vector<std::uint8_t>>(
                            *sourceIt->second);
            }
        }
    }

    /*
     * Control publication selects the first or second arm, then handles two
     * value kinds. Buffers are deep-copied; scalars publish raw bits.
     * Source, target, and this control queue must be co-located because no
     * bridge runs inside this helper. Missing values and kind mismatches are
     * compiler/runtime invariant failures.
     */
    void publishControlValue(
        const CpuControlPublication& publication, bool second) {
        const DeviceId& sourceDevice =
            second ? publication.secondDevice
                   : publication.firstDevice;
        if (!sourceDevice.empty() &&
            sourceDevice.value() != device_->id()) {
            throw std::runtime_error(
                "CpuDevice: cross-device control-flow output publication is not executable yet");
        }
        if (!publication.targetDevice.empty() &&
            publication.targetDevice.value() != device_->id()) {
            throw std::runtime_error(
                "CpuDevice: cross-device control-flow output publication is not executable yet");
        }
        const CpuControlValue& source =
            second ? publication.second : publication.first;
        if (const auto* target =
                std::get_if<GraphBuffer>(&publication.target)) {
            const auto* sourceBuffer =
                std::get_if<GraphBuffer>(&source);
            if (!sourceBuffer) {
                throw std::logic_error(
                    "CpuDevice: control publication kind mismatch");
            }
            const std::string sourceKey = scopedBufferKey(
                sourceBuffer->scopeId(), sourceBuffer->name());
            std::lock_guard<std::mutex> lock(device_->bufferMutex_);
            auto sourceIt = device_->buffers_.find(sourceKey);
            if (sourceIt == device_->buffers_.end() &&
                sourceBuffer->scopeId() == 0) {
                sourceIt = device_->buffers_.find(sourceBuffer->name());
            }
            if (sourceIt == device_->buffers_.end()) {
                throw std::runtime_error(
                    "CpuDevice: control-flow output buffer source '" +
                    sourceBuffer->name() + "' was not produced");
            }
            device_->buffers_[scopedBufferKey(
                target->scopeId(), target->name())] =
                    std::make_shared<std::vector<std::uint8_t>>(
                        *sourceIt->second);
            return;
        }
        const GraphScalar& target =
            std::get<GraphScalar>(publication.target);
        const auto* sourceScalar = std::get_if<GraphScalar>(&source);
        if (!sourceScalar) {
            throw std::logic_error(
                "CpuDevice: control publication kind mismatch");
        }
        std::lock_guard<std::mutex> lock(
            runtimeState_->scalarMutex());
        auto sourceIt = scalarValues_->find(
            scopedScalarKey(
                sourceScalar->scopeId(), sourceScalar->varName()));
        if (sourceIt == scalarValues_->end()) {
            throw std::runtime_error(
                "CpuDevice: control-flow output scalar source '" +
                sourceScalar->varName() + "' was not produced");
        }
        (*scalarValues_)[scopedScalarKey(
            target.scopeId(), target.varName())] = sourceIt->second;
    }

    void publishLoopOutputs(
        const CpuLoopCommand& loop, bool initialValue) {
        for (const CpuControlPublication& publication :
             loop.publications) {
            publishControlValue(publication, initialValue);
        }
    }

    void publishConditionalOutputs(
        const CpuConditionalCommand& condition, bool elseBranch) {
        for (const CpuControlPublication& publication :
             condition.publications) {
            publishControlValue(publication, elseBranch);
        }
    }

    /*
     * The split-loop handshake runs before each body and once more to stop:
     * - authority writes value (0 continue, 1 stop), then marks decision ready;
     * - follower consumes the decision and raises acknowledgement;
     * - authority waits for that acknowledgement and clears it before its body;
     * - a continue decision runs both bodies, while the final stop releases
     *   the follower without another iteration.
     * The round trip prevents either participant getting one decision ahead.
     */
    void executeLoopAuthority(const NodeRuntime& runtime) {
        const CpuLoopCommand& loop = runtime.loop;
        const IDeviceResourceAccess& access =
            runtimeState_->access(loop.resourceOwner);
        if (!loop.condition && loop.kind != LoopKind::FixedCount) {
            throw std::runtime_error("CpuDevice: split-loop Authority is missing its condition");
        }
        std::optional<uint64_t> fixedCount;
        if (loop.kind == LoopKind::FixedCount) {
            if (!loop.tripCount) {
                throw std::runtime_error(
                    "CpuDevice: split fixed-count loop is missing its trip count");
            }
            fixedCount = evaluateTripCount(*loop.tripCount);
        }
        bool completedIteration = false;
        uint64_t iteration = 0;
        const bool kTrace = std::getenv("VRT_CPU_TRACE") != nullptr;
        auto publishDecision = [&](bool cont) {
            access.writeRendezvous(loop.value, cont ? 0u : 1u);
            access.writeRendezvous(loop.decision, 1u);
            const auto deadline =
                std::chrono::steady_clock::now() + kBridgeWaitTimeout;
            while (access.readRendezvous(
                       loop.acknowledgement) == 0u) {
                if (std::chrono::steady_clock::now() > deadline) {
                    throw std::runtime_error(
                        "CpuDevice: split-loop Authority timed out awaiting Follower ack");
                }
                std::this_thread::yield();
            }
            access.writeRendezvous(loop.acknowledgement, 0u);
        };

        /* Fixed-count and conditional loops differ only in decision source. */
        bool cont = fixedCount ? *fixedCount != 0
                               : evaluateCondition(*loop.condition);
        for (;;) {
            publishDecision(cont);
            if (!cont) break;
            if (kTrace) std::cerr << "[cpu-trace] authority body iter=" << iteration
                                  << " begin" << std::endl;
            runChildren(runtime, ControlChildRole::LoopBody);
            if (kTrace) std::cerr << "[cpu-trace] authority body iter=" << iteration
                                  << " done" << std::endl;
            completedIteration = true;
            ++iteration;
            cont = fixedCount ? iteration < *fixedCount
                              : evaluateCondition(*loop.condition);
        }
        /*
         * Zero iterations publish the initial arm; otherwise the last
         * completed backedge is the loop result.
         */
        publishLoopOutputs(loop, !completedIteration);
    }

    /*
     * CPU authority uses the cross-device handshake above. Local controls run
     * either an evaluated fixed count or a top-tested while condition.
     * completedIteration selects initial versus backedge result publication,
     * including the zero-trip case.
     */
    void executeLoop(const NodeRuntime& runtime) {
        const CpuLoopCommand& loop = runtime.loop;
        if (loop.role == CpuControlRole::Authority) {
            executeLoopAuthority(runtime);
            return;
        }
        bool completedIteration = false;
        if (loop.kind == LoopKind::FixedCount) {
            if (!loop.tripCount) {
                throw std::runtime_error("CpuDevice: compiled fixed-count loop is missing trip count");
            }
            const uint64_t count = evaluateTripCount(*loop.tripCount);
            for (uint64_t i = 0; i < count; ++i) {
                runChildren(runtime, ControlChildRole::LoopBody);
                completedIteration = true;
            }
        } else {
            if (!loop.condition) {
                throw std::runtime_error("CpuDevice: compiled while loop is missing condition");
            }
            while (evaluateCondition(*loop.condition)) {
                runChildren(runtime, ControlChildRole::LoopBody);
                completedIteration = true;
            }
        }

        publishLoopOutputs(loop, !completedIteration);
    }

    /*
     * Evaluate once, run exactly one connected child role, then publish the
     * matching arm into each parent result token.
     */
    void executeConditional(const NodeRuntime& runtime) {
        const CpuConditionalCommand& condition = runtime.conditional;
        if (!condition.condition) {
            throw std::logic_error(
                "CpuDevice: conditional is missing its condition");
        }
        const bool thenBranch =
            evaluateCondition(*condition.condition);
        runChildren(
            runtime, thenBranch ? ControlChildRole::ConditionalThen
                                : ControlChildRole::ConditionalElse);
        publishConditionalOutputs(condition, !thenBranch);
    }

    // --- Cross-queue rendezvous (Phase E) ---
    //
    // A split cross-device loop runs this CPU slice concurrently with its peer
    // (FPGA) queue. The two queues rendezvous each iteration through signal
    // slots in the peer's host-visible BAR window; SIGNAL nodes SET/accumulate
    // a slot, WAIT nodes poll one until a comparison holds. RP1 executes the
    // mirror-image half autonomously on its side.

    void executeSignal(const NodeRuntime& rt) {
        const IDeviceResourceAccess& access =
            runtimeState_->access(rt.resourceOwner);
        const BackendResourceId resource = rt.signalResource;
        std::uint32_t next = rt.signalValue;
        switch (rt.signalOp) {
            case CpuSignalOperation::Set:
                next = rt.signalValue;
                break;
            case CpuSignalOperation::Add:
                next = access.readRendezvous(resource) + rt.signalValue;
                break;
            case CpuSignalOperation::Or:
                next = access.readRendezvous(resource) | rt.signalValue;
                break;
            case CpuSignalOperation::And:
                next = access.readRendezvous(resource) & rt.signalValue;
                break;
        }
        if (std::getenv("VRT_CPU_TRACE")) {
            std::cerr << "[cpu-trace] SIGNAL slot="
                      << rt.signalResource.value() << " <- " << next
                      << " (" << rt.diagnosticId << ")" << std::endl;
        }
        access.writeRendezvous(resource, next);
    }

    bool waitSatisfied(const NodeRuntime& rt) const {
        const std::uint32_t current =
            runtimeState_->access(rt.resourceOwner).readRendezvous(
                rt.signalResource);
        switch (rt.conditionOp) {
            case CpuWaitCondition::Equal:
                return current == rt.signalValue;
            case CpuWaitCondition::NotEqual:
                return current != rt.signalValue;
            case CpuWaitCondition::Less:
                return current < rt.signalValue;
            case CpuWaitCondition::GreaterEqual:
                return current >= rt.signalValue;
            case CpuWaitCondition::AndNonzero:
                return (current & rt.signalValue) != 0;
            case CpuWaitCondition::AndZero:
                return (current & rt.signalValue) == 0;
        }
        return false;
    }

    void executeKernel(const CpuKernelCommand& node);

    std::shared_ptr<CpuDevice> device_;
    std::shared_ptr<BackendRuntimeState> runtimeState_;
    std::shared_ptr<std::map<std::string, uint64_t>> scalarValues_;
    std::vector<NodeRuntime> runtime_;
    QueueId queue_;
    std::map<
        ScheduleStepId,
        std::map<ControlChildRole, std::vector<QueueExecutableHandle>>>
        childExecutables_;
    std::thread worker_;
    std::exception_ptr workerException_;
};

// ---------------------------------------------------------------------------
// CpuBufferView helpers
// ---------------------------------------------------------------------------

namespace {

size_t elementSize(BufferType t) {
    switch (t) {
        case BufferType::U8:  case BufferType::I8:  return 1;
        case BufferType::U16: case BufferType::I16: return 2;
        case BufferType::U32: case BufferType::I32: case BufferType::F32: return 4;
        case BufferType::U64: case BufferType::I64: case BufferType::F64: return 8;
    }
    return 1;
}

}  // namespace

size_t CpuBufferView::elementCount() const {
    size_t es = elementSize(elementType);
    return (es > 0) ? (sizeBytes / es) : 0;
}

// ---------------------------------------------------------------------------
// CpuDevice
// ---------------------------------------------------------------------------

CpuDevice::CpuDevice(std::string id) : id_(std::move(id)) {}

std::unique_ptr<IDeviceExecutionLease> CpuDevice::leaseExecution() {
    return tryAcquireExclusiveExecutionLease(executionLeased_);
}

void CpuDevice::registerKernel(std::shared_ptr<CpuKernel> kernel) {
    if (!kernel) {
        throw std::invalid_argument("CpuDevice::registerKernel: kernel must not be null");
    }
    std::string kernelName = kernel->name();
    if (kernelName.empty()) {
        throw std::invalid_argument("CpuDevice::registerKernel: kernel name must not be empty");
    }
    kernels_[std::move(kernelName)] = std::move(kernel);
}

// Buffer keys are always scoped ("scope:N:name"). The user-facing setters and
// getters accept a plain buffer name — the declared root-scope name — and
// internally normalize to the scope-0 storage key. Cross-device bridges that
// already pass a scoped key (e.g. "scope:1:state" for a buffer published from
// a loop body) continue to work because we leave keys with the "scope:"
// prefix untouched.
namespace {
inline std::string normalizeUserBufferKey(const std::string& bufferName) {
    if (bufferName.rfind("scope:", 0) == 0) return bufferName;
    return scopedBufferKey(0, bufferName);
}
}  // namespace

void CpuDevice::setInputBuffer(const std::string& bufferName,
                                const void*        data,
                                size_t             sizeBytes) {
    if (std::getenv("VRT_CPU_TRACE")) {
        std::cerr << "[cpu-trace] setInputBuffer '" << normalizeUserBufferKey(bufferName)
                  << "' size=" << sizeBytes << " data=" << (data != nullptr) << std::endl;
    }
    auto buffer =
        std::make_shared<std::vector<std::uint8_t>>(sizeBytes);
    if (data && sizeBytes > 0) {
        std::memcpy(buffer->data(), data, sizeBytes);
    }
    std::lock_guard<std::mutex> lock(bufferMutex_);
    buffers_[normalizeUserBufferKey(bufferName)] =
        std::move(buffer);
}

void CpuDevice::getOutputBuffer(const std::string& bufferName,
                                void*              data,
                                size_t             sizeBytes) const {
    if (sizeBytes != 0 && data == nullptr) {
        throw std::invalid_argument(
            "CpuDevice::getOutputBuffer: data must not be null");
    }
    /*
     * Pin the selected allocation, then release the map lock before copying.
     * A concurrent bridge or queue may replace the key, but shared ownership
     * keeps this snapshot and its data pointer valid through memcpy.
     */
    std::shared_ptr<const std::vector<std::uint8_t>> buffer;
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        auto it = buffers_.find(normalizeUserBufferKey(bufferName));
        if (it == buffers_.end()) {
            throw std::runtime_error(
                "CpuDevice::getOutputBuffer: unknown buffer '" +
                bufferName + "'");
        }
        if (sizeBytes > it->second->size()) {
            throw std::out_of_range(
                "CpuDevice::getOutputBuffer: requested " +
                std::to_string(sizeBytes) + " bytes but buffer '" +
                bufferName + "' holds " +
                std::to_string(it->second->size()));
        }
        buffer = it->second;
    }
    if (sizeBytes != 0) {
        std::memcpy(data, buffer->data(), sizeBytes);
    }
}

size_t CpuDevice::bufferSize(const std::string& bufferName) const {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    auto it = buffers_.find(normalizeUserBufferKey(bufferName));
    return (it == buffers_.end()) ? 0 : it->second->size();
}

bool CpuDevice::hasBuffer(const std::string& bufferName) const {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return buffers_.find(normalizeUserBufferKey(bufferName)) != buffers_.end();
}

std::unique_ptr<IBackendExecutable> CpuDevice::lowerQueue(
    const BackendLoweringContext& context) {
    try {
        return std::make_unique<CpuDevicePlan>(
            shared_from_this(), context);
    } catch (const std::bad_weak_ptr&) {
        throw std::runtime_error(
            "CpuDevice: executable plans require the device to be "
            "owned by std::shared_ptr");
    }
}

// --- CpuDevicePlan kernel execution helpers ---

/*
 * Kernel invocation has four phases:
 * - find the registered implementation and snapshot scalar inputs/sizes;
 * - resolve inputs, allocate outputs, clone inouts, and pin every buffer;
 * - call user code with no runtime or device mutex held;
 * - publish scalar outputs only after successful return.
 * Raw buffer views remain valid because their shared pins span phases 3-4.
 */
void CpuDevicePlan::executeKernel(const CpuKernelCommand& node) {
    const std::string& kname = node.kernel.name;
    auto kernelIt = device_->kernels_.find(kname);
    if (kernelIt == device_->kernels_.end()) {
        throw std::runtime_error(
            "CpuDevice: no kernel registered for '" + kname + "'");
    }
    const std::shared_ptr<CpuKernel> kernel = kernelIt->second;

    /*
     * Resolve every scalar-dependent size under the same lock as scalar
     * snapshots so one invocation cannot mix values from two publications.
     */
    std::map<std::string, uint64_t> scalars;
    std::map<std::string, uint64_t> outputScalars;
    std::map<std::string, std::size_t> inputSizes;
    std::map<std::string, std::size_t> outputSizes;
    std::map<std::string, std::size_t> inoutInputSizes;
    std::map<std::string, std::size_t> inoutOutputSizes;
    {
        std::lock_guard<std::mutex> scalarLock(
            runtimeState_->scalarMutex());
        for (const auto& [portName, gs] :
             node.ioMap.inputScalars()) {
            auto sit = scalarValues_->find(
                scopedScalarKey(gs.scopeId(), gs.varName()));
            if (sit == scalarValues_->end() && gs.scopeId() == 0) {
                sit = scalarValues_->find(gs.varName());
            }
            if (sit == scalarValues_->end()) {
                throw std::runtime_error(
                    "CpuDevice: scalar '" + gs.varName() +
                    "' not set before launch");
            }
            scalars[portName] = sit->second;
        }
        for (const auto& [portName, scalar] :
             node.ioMap.outputScalars()) {
            (void)scalar;
            outputScalars[portName] = 0;
        }
        for (const auto& [portName, buffer] :
             node.ioMap.inputs()) {
            if (buffer.hasSizeScalar()) {
                inputSizes[portName] = resolvedBufferSizeBytes(
                    buffer, scalarValues_, "CpuDevice");
            }
        }
        for (const auto& [portName, buffer] :
             node.ioMap.outputs()) {
            outputSizes[portName] = resolvedBufferSizeBytes(
                buffer, scalarValues_, "CpuDevice");
        }
        for (const auto& binding : node.ioMap.inouts()) {
            if (binding.in.hasSizeScalar()) {
                inoutInputSizes[binding.inPort] =
                    resolvedBufferSizeBytes(
                        binding.in, scalarValues_, "CpuDevice");
            }
            inoutOutputSizes[binding.outPort] =
                resolvedBufferSizeBytes(
                    binding.out, scalarValues_, "CpuDevice");
        }
    }

    /*
     * Form all raw views while the buffer map is stable. Input allocations
     * are retained, outputs are installed under canonical keys, and inouts
     * clone their input so input and output ports preserve value semantics.
     */
    std::map<std::string, CpuBufferView> bufferViews;
    /*
     * The map may later replace any key. These pins, rather than map presence,
     * are what guarantee every CpuBufferView through kernel->run().
     */
    std::vector<std::shared_ptr<std::vector<std::uint8_t>>>
        bufferPins;
    {
        std::lock_guard<std::mutex> bufferLock(
            device_->bufferMutex_);
        auto findStorage =
            [&](const GraphBuffer& buffer) {
                const std::string key = scopedBufferKey(
                    buffer.scopeId(), buffer.name());
                auto it = device_->buffers_.find(key);
                if (it == device_->buffers_.end() &&
                    buffer.scopeId() == 0) {
                    it = device_->buffers_.find(buffer.name());
                }
                if (it == device_->buffers_.end()) {
                    throw std::runtime_error(
                        "CpuDevice: buffer '" + buffer.name() +
                        "' not found; did you forget to call "
                        "setInputBuffer()?");
                }
                return it->second;
            };
        auto validateSize =
            [](const GraphBuffer& buffer,
               const std::shared_ptr<std::vector<std::uint8_t>>& storage,
               const std::map<std::string, std::size_t>& sizes,
               const std::string& port) {
                auto expected = sizes.find(port);
                if (expected != sizes.end() &&
                    storage->size() != expected->second) {
                    throw std::runtime_error(
                        "CpuDevice: buffer '" + buffer.name() +
                        "' holds " +
                        std::to_string(storage->size()) +
                        " byte(s), expected " +
                        std::to_string(expected->second));
                }
            };

        for (const auto& [portName, buffer] :
             node.ioMap.inputs()) {
            auto storage = findStorage(buffer);
            validateSize(
                buffer, storage, inputSizes, portName);
            bufferViews[portName] = {
                storage->data(), storage->size(), buffer.type()};
            bufferPins.push_back(std::move(storage));
        }
        for (const auto& [portName, buffer] :
             node.ioMap.outputs()) {
            auto storage =
                std::make_shared<std::vector<std::uint8_t>>(
                    outputSizes.at(portName));
            device_->buffers_[scopedBufferKey(
                buffer.scopeId(), buffer.name())] = storage;
            bufferViews[portName] = {
                storage->data(), storage->size(), buffer.type()};
            bufferPins.push_back(std::move(storage));
        }
        for (const auto& binding : node.ioMap.inouts()) {
            auto input = findStorage(binding.in);
            validateSize(
                binding.in, input, inoutInputSizes,
                binding.inPort);
            const std::size_t outputBytes =
                inoutOutputSizes.at(binding.outPort);
            if (outputBytes != input->size()) {
                throw std::runtime_error(
                    "CpuDevice: RW output buffer '" +
                    binding.out.name() +
                    "' size does not match input buffer '" +
                    binding.in.name() + "'");
            }
            auto output =
                std::make_shared<std::vector<std::uint8_t>>(
                    *input);
            device_->buffers_[scopedBufferKey(
                binding.out.scopeId(), binding.out.name())] =
                    output;
            bufferViews[binding.inPort] = {
                input->data(), input->size(), binding.in.type()};
            bufferViews[binding.outPort] = {
                output->data(), output->size(),
                binding.out.type()};
            bufferPins.push_back(std::move(input));
            bufferPins.push_back(std::move(output));
        }
    }

    /*
     * Output scalar addresses point into a map that is now structurally
     * stable. User code sees input snapshots and invocation-local outputs.
     */
    std::map<std::string, uint64_t*> writableScalars;
    for (auto& [portName, bits] : outputScalars) {
        writableScalars[portName] = &bits;
    }
    CpuKernelArgs args(
        std::move(bufferViews), std::move(scalars),
        std::move(writableScalars));
    kernel->run(args);

    /*
     * Publish all scalar results under one lock after user code succeeds.
     * A throwing kernel leaves the previous shared scalar snapshot intact.
     */
    {
        std::lock_guard<std::mutex> scalarLock(
            runtimeState_->scalarMutex());
        for (const auto& [portName, scalar] :
             node.ioMap.outputScalars()) {
            (*scalarValues_)[scopedScalarKey(
                scalar.scopeId(), scalar.varName())] =
                    outputScalars.at(portName);
        }
    }
}

}  // namespace vrt::graph
