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
 * @file semantic_plan.hpp
 * @brief Deterministic, implementation-neutral projection of compiled DGraphs.
 */

#ifndef VRT_GRAPH_SEMANTIC_PLAN_HPP
#define VRT_GRAPH_SEMANTIC_PLAN_HPP

#include <map>
#include <string>
#include <vector>

namespace vrt::graph {

struct DGraph;
class ScheduledGraph;

/**
 * @brief One normalized operation in a queue.
 *
 * Generated names, raw scope ids, closure identity, and physical RP1 slot
 * numbers are canonicalized by normalizeSemanticPlan().
 */
struct SemanticNode {
    std::string                        id;
    std::string                        kind;
    std::vector<std::string>           dependsOn;
    std::map<std::string, std::string> attributes;

    bool operator==(const SemanticNode& other) const {
        return id == other.id && kind == other.kind &&
               dependsOn == other.dependsOn && attributes == other.attributes;
    }
};

/**
 * @brief Ordered operations assigned to one device queue.
 */
struct SemanticQueue {
    std::string               deviceId;
    std::vector<SemanticNode> nodes;

    bool operator==(const SemanticQueue& other) const {
        return deviceId == other.deviceId && nodes == other.nodes;
    }
};

/**
 * @brief Normalized region and its recursively nested control regions.
 */
struct SemanticRegion {
    std::string                 path;
    std::string                 parentNode;
    std::string                 role;
    std::vector<SemanticQueue>  queues;
    std::vector<SemanticRegion> children;

    bool operator==(const SemanticRegion& other) const {
        return path == other.path && parentNode == other.parentNode &&
               role == other.role && queues == other.queues &&
               children == other.children;
    }
};

/**
 * @brief Deterministic semantic projection used by compiler differential tests.
 */
struct SemanticPlan {
    SemanticRegion root;

    bool operator==(const SemanticPlan& other) const {
        return root == other.root;
    }

    bool operator!=(const SemanticPlan& other) const {
        return !(*this == other);
    }

    /**
     * @brief Return a stable textual form suitable for diagnostics and fixtures.
     */
    std::string toString() const;
};

/**
 * @brief Normalize per-device DGraphs into a deterministic semantic plan.
 */
SemanticPlan normalizeSemanticPlan(const std::vector<DGraph>& dgraphs);

struct SemanticOperationPlacement {
    std::string              regionPath;
    std::string              authoredId;
    std::string              kind;
    std::vector<std::string> devices;

    bool operator==(const SemanticOperationPlacement& other) const {
        return regionPath == other.regionPath &&
               authoredId == other.authoredId && kind == other.kind &&
               devices == other.devices;
    }
};

struct SemanticPlacementPlan {
    std::vector<SemanticOperationPlacement> operations;

    bool operator==(const SemanticPlacementPlan& other) const {
        return operations == other.operations;
    }

    bool operator!=(const SemanticPlacementPlan& other) const {
        return !(*this == other);
    }

    std::string toString() const;
};

SemanticPlacementPlan normalizeOperationPlacements(
    const std::vector<DGraph>& dgraphs);

SemanticPlacementPlan normalizeOperationPlacements(
    const ScheduledGraph& scheduled);

}  // namespace vrt::graph

#endif  // VRT_GRAPH_SEMANTIC_PLAN_HPP
