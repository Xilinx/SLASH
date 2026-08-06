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

#include <vrt/graph/graph.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>

#include <vrtd/bar.hpp>
#include <vrtd/device.hpp>
#include <vrtd/session.hpp>

#include <vrt/device.hpp>
#include <vrt/graph/crossdevice/cpu_fpga_bridge.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/fpga/rp1_bar_window.hpp>
#include <vrt/graph/device/fpga/rp1_submitter.hpp>
#include <vrt/graph/device/fpga/vbin_spec.hpp>
#include <vrt/graph/device/fpga_device.hpp>

#if defined(VRT_HAS_GPU) && (VRT_HAS_GPU == 1)
#include <vrt/graph/crossdevice/cpu_gpu_bridge.hpp>
#endif

namespace vrt::graph {

Graph Graph::withDefaults() {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    graph.registerBridgeFactory(DeviceType::CPU, DeviceType::FPGA,
                                CpuFpgaBridgeFactory());
    graph.registerBridgeFactory(DeviceType::FPGA, DeviceType::CPU,
                                CpuFpgaBridgeFactory());

#if defined(VRT_HAS_GPU) && (VRT_HAS_GPU == 1)
    graph.registerBridgeFactory(DeviceType::CPU, DeviceType::GPU,
                                CpuGpuBridgeFactory());
    graph.registerBridgeFactory(DeviceType::GPU, DeviceType::CPU,
                                CpuGpuBridgeFactory());
#endif

    return graph;
}

FpgaHandle Graph::addFpga(const FpgaSpec& spec) {
    if (spec.images.empty()) {
        throw std::invalid_argument("Graph::addFpga: at least one image is required");
    }
    if (spec.bdf.empty()) {
        throw std::invalid_argument("Graph::addFpga: bdf must not be empty");
    }

    // QDMA PDI staging device (first vbin is enough to open the device; the
    // staging path only moves partial PDIs into DDR).
    auto staging = std::make_shared<::vrt::Device>(spec.bdf, spec.images.front().vbinPath,
                                                   /*program=*/false);

    // Load each named image's kernel catalog and partial PDI.
    auto vbinSpec = std::make_shared<fpga::FpgaVbinSpec>();
    for (const auto& image : spec.images) {
        vbinSpec->addImage(fpga::FpgaVbinSpec::loadImage(image.name, image.vbinPath, spec.bdf));
    }

    // vrtd session + BAR4 window.
    auto session = std::make_shared<vrtd::Session>(spec.socket.c_str());
    vrtd::Device dev = session->getDeviceByBdf(spec.bdf);
    vrtd::BarFile barFile = dev.getBar(4).openBarFile();
    auto window = std::make_shared<fpga::Rp1BarWindow>(std::move(barFile));

    // RP1 firmware readiness preflight.
    fpga::Rp1Submitter preflight(*window);
    preflight.ensureReady(std::chrono::milliseconds{5000});

    // Construct the device with NO initial image: every FPGA dispatch must be
    // gated behind an explicit reprogram of its image.
    auto device = std::make_shared<FpgaDevice>(spec.deviceId, window, vbinSpec,
                                               /*initialImageId=*/std::string{});
    device->setWaitTimeout(spec.waitTimeout);
    device->setPdiStagingDevice(*staging);
    registerDevice(device);

    FpgaHandle handle;
    handle.deviceId_ = spec.deviceId;
    handle.device_   = device;
    handle.spec_     = vbinSpec;
    handle.keepAlive_.push_back(session);
    handle.keepAlive_.push_back(staging);
    handle.keepAlive_.push_back(window);
    for (const auto& image : spec.images) {
        const auto& imageSpec = vbinSpec->image(image.name);
        handle.images_.emplace(
            image.name,
            FpgaImageHandle(image.name, imageSpec.pdiPath, spec.deviceId));
    }
    return handle;
}

}  // namespace vrt::graph