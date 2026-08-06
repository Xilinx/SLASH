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

#include "route_graph_internal.hpp"

#include <utility>

namespace vrt::graph::route_detail {

namespace {

/*
 * Regionless placement denotes device-wide accessible storage. A local route
 * is needed only when both endpoints name different concrete regions, unless
 * copy-on-write explicitly forces an independent destination.
 */
bool sameLocation(const RouteSignature& signature) {
    return signature.sourceLocation.device ==
               signature.destinationLocation.device &&
           (!signature.sourceLocation.region ||
            !signature.destinationLocation.region ||
            signature.sourceLocation.region ==
                signature.destinationLocation.region);
}

RouteSelection selectionError(
    DiagCode code, std::string message) {
    RouteSelection result;
    result.error = code;
    result.message = std::move(message);
    return result;
}

/*
 * Host-involved direct transfers are driven by the host. A device-to-device
 * bridge instead runs on the destination queue, where arrival ordering is
 * consumed.
 */
TransferExecutor directExecutor(
    const RouteSignature& signature,
    const TransferCapabilityCatalog& capabilities) {
    if (capabilities.host() &&
        (signature.sourceLocation.device == *capabilities.host() ||
         signature.destinationLocation.device ==
             *capabilities.host())) {
        return HostTransferExecutor{*capabilities.host()};
    }
    return DestinationQueueTransferExecutor{};
}

}  // namespace

/*
 * Route selection is exhaustive and ordered: shared storage needs no motion;
 * one-device isolation or region changes require a local copy; a native edge
 * beats a two-leg host bounce. The bounce is legal only between non-host
 * endpoints with both host edges present. Anything else is a hard no-route
 * diagnostic rather than an implicit transport guess.
 */
RouteSelection selectRoute(
    const RouteSignature& signature,
    const TransferCapabilityCatalog& capabilities,
    bool forceCopy) {
    RouteSelection result;

    /*
     * Preserve the absence of a route as a successful no-op; dependency
     * planning still records ordering for values already sharing storage.
     */
    if (sameLocation(signature) && !forceCopy) return result;

    const DeviceId& source = signature.sourceLocation.device;
    const DeviceId& destination =
        signature.destinationLocation.device;
    result.transferRequired = true;

    /*
     * Forced isolation and concrete region changes stay on one device, but
     * are valid only when that device exposes a memory-region copy engine.
     */
    if (source == destination) {
        if (!capabilities.supportsMemoryRegionCopies(source)) {
            return selectionError(
                DiagCode::IncompatibleMemoryPlacement,
                "GraphCompiler: device '" + source.value() +
                    "' cannot copy between memory regions");
        }
        const TransferExecutor executor =
            capabilities.host()
                ? TransferExecutor(
                      HostTransferExecutor{*capabilities.host()})
                : TransferExecutor(
                      DestinationQueueTransferExecutor{});
        result.legs.push_back(
            {TransferMechanism::HostMediatedDeviceCopy,
             source, destination, executor});
        return result;
    }

    /*
     * Prefer the single native edge: it minimizes synchronization points and
     * avoids staging through host ownership.
     */
    if (capabilities.hasDirect(source, destination)) {
        result.legs.push_back(
            {TransferMechanism::DirectBridge, source, destination,
             directExecutor(signature, capabilities)});
        return result;
    }

    /*
     * Host bounce is a real two-edge path, not a generic fallback. Both
     * directions must be declared and the host cannot duplicate an endpoint.
     */
    if (capabilities.host() && source != *capabilities.host() &&
        destination != *capabilities.host() &&
        capabilities.hasDirect(source, *capabilities.host()) &&
        capabilities.hasDirect(
            *capabilities.host(), destination)) {
        const TransferExecutor executor =
            HostTransferExecutor{*capabilities.host()};
        result.legs.push_back(
            {TransferMechanism::HostBounce, source,
             *capabilities.host(), executor});
        result.legs.push_back(
            {TransferMechanism::HostBounce, *capabilities.host(),
             destination, executor});
        return result;
    }

    return selectionError(
        DiagCode::MissingTransferRoute,
        "GraphCompiler: no transfer route from '" +
            source.value() + "' to '" + destination.value() + "'");
}

}  // namespace vrt::graph::route_detail
