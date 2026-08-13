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

#include <vrt/graph/transfer.hpp>

#include <map>
#include <memory>
#include <string>
#include <utility>

#include <vrt/graph/device/device.hpp>

namespace vrt::graph {

TransferCapabilityCatalog TransferCapabilityCatalog::fromGraph(
    const std::map<std::string, std::shared_ptr<IDevice>>& devices,
    const std::map<std::pair<DeviceType, DeviceType>,
                   BridgeFactory>& bridgeFactories) {
    TransferCapabilityCatalog result;
    for (const auto& [name, device] : devices) {
        if (!device) continue;
        const DeviceId id(name);
        const DeviceCapabilities capabilities =
            device->compilerCapabilities();
        if (capabilities.hostsGraphIo) {
            if (!result.host_) result.host_ = id;
        }
        if (capabilities.supportsMemoryRegionCopies) {
            result.memoryRegionCopyDevices_.insert(id);
        }
        if (capabilities.ownsRendezvousNamespace) {
            result.rendezvousOwners_.insert(id);
        }
    }

    for (const auto& [sourceName, source] : devices) {
        if (!source) continue;
        for (const auto& [destinationName, destination] : devices) {
            if (!destination || sourceName == destinationName) continue;
            if (bridgeFactories.count(
                    {source->type(), destination->type()}) != 0) {
                result.direct_.insert(
                    {DeviceId(sourceName), DeviceId(destinationName)});
            }
        }
    }
    return result;
}

bool TransferCapabilityCatalog::hasDirect(
    DeviceId source, DeviceId destination) const {
    return direct_.count({std::move(source), std::move(destination)}) != 0;
}

bool TransferCapabilityCatalog::supportsMemoryRegionCopies(
    DeviceId device) const {
    return memoryRegionCopyDevices_.count(std::move(device)) != 0;
}

bool TransferCapabilityCatalog::ownsRendezvousNamespace(
    DeviceId device) const {
    return rendezvousOwners_.count(std::move(device)) != 0;
}

}  // namespace vrt::graph
