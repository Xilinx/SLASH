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

#include <vrt/graph/semantic_plan.hpp>

#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <variant>

#include <vrt/graph/ir/scheduled_graph.hpp>

namespace vrt::graph {
namespace {

using PlacementKey = std::tuple<std::string, std::string, std::string>;

void indexAuthored(
    const AuthoredRegion& region, const std::string& path,
    std::map<NodeId, PlacementKey>& placements) {
    for (const AuthoredOperation& operation : region.operations) {
        const std::string& id = authoredSourceId(operation);
        std::visit(
            [&](const auto& concrete) {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, AuthoredKernel>) {
                    placements[concrete.id] = {path, id, "kernel"};
                } else if constexpr (std::is_same_v<T, AuthoredReprogram>) {
                    placements[concrete.id] = {path, id, "reprogram"};
                } else if constexpr (std::is_same_v<T, AuthoredLoop>) {
                    placements[concrete.id] = {path, id, "loop"};
                    if (concrete.body) {
                        indexAuthored(*concrete.body,
                                      path + "/" + id + ":loop_body",
                                      placements);
                    }
                } else if constexpr (std::is_same_v<T, AuthoredConditional>) {
                    placements[concrete.id] = {path, id, "conditional"};
                    if (concrete.thenRegion) {
                        indexAuthored(*concrete.thenRegion,
                                      path + "/" + id + ":then",
                                      placements);
                    }
                    if (concrete.elseRegion) {
                        indexAuthored(*concrete.elseRegion,
                                      path + "/" + id + ":else",
                                      placements);
                    }
                }
            },
            operation);
    }
}

}  // namespace

SemanticPlacementPlan normalizeOperationPlacements(
    const ScheduledGraph& scheduled) {
    std::map<NodeId, PlacementKey> authored;
    indexAuthored(
        scheduled.routed().placed().resolved().authored().root(),
        "root", authored);
    std::map<QueueId, DeviceId> queueDevices;
    for (const QueueProgram& queue : scheduled.queues()) {
        queueDevices[queue.id] = queue.device;
    }
    std::map<PlacementKey, std::set<std::string>> placements;
    for (const auto& [id, step] : scheduled.steps()) {
        (void)id;
        const auto* operation =
            std::get_if<ScheduledOperation>(&step.payload);
        if (!operation) continue;
        auto authoredPlacement = authored.find(operation->operation);
        auto device = queueDevices.find(step.queue);
        if (authoredPlacement != authored.end() &&
            device != queueDevices.end()) {
            placements[authoredPlacement->second].insert(
                device->second.value());
        }
    }
    SemanticPlacementPlan result;
    for (const auto& [key, devices] : placements) {
        result.operations.push_back({
            std::get<0>(key), std::get<1>(key), std::get<2>(key),
            {devices.begin(), devices.end()}});
    }
    return result;
}

std::string SemanticPlacementPlan::toString() const {
    std::ostringstream out;
    for (const SemanticOperationPlacement& operation : operations) {
        out << operation.regionPath << ' ' << operation.authoredId
            << ' ' << operation.kind << " [";
        for (std::size_t i = 0; i < operation.devices.size(); ++i) {
            if (i != 0) out << ',';
            out << operation.devices[i];
        }
        out << "]\n";
    }
    return out.str();
}

}  // namespace vrt::graph
