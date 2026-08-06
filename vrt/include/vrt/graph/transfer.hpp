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
 * @file transfer.hpp
 * @brief Declarative transfer requirements, routes, and capability catalog.
 */

#ifndef VRT_GRAPH_TRANSFER_HPP
#define VRT_GRAPH_TRANSFER_HPP

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vrt/graph/capabilities.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/ids.hpp>

namespace vrt::graph {

class IDevice;

enum class TransferPayloadKind {
    Buffer,
    Scalar,
    Barrier,
};

enum class TransferMechanism {
    DirectBridge,
    HostBounce,
    HostMediatedDeviceCopy,
};

enum class TransferPhase {
    PreLaunch,
    Once,
    PerIteration,
    AfterControl,
};

enum class TransferAnchorKind {
    GraphInput,
    GraphOutput,
    Operation,
};

class TransferControlAnchor {
   public:
    static TransferControlAnchor graphInput(RegionId region) {
        return TransferControlAnchor(
            TransferAnchorKind::GraphInput, region, NodeId{});
    }

    static TransferControlAnchor graphOutput(RegionId region) {
        return TransferControlAnchor(
            TransferAnchorKind::GraphOutput, region, NodeId{});
    }

    static TransferControlAnchor operation(
        NodeId operation, RegionId region) {
        return TransferControlAnchor(
            TransferAnchorKind::Operation, region, operation);
    }

    TransferAnchorKind kind() const { return kind_; }
    RegionId region() const { return region_; }
    std::optional<NodeId> operation() const {
        return kind_ == TransferAnchorKind::Operation
                   ? std::optional<NodeId>(operation_)
                   : std::nullopt;
    }

    friend bool operator==(
        const TransferControlAnchor& lhs,
        const TransferControlAnchor& rhs) {
        return std::tie(lhs.kind_, lhs.region_, lhs.operation_) ==
               std::tie(rhs.kind_, rhs.region_, rhs.operation_);
    }

    friend bool operator<(
        const TransferControlAnchor& lhs,
        const TransferControlAnchor& rhs) {
        return std::tie(lhs.kind_, lhs.region_, lhs.operation_) <
               std::tie(rhs.kind_, rhs.region_, rhs.operation_);
    }

   private:
    TransferControlAnchor(
        TransferAnchorKind kind, RegionId region, NodeId operation)
        : kind_(kind), region_(region), operation_(operation) {}

    TransferAnchorKind kind_;
    RegionId           region_;
    NodeId             operation_;
};

struct GraphTransferScope {
    RegionId region;

    friend bool operator==(
        const GraphTransferScope& lhs,
        const GraphTransferScope& rhs) {
        return lhs.region == rhs.region;
    }

    friend bool operator<(
        const GraphTransferScope& lhs,
        const GraphTransferScope& rhs) {
        return lhs.region < rhs.region;
    }
};

struct ControlTransferScope {
    NodeId   control;
    RegionId region;

    friend bool operator==(
        const ControlTransferScope& lhs,
        const ControlTransferScope& rhs) {
        return std::tie(lhs.control, lhs.region) ==
               std::tie(rhs.control, rhs.region);
    }

    friend bool operator<(
        const ControlTransferScope& lhs,
        const ControlTransferScope& rhs) {
        return std::tie(lhs.control, lhs.region) <
               std::tie(rhs.control, rhs.region);
    }
};

using TransferControlScope =
    std::variant<GraphTransferScope, ControlTransferScope>;

struct HostTransferExecutor {
    DeviceId device;

    friend bool operator==(
        const HostTransferExecutor& lhs,
        const HostTransferExecutor& rhs) {
        return lhs.device == rhs.device;
    }

    friend bool operator<(
        const HostTransferExecutor& lhs,
        const HostTransferExecutor& rhs) {
        return lhs.device < rhs.device;
    }
};

struct SourceQueueTransferExecutor {
    friend bool operator==(
        SourceQueueTransferExecutor, SourceQueueTransferExecutor) {
        return true;
    }

    friend bool operator<(
        SourceQueueTransferExecutor, SourceQueueTransferExecutor) {
        return false;
    }
};

struct DestinationQueueTransferExecutor {
    friend bool operator==(
        DestinationQueueTransferExecutor,
        DestinationQueueTransferExecutor) {
        return true;
    }

    friend bool operator<(
        DestinationQueueTransferExecutor,
        DestinationQueueTransferExecutor) {
        return false;
    }
};

using TransferExecutor =
    std::variant<HostTransferExecutor, SourceQueueTransferExecutor,
                 DestinationQueueTransferExecutor>;

enum class TransferCompletionProtocol {
    ProducerConsumerAcknowledged,
    ExecutorSignalsReady,
};

struct HostEventSynchronization {};

struct DeviceRendezvousSynchronization {
    DeviceId owner;
};

using TransferSynchronization =
    std::variant<HostEventSynchronization,
                 DeviceRendezvousSynchronization>;

struct ReplicaTransferEndpoint {
    ReplicaId replica;

    friend bool operator==(
        ReplicaTransferEndpoint lhs, ReplicaTransferEndpoint rhs) {
        return lhs.replica == rhs.replica;
    }

    friend bool operator<(
        ReplicaTransferEndpoint lhs, ReplicaTransferEndpoint rhs) {
        return lhs.replica < rhs.replica;
    }
};

struct BarrierTransferEndpoint {
    DeviceId device;
    NodeId   operation;

    friend bool operator==(
        const BarrierTransferEndpoint& lhs,
        const BarrierTransferEndpoint& rhs) {
        return std::tie(lhs.device, lhs.operation) ==
               std::tie(rhs.device, rhs.operation);
    }

    friend bool operator<(
        const BarrierTransferEndpoint& lhs,
        const BarrierTransferEndpoint& rhs) {
        return std::tie(lhs.device, lhs.operation) <
               std::tie(rhs.device, rhs.operation);
    }
};

using TransferEndpoint =
    std::variant<ReplicaTransferEndpoint, BarrierTransferEndpoint>;

struct TransferLocation {
    DeviceId                     device;
    std::optional<MemoryRegionId> region;

    friend bool operator==(
        const TransferLocation& lhs,
        const TransferLocation& rhs) {
        return std::tie(lhs.device, lhs.region) ==
               std::tie(rhs.device, rhs.region);
    }

    friend bool operator<(
        const TransferLocation& lhs,
        const TransferLocation& rhs) {
        return std::tie(lhs.device, lhs.region) <
               std::tie(rhs.device, rhs.region);
    }
};

struct RouteSignature {
    TransferPayloadKind  payload = TransferPayloadKind::Buffer;
    TransferEndpoint     source;
    TransferEndpoint     destination;
    TransferLocation     sourceLocation;
    TransferLocation     destinationLocation;
    TransferPhase        phase = TransferPhase::Once;
    TransferControlScope scope = GraphTransferScope{};

    friend bool operator==(
        const RouteSignature& lhs, const RouteSignature& rhs) {
        return std::tie(
                   lhs.payload, lhs.source, lhs.destination,
                   lhs.sourceLocation, lhs.destinationLocation,
                   lhs.phase, lhs.scope) ==
               std::tie(
                   rhs.payload, rhs.source, rhs.destination,
                   rhs.sourceLocation, rhs.destinationLocation,
                   rhs.phase, rhs.scope);
    }

    friend bool operator<(
        const RouteSignature& lhs, const RouteSignature& rhs) {
        return std::tie(
                   lhs.payload, lhs.source, lhs.destination,
                   lhs.sourceLocation, lhs.destinationLocation,
                   lhs.phase, lhs.scope) <
               std::tie(
                   rhs.payload, rhs.source, rhs.destination,
                   rhs.sourceLocation, rhs.destinationLocation,
                   rhs.phase, rhs.scope);
    }
};

struct TransferRequirement {
    RouteSignature                     signature;
    TransferControlAnchor              sourceAnchor =
        TransferControlAnchor::graphInput(RegionId{});
    TransferControlAnchor              destinationAnchor =
        TransferControlAnchor::graphOutput(RegionId{});
    TransferCompletionProtocol         completion =
        TransferCompletionProtocol::ProducerConsumerAcknowledged;
    TransferSynchronization            synchronization =
        HostEventSynchronization{};
    bool                               isolatedDestination = false;
    std::vector<RouteId>               prerequisites;
    std::vector<NodeId>                controlPrerequisites;
};

struct TransferLeg {
    TransferLegId       id;
    TransferMechanism   mechanism = TransferMechanism::DirectBridge;
    DeviceId            source;
    DeviceId            destination;
    TransferExecutor    executor =
        DestinationQueueTransferExecutor{};
    RegionId            sourceRegion;
    RegionId            destinationRegion;
    RegionId            executorRegion;
};

struct TransferRoute {
    RouteId                  id;
    TransferRequirement      requirement;
    std::vector<TransferLeg> legs;
};

inline std::optional<ReplicaId> transferReplica(
    const TransferEndpoint& endpoint) {
    const auto* replica =
        std::get_if<ReplicaTransferEndpoint>(&endpoint);
    return replica ? std::optional<ReplicaId>(replica->replica)
                   : std::nullopt;
}

inline DeviceId transferExecutorDevice(
    const TransferExecutor& executor,
    const DeviceId& source, const DeviceId& destination) {
    if (const auto* host =
            std::get_if<HostTransferExecutor>(&executor)) {
        return host->device;
    }
    return std::holds_alternative<SourceQueueTransferExecutor>(
               executor)
               ? source
               : destination;
}

class TransferCapabilityCatalog {
   public:
    static TransferCapabilityCatalog fromGraph(
        const std::map<std::string, std::shared_ptr<IDevice>>& devices,
        const std::map<std::pair<DeviceType, DeviceType>,
                       BridgeFactory>& bridgeFactories);

    bool hasDirect(DeviceId source, DeviceId destination) const;
    bool supportsMemoryRegionCopies(DeviceId device) const;
    bool ownsRendezvousNamespace(DeviceId device) const;
    const std::optional<DeviceId>& host() const { return host_; }

   private:
    std::set<std::pair<DeviceId, DeviceId>> direct_;
    std::set<DeviceId>                      memoryRegionCopyDevices_;
    std::set<DeviceId>                      rendezvousOwners_;
    std::optional<DeviceId>                 host_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_TRANSFER_HPP
