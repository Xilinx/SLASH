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
 * @file backend_runtime.hpp
 * @brief Execution-plan-owned host actions and backend resource access.
 */

#ifndef VRT_GRAPH_BACKEND_RUNTIME_HPP
#define VRT_GRAPH_BACKEND_RUNTIME_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/crossdevice/bridge_op.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/ids.hpp>

namespace vrt::graph {

/*
 * HostActionTable is only an assembly catalog. Queue lowerers copy the
 * callbacks they need and retain operation as an explicit lifetime pin for
 * bridge-private staging and synchronization state. Device-copy actions have
 * no bridge operation and rely on their closure captures alone.
 */
struct HostAction {
    std::string                label;
    std::shared_ptr<IBridgeOp> operation;
    std::function<void()>      action;
    std::function<bool()>      tryReady = [] { return true; };
};

/*
 * Actions have a stable id plus step and route indexes because one scheduled
 * step may drive several transfer legs. The table becomes immutable before
 * queue lowering starts, so returned references need no synchronization.
 */
class HostActionTable {
   public:
    HostActionId add(ScheduleStepId step, RouteId route, HostAction action) {
        const HostActionId id(next_++);
        actions_.emplace(id, std::move(action));
        byStep_[step].push_back(id);
        byRoute_[route].push_back(id);
        return id;
    }

    const HostAction& at(HostActionId id) const { return actions_.at(id); }

    const std::vector<HostActionId>& find(ScheduleStepId step) const {
        static const std::vector<HostActionId> empty;
        auto it = byStep_.find(step);
        return it == byStep_.end() ? empty : it->second;
    }

    const std::vector<HostActionId>& find(RouteId route) const {
        static const std::vector<HostActionId> empty;
        auto it = byRoute_.find(route);
        return it == byRoute_.end() ? empty : it->second;
    }

   private:
    std::uint64_t next_ = 0;
    std::map<HostActionId, HostAction> actions_;
    std::map<ScheduleStepId, std::vector<HostActionId>> byStep_;
    std::map<RouteId, std::vector<HostActionId>> byRoute_;
};

/*
 * Scalars span two storage domains:
 * - unbound values live only in the shared host map;
 * - bound values are read from their owner's physical slot;
 * - publishing updates the host snapshot, then any bound physical slot.
 * One mutex serializes CPU users and bridge callbacks across those domains.
 */
class BackendRuntimeState {
   public:
    using ScalarBindings =
        std::map<std::pair<DeviceId, std::string>, BackendScalarId>;
    using OwnerAccess =
        std::map<DeviceId, std::shared_ptr<IDeviceResourceAccess>>;

    BackendRuntimeState(
        std::shared_ptr<std::map<std::string, std::uint64_t>> scalarValues,
        ScalarBindings scalarBindings = {},
        OwnerAccess ownerAccess = {})
        : scalarValues_(std::move(scalarValues)),
          scalarBindings_(std::move(scalarBindings)),
          ownerAccess_(std::move(ownerAccess)) {
        if (!scalarValues_) {
            scalarValues_ =
                std::make_shared<std::map<std::string, std::uint64_t>>();
        }
    }

    /**
     * @brief Shared scalar storage.
     *
     * Callers that access the map directly must hold @ref scalarMutex.
     */
    const std::shared_ptr<std::map<std::string, std::uint64_t>>&
    scalarValues() const {
        return scalarValues_;
    }

    std::mutex& scalarMutex() const { return scalarMutex_; }

    void addOwnerAccess(
        DeviceId owner, std::shared_ptr<IDeviceResourceAccess> access) {
        ownerAccess_[std::move(owner)] = std::move(access);
    }

    const IDeviceResourceAccess& access(DeviceId owner) const {
        auto it = ownerAccess_.find(owner);
        if (it == ownerAccess_.end() || !it->second) {
            throw std::runtime_error(
                "BackendRuntimeState: resource owner '" + owner.value() +
                "' has no host access binding");
        }
        return *it->second;
    }

    /*
     * A leased device slot is authoritative when present. Otherwise the
     * shared host value is the latest published scalar snapshot.
     */
    std::uint64_t readScalar(DeviceId owner, const std::string& key) const {
        std::lock_guard<std::mutex> lock(scalarMutex_);
        auto binding = scalarBindings_.find({owner, key});
        if (binding != scalarBindings_.end()) {
            return access(owner).readScalar(binding->second);
        }
        auto value = scalarValues_->find(key);
        if (value == scalarValues_->end()) {
            throw std::runtime_error(
                "BackendRuntimeState: unknown scalar '" + key + "'");
        }
        return value->second;
    }

    /*
     * Publish to host storage and the owner's leased slot under one host-side
     * critical section so concurrent bridge callbacks cannot interleave.
     */
    void writeScalar(
        DeviceId owner, const std::string& key, std::uint64_t bits) {
        std::lock_guard<std::mutex> lock(scalarMutex_);
        (*scalarValues_)[key] = bits;
        auto binding = scalarBindings_.find({owner, key});
        if (binding != scalarBindings_.end()) {
            access(owner).writeScalar(binding->second, bits);
        }
    }

   private:
    std::shared_ptr<std::map<std::string, std::uint64_t>> scalarValues_;
    ScalarBindings scalarBindings_;
    OwnerAccess ownerAccess_;
    mutable std::mutex scalarMutex_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_BACKEND_RUNTIME_HPP
