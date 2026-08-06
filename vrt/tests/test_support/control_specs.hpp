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
 * @file control_specs.hpp
 * @brief Shared test helpers for building KernelDescriptors and control-flow
 *        Specs (::vrt::graph::detail::LoopRecord / ::vrt::graph::detail::ConditionalRecord) without repeating the same
 *        boilerplate in every test file. Header-only; intended for use only
 *        from inside `vrt/tests/`.
 */

#ifndef VRT_TESTS_TEST_SUPPORT_CONTROL_SPECS_HPP
#define VRT_TESTS_TEST_SUPPORT_CONTROL_SPECS_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <atomic>
#include <utility>
#include <vector>

#include <vrt/graph/control/condition.hpp>
#include <vrt/graph/control/control_node.hpp>
#include <vrt/graph/detail/authoring_region.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/detail/port_bindings.hpp>
#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

namespace vrt::graph::test_support {

namespace detail {
inline std::string nextTripCountName() {
    static std::atomic<std::uint64_t> counter{0};
    return "__trip_count_" + std::to_string(counter++);
}
}  // namespace detail

inline GraphScalar tripCountScalar(::vrt::graph::detail::AuthoringRegion& region,
                                   ScalarType type = ScalarType::I32,
                                   std::string name = {}) {
    if (name.empty()) name = detail::nextTripCountName();
    return region.scalar(type, std::move(name));
}

inline LoopTripCount tripCount(GraphScalar scalar) {
    return LoopTripCount::scalar(scalar);
}

inline KernelDescriptor cpuKernel(std::string name, IOTypeMap ioType = {}) {
    return KernelDescriptor{std::move(name), DeviceType::CPU, std::nullopt,
                            std::move(ioType)};
}

inline KernelDescriptor mockCpuKernel(std::string name, IOTypeMap ioType = {}) {
    return KernelDescriptor{std::move(name), DeviceType::MOCK_CPU, std::nullopt,
                            std::move(ioType)};
}

inline ::vrt::graph::detail::LoopRecord fixedLoopRecord(LoopTripCount tripCount,
                              std::shared_ptr<::vrt::graph::detail::AuthoringRegion> body,
                              std::vector<std::string> afterOps = {}) {
    ::vrt::graph::detail::LoopRecord spec;
    spec.tripCount = std::move(tripCount);
    spec.body = std::move(body);
    spec.afterOps = std::move(afterOps);
    return spec;
}

inline ::vrt::graph::detail::LoopRecord fixedLoopRecord(IOTypeMap ioType, ::vrt::graph::detail::PortBindings ioMap,
                              LoopTripCount tripCount,
                              std::shared_ptr<::vrt::graph::detail::AuthoringRegion> body,
                              std::vector<std::string> afterOps = {}) {
    ::vrt::graph::detail::LoopRecord spec;
    spec.ioType = std::move(ioType);
    spec.ioMap = std::move(ioMap);
    spec.tripCount = std::move(tripCount);
    spec.body = std::move(body);
    spec.afterOps = std::move(afterOps);
    return spec;
}

inline ::vrt::graph::detail::LoopRecord whileLoopRecord(Condition condition,
                              std::shared_ptr<::vrt::graph::detail::AuthoringRegion> body,
                              std::vector<std::string> afterOps = {}) {
    ::vrt::graph::detail::LoopRecord spec;
    spec.kind = LoopKind::WhileCondition;
    spec.condition = std::move(condition);
    spec.body = std::move(body);
    spec.afterOps = std::move(afterOps);
    return spec;
}

inline ::vrt::graph::detail::LoopRecord whileLoopRecord(IOTypeMap ioType, ::vrt::graph::detail::PortBindings ioMap, Condition condition,
                              std::shared_ptr<::vrt::graph::detail::AuthoringRegion> body,
                              std::vector<std::string> afterOps = {}) {
    ::vrt::graph::detail::LoopRecord spec;
    spec.kind = LoopKind::WhileCondition;
    spec.ioType = std::move(ioType);
    spec.ioMap = std::move(ioMap);
    spec.condition = std::move(condition);
    spec.body = std::move(body);
    spec.afterOps = std::move(afterOps);
    return spec;
}

inline ::vrt::graph::detail::ConditionalRecord ifElseSpec(Condition condition,
                                  std::shared_ptr<::vrt::graph::detail::AuthoringRegion> thenRegion,
                                  std::shared_ptr<::vrt::graph::detail::AuthoringRegion> elseRegion,
                                  std::vector<std::string> afterOps = {}) {
    ::vrt::graph::detail::ConditionalRecord spec;
    spec.condition = std::move(condition);
    spec.thenRegion = std::move(thenRegion);
    spec.elseRegion = std::move(elseRegion);
    spec.afterOps = std::move(afterOps);
    return spec;
}

inline ::vrt::graph::detail::ConditionalRecord ifElseSpec(IOTypeMap ioType, ::vrt::graph::detail::PortBindings ioMap, Condition condition,
                                  std::shared_ptr<::vrt::graph::detail::AuthoringRegion> thenRegion,
                                  std::shared_ptr<::vrt::graph::detail::AuthoringRegion> elseRegion,
                                  std::vector<std::string> afterOps = {}) {
    ::vrt::graph::detail::ConditionalRecord spec;
    spec.ioType = std::move(ioType);
    spec.ioMap = std::move(ioMap);
    spec.condition = std::move(condition);
    spec.thenRegion = std::move(thenRegion);
    spec.elseRegion = std::move(elseRegion);
    spec.afterOps = std::move(afterOps);
    return spec;
}

}  // namespace vrt::graph::test_support

#endif  // VRT_TESTS_TEST_SUPPORT_CONTROL_SPECS_HPP
