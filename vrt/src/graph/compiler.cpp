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

#include <vrt/graph/compiler.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <vrt/graph/capabilities.hpp>
#include <vrt/graph/detail/executable_assembler.hpp>
#include <vrt/graph/ir/authored_graph.hpp>
#include <vrt/graph/ir/placed_graph.hpp>
#include <vrt/graph/ir/resolved_graph.hpp>
#include <vrt/graph/ir/routed_graph.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>
#include <vrt/graph/pass/validate_authored_graph.hpp>
#include <vrt/graph/transfer.hpp>

namespace vrt::graph {

CompileResult<ExecutionPlan> GraphCompiler::compile(
    const AuthoredGraph& authored,
    const std::map<std::string, std::shared_ptr<IDevice>>& devices,
    const std::map<std::pair<DeviceType, DeviceType>,
                   BridgeFactory>& bridgeFactories,
    const BridgeFor& bridgeFor,
    const std::shared_ptr<std::map<std::string, std::uint64_t>>&
        scalarValues) const {
    /*
     * Compilation has three phases:
     * - preflight graph-host connectivity for every non-host device;
     * - validate, resolve, place, route, and schedule backend-neutral IR;
     * - bind physical resources and lower queue executables.
     *
     * Stop at the first failing phase. On success, retain non-fatal
     * diagnostics from every completed IR stage.
     */

    /* Require bridge factories in both directions for graph I/O traffic. */
    Diagnostics preflight;
    std::optional<DeviceType> hostType;
    for (const auto& [name, device] : devices) {
        (void)name;
        if (device && device->compilerCapabilities().hostsGraphIo) {
            hostType = device->type();
            break;
        }
    }
    if (hostType) {
        for (const auto& [name, device] : devices) {
            if (!device ||
                device->compilerCapabilities().hostsGraphIo) {
                continue;
            }
            if (bridgeFactories.count({*hostType, device->type()}) == 0 ||
                bridgeFactories.count({device->type(), *hostType}) == 0) {
                preflight.error(
                    DiagCode::MissingTransferRoute,
                    "GraphCompiler: device '" + name +
                        "' requires bridge factories to and from the "
                        "graph host");
            }
        }
    }
    if (preflight.hasErrors()) {
        return CompileResult<ExecutionPlan>::failure(
            std::move(preflight));
    }

    /* Build each graph IR only after the preceding stage validates. */
    CompileResult<AuthoredGraph> validated =
        validateAuthoredGraph(authored);
    if (!validated.ok()) {
        return CompileResult<ExecutionPlan>::failure(
            std::move(validated.diagnostics));
    }

    CompileResult<ResolvedGraph> resolved =
        detail::resolveValidatedGraph(*validated.output);
    if (!resolved.ok()) {
        return CompileResult<ExecutionPlan>::failure(
            std::move(resolved.diagnostics));
    }

    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    if (!placed.ok()) {
        return CompileResult<ExecutionPlan>::failure(
            std::move(placed.diagnostics));
    }

    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(
            devices, bridgeFactories));
    if (!routed.ok()) {
        return CompileResult<ExecutionPlan>::failure(
            std::move(routed.diagnostics));
    }

    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    if (!scheduled.ok()) {
        return CompileResult<ExecutionPlan>::failure(
            std::move(scheduled.diagnostics));
    }

    /* Bind logical resources and lower scheduled queues to backend objects. */
    CompileResult<detail::AssembledExecutables> programs =
        detail::assembleExecutables(
            *scheduled.output, devices, bridgeFor, scalarValues);
    if (!programs.ok()) {
        return CompileResult<ExecutionPlan>::failure(
            std::move(programs.diagnostics));
    }

    /* Preserve warnings and notes from every successful compiler stage. */
    Diagnostics diagnostics;
    diagnostics.append(std::move(validated.diagnostics));
    diagnostics.append(std::move(resolved.diagnostics));
    diagnostics.append(std::move(placed.diagnostics));
    diagnostics.append(std::move(routed.diagnostics));
    diagnostics.append(std::move(scheduled.diagnostics));
    diagnostics.append(std::move(programs.diagnostics));
    return CompileResult<ExecutionPlan>::success(
        ExecutionPlan(std::move(*programs.output)),
        std::move(diagnostics));
}

}  // namespace vrt::graph
