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
 * @file fpga.hpp
 * @brief FPGA bring-up handles for the vrt::graph authoring API.
 *
 * `Graph::addFpga(FpgaSpec)` folds QDMA PDI staging, vbin/image loading, the
 * vrtd session + BAR window, the RP1 readiness preflight, and FpgaDevice
 * construction into one call, returning an `FpgaHandle`. Named `FpgaImageHandle`s
 * are pulled from it; each yields typed FPGA kernel handles and converts to an
 * `ImageRef` for reprogram nodes. The user region starts with no active image,
 * so every FPGA dispatch must be gated behind a reprogram of its image.
 */

#ifndef VRT_GRAPH_AUTHORING_FPGA_HPP
#define VRT_GRAPH_AUTHORING_FPGA_HPP

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/authoring/calls.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/device/fpga/vbin_spec.hpp>
#include <vrt/graph/device/fpga_device.hpp>
#include <vrt/graph/node/io_type_map.hpp>

namespace vrt::graph {

/** @brief One named image to load: a user label and its vbin path. */
struct FpgaImageSource {
    std::string name;
    std::string vbinPath;
};

/** @brief Parameters for Graph::addFpga(). */
struct FpgaSpec {
    std::string                  bdf;
    std::string                  socket = "/run/vrtd.sock";
    std::vector<FpgaImageSource> images;
    std::chrono::milliseconds    waitTimeout{30000};
    std::string                  deviceId = "fpga:0";
};

/**
 * @brief Fluent builder for an FPGA kernel handle.
 *
 * `.scalarIn/.in/.out/.inout` attach element types to named ports; the port
 * order (scalars, then inputs, then outputs) matches the RP1 scalar-first ABI.
 */
class FpgaKernelBuilder {
   public:
    FpgaKernelBuilder(std::string name, std::string image, std::string deviceId)
        : name_(std::move(name)), image_(std::move(image)), deviceId_(std::move(deviceId)) {}

    template <class T>
    FpgaKernelBuilder& scalarIn(std::string n) { io_.scalarIn<T>(std::move(n)); return *this; }
    template <class T>
    FpgaKernelBuilder& scalarOut(std::string n) { io_.scalarOut<T>(std::move(n)); return *this; }
    template <class T>
    FpgaKernelBuilder& in(std::string n) { io_.in<T>(std::move(n)); return *this; }
    template <class T>
    FpgaKernelBuilder& out(std::string n) { io_.out<T>(std::move(n)); return *this; }
    template <class T>
    FpgaKernelBuilder& inout(std::string n) { io_.inout<T>(std::move(n)); return *this; }

    KernelHandle handle() const {
        return KernelHandle{name_, DeviceType::FPGA, image_, io_, deviceId_};
    }
    operator KernelHandle() const { return handle(); }

   private:
    std::string name_;
    std::string image_;
    std::string deviceId_;
    IOTypeMap   io_;
};

/**
 * @brief Handle to one loaded image; mints kernel handles and reprogram refs.
 */
class FpgaImageHandle {
   public:
    FpgaImageHandle() = default;
    FpgaImageHandle(std::string imageId, std::string pdiPath, std::string deviceId)
        : imageId_(std::move(imageId)),
          pdiPath_(std::move(pdiPath)),
          deviceId_(std::move(deviceId)) {}

    /** @brief Build a typed FPGA kernel handle for @p kernelName in this image. */
    FpgaKernelBuilder kernel(const std::string& kernelName) const {
        return FpgaKernelBuilder(kernelName, imageId_, deviceId_);
    }

    const std::string& id() const { return imageId_; }
    const std::string& pdiPath() const { return pdiPath_; }

    operator ImageRef() const { return ImageRef{imageId_, pdiPath_, deviceId_}; }

   private:
    std::string imageId_;
    std::string pdiPath_;
    std::string deviceId_;
};

/**
 * @brief Owning handle returned by Graph::addFpga().
 *
 * Keeps the vrtd session, PDI staging device, vbin spec, and FpgaDevice alive
 * for the graph's lifetime and exposes named image handles.
 */
class FpgaHandle {
   public:
    /** @brief Look up a loaded image by its user-assigned name. */
    FpgaImageHandle image(const std::string& name) const {
        auto it = images_.find(name);
        if (it == images_.end()) {
            throw std::invalid_argument("FpgaHandle::image: unknown image '" + name + "'");
        }
        return it->second;
    }

    const std::shared_ptr<FpgaDevice>& device() const { return device_; }
    const std::string& deviceId() const { return deviceId_; }

   private:
    friend class Graph;

    std::string                              deviceId_;
    std::shared_ptr<FpgaDevice>              device_;
    std::shared_ptr<fpga::FpgaVbinSpec>      spec_;
    std::map<std::string, FpgaImageHandle>   images_;
    std::vector<std::shared_ptr<void>>       keepAlive_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_AUTHORING_FPGA_HPP
