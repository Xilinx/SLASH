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

#include <vrt/graph/detail/executable_assembler.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include <vrt/graph/ir/scheduled_graph.hpp>

namespace vrt::graph::detail {
namespace {

class ExecutableAssembler {
   public:
    CompileResult<AssembledExecutables> assemble(
        const ScheduledGraph& scheduled,
        const std::map<std::string, std::shared_ptr<IDevice>>& devices,
        const BridgeLookup& bridgeFor,
        const std::shared_ptr<std::map<std::string, std::uint64_t>>&
            scalarValues) {
        /*
         * Resource setup: bind logical events and scalars, then create shared
         * runtime state while the binding result owns all leases.
         * Queue assembly: materialize host actions and lower each scheduled
         * queue to one owned backend executable.
         * Graph assembly: connect control children, select roots, and finalize
         * the reachable executable trees.
         * Ownership handoff: move leases, runtime state, executables, and
         * their non-owning root pointers into the result.
         */
        scheduled_ = &scheduled;
        devices_ = &devices;
        bridgeFor_ = &bridgeFor;
        CompileResult<BackendResourceBindings> bound =
            bindBackendResources(scheduled, devices);
        if (!bound.ok()) {
            return CompileResult<AssembledExecutables>::failure(
                std::move(bound.diagnostics));
        }
        resources_ = &*bound.output;
        runtimeState_ = makeRuntimeState(scalarValues);
        hostActions_ = std::make_shared<HostActionTable>();
        indexQueues();
        assembleHostActions();
        lowerQueues();
        connectControlChildren();
        collectRoots();
        if (diagnostics_.hasErrors()) {
            return CompileResult<AssembledExecutables>::failure(
                std::move(diagnostics_));
        }
        for (IBackendExecutable* root : roots_) root->finalize();

        Diagnostics diagnostics;
        diagnostics.append(std::move(bound.diagnostics));
        return CompileResult<AssembledExecutables>::success(
            AssembledExecutables(
                std::move(*bound.output), std::move(runtimeState_),
                collectIo(), pinDevices(), std::move(executables_),
                std::move(roots_)),
            std::move(diagnostics));
    }

   private:
    std::shared_ptr<BackendRuntimeState> makeRuntimeState(
        const std::shared_ptr<std::map<std::string, std::uint64_t>>&
            scalarValues) const {
        /*
         * Scalar callbacks address device-owned slots by owner and key. The
         * runtime state also retains shared access objects for those owners;
         * the final resource bindings keep the corresponding leases alive.
         */
        BackendRuntimeState::ScalarBindings scalars;
        for (const auto& [logical, scalar] : resources_->scalars()) {
            (void)logical;
            scalars[{scalar.owner, scalar.key}] = scalar.physical;
        }
        return std::make_shared<BackendRuntimeState>(
            scalarValues, std::move(scalars), resources_->ownerAccess());
    }

    void indexQueues() {
        /*
         * These are non-owning indexes into the ScheduledGraph supplied to
         * assemble(); they are used only while backend objects are built.
         */
        for (const QueueProgram& queue : scheduled_->queues()) {
            queues_[queue.id] = &queue;
            queuesByRegion_[queue.region].push_back(queue.id);
        }
    }

    const TransferRoute* route(RouteId id) const {
        auto found = std::find_if(
            scheduled_->routed().routes().begin(),
            scheduled_->routed().routes().end(),
            [&](const TransferRoute& candidate) {
                return candidate.id == id;
            });
        return found == scheduled_->routed().routes().end()
                   ? nullptr
                   : &*found;
    }

    static const TransferLeg* leg(
        const TransferRoute& route, TransferLegId id) {
        auto found = std::find_if(
            route.legs.begin(), route.legs.end(),
            [&](const TransferLeg& candidate) {
                return candidate.id == id;
            });
        return found == route.legs.end() ? nullptr : &*found;
    }

    bool usesDeviceResources(RouteId id) const {
        const TransferRoute* found = route(id);
        return found &&
               std::holds_alternative<DeviceRendezvousSynchronization>(
                   found->requirement.synchronization);
    }

    std::optional<ValueId> routeValue(
        const TransferRoute& transfer) const {
        const std::optional<ReplicaId> source =
            transferReplica(transfer.requirement.signature.source);
        const ValueReplica* replica =
            source ? scheduled_->routed().findReplica(*source) : nullptr;
        return replica ? std::optional<ValueId>(replica->value)
                       : std::nullopt;
    }

    std::optional<ValueId> routeDestinationValue(
        const TransferRoute& transfer) const {
        const std::optional<ReplicaId> destination =
            transferReplica(
                transfer.requirement.signature.destination);
        const ValueReplica* replica =
            destination
                ? scheduled_->routed().findReplica(*destination)
                : nullptr;
        return replica ? std::optional<ValueId>(replica->value)
                       : std::nullopt;
    }

    std::string operationName(std::optional<NodeId> operation) const {
        if (!operation) return {};
        const AuthoredOperation* authored =
            scheduled_->routed().placed().resolved()
                .authored().index().findOperation(*operation);
        return authored ? authoredSourceId(*authored)
                        : "node_" + std::to_string(operation->value());
    }

    /*
     * Materialize one bridge operation for each payload case:
     * - barriers carry ordering and endpoint names but no graph value;
     * - scalars use runtime callbacks keyed by source and destination tokens;
     * - buffers use graph tokens, with private storage for isolated routes.
     */
    BridgeStepPair materialize(
        const TransferRoute& transfer,
        const TransferLeg& transferLeg) {
        /* Resolve both endpoint devices and the bridge joining them. */
        auto source = devices_->find(transferLeg.source.value());
        auto destination =
            devices_->find(transferLeg.destination.value());
        if (source == devices_->end() ||
            destination == devices_->end()) {
            throw std::runtime_error(
                "GraphCompiler: transfer endpoint is unavailable");
        }
        IBridge* bridge = (*bridgeFor_)(
            transferLeg.source.value(), transferLeg.destination.value());
        if (!bridge) {
            throw std::runtime_error(
                "GraphCompiler: transfer bridge is unavailable");
        }
        const std::string producer =
            operationName(transfer.requirement.sourceAnchor.operation());
        const std::string consumer =
            operationName(
                transfer.requirement.destinationAnchor.operation());
        if (transfer.requirement.signature.payload ==
            TransferPayloadKind::Barrier) {
            return bridge->makeBarrier(
                *source->second, *destination->second,
                producer, consumer);
        }

        /* Resolve the routed source and destination values, when distinct. */
        const std::optional<ValueId> valueId = routeValue(transfer);
        const ResolvedValue* value =
            valueId
                ? scheduled_->routed().placed().resolved()
                      .findValue(*valueId)
                : nullptr;
        const std::optional<ValueId> destinationValueId =
            routeDestinationValue(transfer);
        const ResolvedValue* destinationValue =
            destinationValueId
                ? scheduled_->routed().placed().resolved()
                      .findValue(*destinationValueId)
                : nullptr;
        if (!value) {
            throw std::runtime_error(
                "GraphCompiler: transfer value is unavailable");
        }

        /* Scalar bridge callbacks read and write through shared runtime state. */
        if (transfer.requirement.signature.payload ==
            TransferPayloadKind::Scalar) {
            const GraphScalar* sourceToken =
                resolvedScalarToken(*value);
            const GraphScalar* destinationToken =
                destinationValue
                    ? resolvedScalarToken(*destinationValue)
                    : sourceToken;
            if (!sourceToken || !destinationToken) {
                throw std::runtime_error(
                    "GraphCompiler: scalar transfer has no token");
            }
            std::shared_ptr<BackendRuntimeState> state = runtimeState_;
            BridgeRuntimeContext runtime;
            runtime.readScalar =
                [state](IDevice& device, const std::string& key) {
                    return state->readScalar(
                        DeviceId(device.id()), key);
                };
            runtime.writeScalar =
                [state](IDevice& device, const std::string& key,
                        std::uint64_t bits) {
                    state->writeScalar(
                        DeviceId(device.id()), key, bits);
                };
            return bridge->makeScalarTransfer(
                *source->second, *destination->second,
                scopedScalarKey(
                    sourceToken->scopeId(),
                    sourceToken->varName()),
                scopedScalarKey(
                    destinationToken->scopeId(),
                    destinationToken->varName()),
                producer, consumer, runtime);
        }

        /*
         * Isolated buffer routes get private destination storage. Later legs
         * in the route use that same private token as their source.
         */
        const GraphBuffer* sourceBuffer =
            resolvedBufferToken(*value);
        const GraphBuffer* destinationBuffer =
            destinationValue
                ? resolvedBufferToken(*destinationValue)
                : sourceBuffer;
        if (!sourceBuffer || !destinationBuffer) {
            throw std::runtime_error(
                "GraphCompiler: buffer transfer has no token");
        }
        GraphBuffer sourceToken = *sourceBuffer;
        GraphBuffer destinationToken = *destinationBuffer;
        if (transfer.requirement.isolatedDestination) {
            destinationToken =
                ::vrt::graph::detail::makeGraphBuffer(
                    sourceBuffer->type(),
                    sourceBuffer->name() + "__route_" +
                        std::to_string(transfer.id.value()),
                    sourceBuffer->scopeId(),
                    sourceBuffer->maybeSizeScalar(),
                    sourceBuffer->graphId());
            if (transferLeg.source !=
                transfer.requirement.signature
                    .sourceLocation.device) {
                sourceToken = destinationToken;
            }
        }
        return bridge->makeTransfer(
            *source->second, *destination->second,
            sourceToken, destinationToken, 0, producer, consumer);
    }

    /*
     * Produce, action, and consume steps for one leg must share a single
     * bridge operation and its callbacks. Cache the pair on first use.
     */
    BridgeStepPair& bridgePair(
        const TransferRoute& transfer,
        const TransferLeg& transferLeg) {
        auto found = bridgePairs_.find(transferLeg.id);
        if (found != bridgePairs_.end()) return found->second;
        return bridgePairs_
            .emplace(
                transferLeg.id,
                materialize(transfer, transferLeg))
            .first->second;
    }

    /*
     * Keep the bridge operation beside its callbacks. Queue lowerers copy the
     * relevant callbacks and retain the operation as their lifetime pin.
     */
    void addAction(
        ScheduleStepId step, RouteId route,
        const std::shared_ptr<IBridgeOp>& operation,
        std::function<void()> action,
        std::function<bool()> ready = [] { return true; }) {
        hostActions_->add(
            step, route,
            HostAction{
                operation ? operation->label() : "device_copy",
                operation, std::move(action), std::move(ready)});
    }

    /*
     * A host-mediated copy is one action on the executor step. Its target is
     * private route storage on the same device, not a second bridge endpoint.
     */
    void assembleDeviceCopy(
        ScheduleStepId step, const TransferRoute& transfer,
        const TransferLeg& transferLeg) {
        const std::optional<ValueId> valueId = routeValue(transfer);
        const ResolvedValue* value =
            valueId
                ? scheduled_->routed().placed().resolved()
                      .findValue(*valueId)
                : nullptr;
        const GraphBuffer* sourceToken =
            value ? resolvedBufferToken(*value) : nullptr;
        if (!sourceToken) {
            throw std::runtime_error(
                "GraphCompiler: device copy token is unavailable");
        }
        GraphBuffer target = ::vrt::graph::detail::makeGraphBuffer(
            sourceToken->type(),
            sourceToken->name() + "__route_" +
                std::to_string(transfer.id.value()),
            sourceToken->scopeId(),
            sourceToken->maybeSizeScalar(),
            sourceToken->graphId());
        auto device = devices_->find(transferLeg.source.value());
        if (device == devices_->end()) {
            throw std::runtime_error(
                "GraphCompiler: device copy endpoint is unavailable");
        }
        addAction(
            step, transfer.id, nullptr,
            device->second->makeDeviceCopyAction(
                *sourceToken, target, sourceToken->type(),
                transfer.requirement.signature.sourceLocation.region
                    ? transfer.requirement.signature.sourceLocation
                          .region->value()
                    : "",
                transfer.requirement.signature.destinationLocation.region
                    ? transfer.requirement.signature.destinationLocation
                          .region->value()
                    : ""));
    }

    /*
     * Transfer action placement has three cases:
     * - host-mediated device copies attach only to the action step;
     * - device rendezvous attach both bridge halves to the action step;
     * - host events attach producer and consumer halves to their respective
     *   produce and consume steps.
     *
     * Non-transfer steps and missing route metadata add no host action.
     */
    void assembleTransferStep(const ScheduledStep& step) {
        const auto* action =
            std::get_if<ScheduledTransferAction>(&step.payload);
        const auto* produce =
            std::get_if<ScheduledTransferProduce>(&step.payload);
        const auto* consume =
            std::get_if<ScheduledTransferConsume>(&step.payload);
        if (!action && !produce && !consume) return;
        const RouteId routeId =
            action ? action->route
                   : produce ? produce->route : consume->route;
        const TransferLegId legId =
            action ? action->leg
                   : produce ? produce->leg : consume->leg;
        const TransferRoute* transfer = route(routeId);
        const TransferLeg* transferLeg =
            transfer ? leg(*transfer, legId) : nullptr;
        if (!transfer || !transferLeg) return;
        if (transferLeg->mechanism ==
            TransferMechanism::HostMediatedDeviceCopy) {
            if (action) {
                assembleDeviceCopy(step.id, *transfer, *transferLeg);
            }
            return;
        }
        if (action && !usesDeviceResources(routeId)) return;
        if (!action && usesDeviceResources(routeId)) return;
        BridgeStepPair& pair = bridgePair(*transfer, *transferLeg);
        if (action || produce) {
            addAction(
                step.id, routeId, pair.op, pair.producerAction);
        }
        if (action || consume) {
            addAction(
                step.id, routeId, pair.op, pair.consumerAction,
                pair.consumerTryReady);
        }
    }

    void assembleHostActions() {
        /* Index transfer callbacks before any queue lowerer asks for them. */
        for (const auto& [id, step] : scheduled_->steps()) {
            (void)id;
            assembleTransferStep(step);
        }
    }

    /*
     * Lower each queue on its owning device. Unknown devices and unsupported
     * lowerers become diagnostics; successful unique_ptrs are the sole owners
     * of queue executables, while executableByQueue_ is only an index.
     */
    void lowerQueues() {
        executables_.reserve(scheduled_->queues().size());
        for (const QueueProgram& queue : scheduled_->queues()) {
            auto device = devices_->find(queue.device.value());
            if (device == devices_->end() || !device->second) {
                diagnostics_.error(
                    DiagCode::UnknownDevice,
                    "GraphCompiler: scheduled queue targets unknown device '" +
                        queue.device.value() + "'");
                continue;
            }
            BackendLoweringContext context{
                *scheduled_, queue, *resources_,
                runtimeState_, *hostActions_};
            std::unique_ptr<IBackendExecutable> executable =
                device->second->lowerQueue(context);
            if (!executable) {
                diagnostics_.error(
                    DiagCode::UnsupportedOperation,
                    "GraphCompiler: device '" + queue.device.value() +
                        "' has no direct scheduled-queue lowerer");
                continue;
            }
            executableByQueue_[queue.id] = executable.get();
            executables_.push_back(std::move(executable));
        }
    }

    static ControlChildRole childRole(AuthoredChildRole role) {
        switch (role) {
            case AuthoredChildRole::LoopBody:
                return ControlChildRole::LoopBody;
            case AuthoredChildRole::ConditionalThen:
                return ControlChildRole::ConditionalThen;
            case AuthoredChildRole::ConditionalElse:
                return ControlChildRole::ConditionalElse;
        }
        return ControlChildRole::LoopBody;
    }

    std::optional<ScheduleStepId> controlStep(
        QueueId queue, NodeId control) const {
        for (ScheduleStepId step : queues_.at(queue)->steps) {
            const auto* operation =
                std::get_if<ScheduledOperation>(
                    &scheduled_->steps().at(step).payload);
            if (operation && operation->operation == control) {
                return step;
            }
        }
        return std::nullopt;
    }

    /*
     * A co-located control receives every executable in each child region. A
     * split control is lowered once per participant, so each parent receives
     * only child executables on its own device. All handles are non-owning
     * references into executables_.
     */
    void connectControlChildren() {
        const ResolvedGraph& resolved =
            scheduled_->routed().placed().resolved();
        for (const auto& [region, children] :
             resolved.authored().index().regions()) {
            (void)children;
            for (const AuthoredChildRegion& child :
                 resolved.authored().index().children(region)) {
                const ResolvedOperation* control =
                    resolved.findOperation(child.control);
                if (!control) continue;
                const bool split =
                    std::holds_alternative<SplitControlPlacement>(
                        scheduled_->routed().placed()
                            .controlPlacements().at(child.control));
                for (QueueId parentQueue :
                     queuesByRegion_[control->region]) {
                    auto parent =
                        executableByQueue_.find(parentQueue);
                    const std::optional<ScheduleStepId> step =
                        controlStep(parentQueue, child.control);
                    if (parent == executableByQueue_.end() || !step) {
                        continue;
                    }
                    std::vector<QueueExecutableHandle> handles;
                    for (QueueId childQueue :
                         queuesByRegion_[child.region]) {
                        auto executable =
                            executableByQueue_.find(childQueue);
                        if (executable == executableByQueue_.end()) {
                            continue;
                        }
                        if (split &&
                            queues_.at(childQueue)->device !=
                                queues_.at(parentQueue)->device) {
                            continue;
                        }
                        handles.push_back(
                            {childQueue, executable->second});
                    }
                    parent->second->connectControlChildren(
                        {parentQueue, *step, child.control},
                        childRole(child.role), std::move(handles));
                }
            }
        }
    }

    void collectRoots() {
        /*
         * Non-empty queues in the graph's root region are launch roots.
         * roots_ borrows them from executables_; the result moves both the
         * owning and borrowing vectors into the execution plan together.
         */
        const RegionId root =
            scheduled_->routed().placed().resolved().root().id;
        for (QueueId queue : queuesByRegion_[root]) {
            auto executable = executableByQueue_.find(queue);
            if (executable != executableByQueue_.end() &&
                !queues_.at(queue)->steps.empty()) {
                roots_.push_back(executable->second);
            }
        }
    }

    ExecutionIoMetadata collectIo() const {
        /*
         * Explicit I/O steps expose buffer and scalar names by direction.
         * Buffer size scalars are implicit runtime inputs and are collected
         * separately from every resolved buffer which names one.
         */
        ExecutionIoMetadata io;
        const ResolvedGraph& resolved =
            scheduled_->routed().placed().resolved();

        /* Collect explicit graph input and output values. */
        for (const auto& [id, step] : scheduled_->steps()) {
            (void)id;
            const auto* input =
                std::get_if<ScheduledGraphInput>(&step.payload);
            const auto* output =
                std::get_if<ScheduledGraphOutput>(&step.payload);
            if (!input && !output) continue;
            for (ValueId valueId :
                 input ? input->values : output->values) {
                const ResolvedValue* value =
                    resolved.findValue(valueId);
                if (!value) continue;
                if (const GraphBuffer* buffer =
                        resolvedBufferToken(*value)) {
                    (input ? io.inputBuffers : io.outputBuffers)
                        .insert(scopedBufferKey(
                            buffer->scopeId(), buffer->name()));
                } else if (const GraphScalar* scalar =
                               resolvedScalarToken(*value)) {
                    (input ? io.inputScalars : io.outputScalars)
                        .insert(scopedScalarKey(
                            scalar->scopeId(), scalar->varName()));
                }
            }
        }

        /* Add implicit size-scalar bindings used by graph buffers. */
        for (const auto& [id, value] : resolved.values()) {
            (void)id;
            const GraphBuffer* buffer = resolvedBufferToken(value);
            if (!buffer || !buffer->hasSizeScalar()) continue;
            const GraphScalar& scalar = buffer->sizeScalar();
            io.sizeScalars[scopedScalarKey(
                scalar.scopeId(), scalar.varName())] =
                scalar.varName();
        }
        return io;
    }

    std::vector<std::shared_ptr<IDevice>> pinDevices() const {
        /*
         * Keep every device supplied to compilation alive for the plan's
         * lifetime; executables and bridge actions may retain device-local
         * state through non-owning backend references.
         */
        std::vector<std::shared_ptr<IDevice>> result;
        result.reserve(devices_->size());
        for (const auto& [id, device] : *devices_) {
            (void)id;
            result.push_back(device);
        }
        return result;
    }

    /* Borrowed inputs and phase-local bindings, valid during assemble(). */
    const ScheduledGraph* scheduled_ = nullptr;
    const std::map<std::string, std::shared_ptr<IDevice>>* devices_ =
        nullptr;
    const BridgeLookup* bridgeFor_ = nullptr;
    const BackendResourceBindings* resources_ = nullptr;

    /* Shared runtime data and phase-local host action catalog. */
    std::shared_ptr<BackendRuntimeState> runtimeState_;
    std::shared_ptr<HostActionTable> hostActions_;
    Diagnostics diagnostics_;

    /* Non-owning schedule indexes and cached bridge action pairs. */
    std::map<QueueId, const QueueProgram*> queues_;
    std::map<RegionId, std::vector<QueueId>> queuesByRegion_;
    std::map<TransferLegId, BridgeStepPair> bridgePairs_;

    /* Sole executable owners followed by non-owning queue and root indexes. */
    std::vector<std::unique_ptr<IBackendExecutable>> executables_;
    std::map<QueueId, IBackendExecutable*> executableByQueue_;
    std::vector<IBackendExecutable*> roots_;
};

}  // namespace

CompileResult<AssembledExecutables> assembleExecutables(
    const ScheduledGraph& scheduled,
    const std::map<std::string, std::shared_ptr<IDevice>>& devices,
    const BridgeLookup& bridgeFor,
    const std::shared_ptr<std::map<std::string, std::uint64_t>>&
        scalarValues) {
    /*
     * Bridge materialization and backend lowering report unsupported runtime
     * combinations with runtime_error. Convert those failures at the public
     * compiler boundary; structured diagnostics return directly.
     */
    try {
        return ExecutableAssembler().assemble(
            scheduled, devices, bridgeFor, scalarValues);
    } catch (const std::runtime_error& error) {
        Diagnostics diagnostics;
        diagnostics.error(
            DiagCode::UnsupportedOperation, error.what());
        return CompileResult<AssembledExecutables>::failure(
            std::move(diagnostics));
    }
}

}  // namespace vrt::graph::detail
