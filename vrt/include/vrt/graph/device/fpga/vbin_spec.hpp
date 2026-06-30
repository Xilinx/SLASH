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

#ifndef VRT_GRAPH_DEVICE_FPGA_VBIN_SPEC_HPP
#define VRT_GRAPH_DEVICE_FPGA_VBIN_SPEC_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <vrt/allocator/allocator.hpp>
#include <vrt/kernel.hpp>
#include <vrt/utils/platform.hpp>
#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

namespace vrt::graph::fpga {

constexpr std::uint64_t kDefaultUserRegionHostBase = 0x020200000000ULL;
constexpr std::uint32_t kDefaultUserRegionR5Base   = 0x88000000u;

struct FpgaKernelArgSpec {
    std::uint32_t idx = 0;
    std::string   name;
    std::string   type;
    std::uint32_t offset = 0;
    std::uint32_t range = 32;
    bool          readable = false;
    bool          writable = false;
    std::string   port;
};

struct FpgaKernelSpec {
    std::string name;
    std::uint64_t system_map_base_addr = 0;
    std::uint32_t r5_base_addr = 0;
    IOTypeMap ioType;
    std::vector<FpgaKernelArgSpec> args;
    /// Memory region (DDR/HBM/HBM_VNOC) each buffer argument's m_axi port is
    /// wired to, from the system_map <connection> entries.  Keyed by arg name
    /// (same key as @ref args).  Absent for args without a port/connection.
    std::map<std::string, ::vrt::MemoryConfig> argMemory;

    KernelDescriptor descriptor(const std::string& imageId) const {
        return KernelDescriptor{name, DeviceType::FPGA, imageId, ioType};
    }
};

struct FpgaImageSpec {
    std::string id;
    std::string vbinPath;
    std::string pdiPath;
    std::vector<std::uint8_t> pdiBytes;
    Platform platform = Platform::UNKNOWN;
    std::map<std::string, FpgaKernelSpec> kernels;
};

class FpgaVbinSpec {
   public:
    FpgaVbinSpec() = default;

    static FpgaImageSpec loadImage(const std::string& imageId,
                                   const std::string& vbinPath,
                                   const std::string& bdf,
                                   std::uint64_t userRegionHostBase = kDefaultUserRegionHostBase,
                                   std::uint32_t userRegionR5Base = kDefaultUserRegionR5Base);

    void addImage(FpgaImageSpec image);
    const FpgaImageSpec& image(const std::string& imageId) const;
    const FpgaKernelSpec& kernel(const std::string& imageId,
                                 const std::string& kernelName) const;
    bool hasImage(const std::string& imageId) const;
    bool empty() const noexcept { return images_.empty(); }
    std::string defaultImageId() const;
    const std::map<std::string, FpgaImageSpec>& images() const noexcept { return images_; }

   private:
    std::map<std::string, FpgaImageSpec> images_;
};

IOTypeMap ioTypeMapFromFunctionalArgs(const std::vector<FunctionalArg>& args);
FpgaKernelSpec fpgaKernelSpecFromKernel(const Kernel& kernel,
                                        std::uint64_t userRegionHostBase =
                                            kDefaultUserRegionHostBase,
                                        std::uint32_t userRegionR5Base =
                                            kDefaultUserRegionR5Base);

}  // namespace vrt::graph::fpga

#endif  // VRT_GRAPH_DEVICE_FPGA_VBIN_SPEC_HPP
