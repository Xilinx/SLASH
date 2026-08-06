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
 * @file ids.hpp
 * @brief Strong compiler identity types.
 */

#ifndef VRT_GRAPH_IDS_HPP
#define VRT_GRAPH_IDS_HPP

#include <cstdint>
#include <ostream>
#include <string>
#include <utility>

namespace vrt::graph {

template <class Tag>
class NumericId {
   public:
    constexpr NumericId() = default;
    explicit constexpr NumericId(std::uint64_t value) : value_(value) {}

    constexpr std::uint64_t value() const { return value_; }

    friend constexpr bool operator==(NumericId lhs, NumericId rhs) {
        return lhs.value_ == rhs.value_;
    }

    friend constexpr bool operator!=(NumericId lhs, NumericId rhs) {
        return !(lhs == rhs);
    }

    friend constexpr bool operator<(NumericId lhs, NumericId rhs) {
        return lhs.value_ < rhs.value_;
    }

    friend constexpr bool operator>(NumericId lhs, NumericId rhs) {
        return rhs < lhs;
    }

    friend constexpr bool operator<=(NumericId lhs, NumericId rhs) {
        return !(rhs < lhs);
    }

    friend constexpr bool operator>=(NumericId lhs, NumericId rhs) {
        return !(lhs < rhs);
    }

   private:
    std::uint64_t value_ = 0;
};

template <class Tag>
std::ostream& operator<<(std::ostream& out, NumericId<Tag> id) {
    return out << id.value();
}

template <class Tag>
class NamedId {
   public:
    NamedId() = default;
    explicit NamedId(std::string value) : value_(std::move(value)) {}

    const std::string& value() const { return value_; }
    bool empty() const { return value_.empty(); }

    friend bool operator==(const NamedId& lhs, const NamedId& rhs) {
        return lhs.value_ == rhs.value_;
    }

    friend bool operator!=(const NamedId& lhs, const NamedId& rhs) {
        return !(lhs == rhs);
    }

    friend bool operator<(const NamedId& lhs, const NamedId& rhs) {
        return lhs.value_ < rhs.value_;
    }

   private:
    std::string value_;
};

template <class Tag>
std::ostream& operator<<(std::ostream& out, const NamedId<Tag>& id) {
    return out << id.value();
}

struct RegionIdTag;
struct AuthoredScopeIdTag;
struct NodeIdTag;
struct ValueIdTag;
struct StorageIdTag;
struct ReplicaIdTag;
struct RouteIdTag;
struct TransferLegIdTag;
struct QueueIdTag;
struct ScheduleStepIdTag;
struct RendezvousIdTag;
struct ScalarResourceIdTag;
struct BackendResourceIdTag;
struct BackendScalarIdTag;
struct HostActionIdTag;
struct DeviceIdTag;
struct MemoryRegionIdTag;
struct PortNameTag;

using RegionId = NumericId<RegionIdTag>;
using AuthoredScopeId = NumericId<AuthoredScopeIdTag>;
using NodeId = NumericId<NodeIdTag>;
using ValueId = NumericId<ValueIdTag>;
using StorageId = NumericId<StorageIdTag>;
using ReplicaId = NumericId<ReplicaIdTag>;
using RouteId = NumericId<RouteIdTag>;
using TransferLegId = NumericId<TransferLegIdTag>;
using QueueId = NumericId<QueueIdTag>;
using ScheduleStepId = NumericId<ScheduleStepIdTag>;
using RendezvousId = NumericId<RendezvousIdTag>;
using ScalarResourceId = NumericId<ScalarResourceIdTag>;
using BackendResourceId = NumericId<BackendResourceIdTag>;
using BackendScalarId = NumericId<BackendScalarIdTag>;
using HostActionId = NumericId<HostActionIdTag>;
using DeviceId = NamedId<DeviceIdTag>;
using MemoryRegionId = NamedId<MemoryRegionIdTag>;
using PortName = NamedId<PortNameTag>;

}  // namespace vrt::graph

#endif  // VRT_GRAPH_IDS_HPP
