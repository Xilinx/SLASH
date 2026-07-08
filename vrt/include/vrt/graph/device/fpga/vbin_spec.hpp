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
 * @file vbin_spec.hpp
 * @brief Graph-facing FPGA image metadata extracted from a vbin archive.
 *
 * The Graph FPGA backend uses this layer to translate classic VRT kernel
 * metadata (functional arguments, AXI-Lite base addresses, memory-port
 * connectivity, and PDI contents) into the image/kernel descriptors needed by
 * `FpgaDevice` and the RP1 submitter.
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

/** @brief Default host-visible base address of the exclusive user region. */
constexpr std::uint64_t kDefaultUserRegionHostBase = 0x020200000000ULL;
/** @brief Default RP1/R5-visible base address corresponding to the user region. */
constexpr std::uint32_t kDefaultUserRegionR5Base   = 0x88000000u;

/**
 * @brief One AXI-Lite or memory-mapped argument parsed from kernel metadata.
 */
struct FpgaKernelArgSpec {
    /** Positional argument index from the HLS kernel signature. */
    std::uint32_t idx = 0;
    /** Functional argument name from vbin metadata. */
    std::string   name;
    /** Functional argument type string from vbin metadata. */
    std::string   type;
    /** AXI-Lite register offset in the kernel control aperture. */
    std::uint32_t offset = 0;
    /** Register width in bits. */
    std::uint32_t range = 32;
    /** Whether the host/RP1 may read the argument register. */
    bool          readable = false;
    /** Whether the host/RP1 may write the argument register. */
    bool          writable = false;
    /** AXI memory port name for buffer args; empty for scalar-only args. */
    std::string   port;
};

/**
 * @brief One FPGA kernel instance within a loaded graph image.
 */
struct FpgaKernelSpec {
    /** Kernel instance name, e.g. "increment_0". */
    std::string name;
    /** Host-view base address from the vbin system map. */
    std::uint64_t system_map_base_addr = 0;
    /** RP1/R5-view base address computed from the user-region mapping. */
    std::uint32_t r5_base_addr = 0;
    /** Graph-visible scalar and buffer port types. */
    IOTypeMap ioType;
    /** Parsed functional argument metadata in signature order. */
    std::vector<FpgaKernelArgSpec> args;
    /// Memory region (DDR/HBM/HBM_VNOC) each buffer argument's m_axi port is
    /// wired to, from the system_map connection entries.  Keyed by arg name
    /// (same key as @ref args).  Absent for args without a port/connection.
    std::map<std::string, ::vrt::MemoryConfig> argMemory;

    /**
     * @brief Build the device-independent kernel handle for this image.
     *
     * @param imageId  Graph image id containing this kernel.
     */
    KernelDescriptor descriptor(const std::string& imageId) const {
        return KernelDescriptor{name, DeviceType::FPGA, imageId, ioType};
    }
};

/**
 * @brief One loadable FPGA image and the kernels it contains.
 */
struct FpgaImageSpec {
    /** User-facing image id registered with the graph. */
    std::string id;
    /** Path to the source vbin archive. */
    std::string vbinPath;
    /** Extracted PDI file path used by the reprogram op. */
    std::string pdiPath;
    /** PDI contents staged into DDR for RP1 PDI_LOAD. */
    std::vector<std::uint8_t> pdiBytes;
    /** Platform encoded in the vbin, normally Platform::HARDWARE for RP1. */
    Platform platform = Platform::UNKNOWN;
    /** Kernel specs keyed by kernel instance name. */
    std::map<std::string, FpgaKernelSpec> kernels;
};

/**
 * @brief Collection of named FPGA images available to one graph FPGA device.
 */
class FpgaVbinSpec {
   public:
    FpgaVbinSpec() = default;

    /**
     * @brief Extract and parse a vbin archive into a graph image spec.
     *
     * @param imageId             User-facing graph image id.
     * @param vbinPath            Path to the vbin archive.
     * @param bdf                 Target board BDF used by classic VRT metadata loading.
     * @param userRegionHostBase  Host-visible base of the exclusive user region.
     * @param userRegionR5Base    RP1/R5-visible base corresponding to that region.
     * @return                    Parsed image metadata and PDI bytes.
     * @throws std::runtime_error On vbin extraction, metadata, or kernel parsing errors.
     */
    static FpgaImageSpec loadImage(const std::string& imageId,
                                   const std::string& vbinPath,
                                   const std::string& bdf,
                                   std::uint64_t userRegionHostBase = kDefaultUserRegionHostBase,
                                   std::uint32_t userRegionR5Base = kDefaultUserRegionR5Base);

    /**
     * @brief Register a parsed image in this spec.
     *
     * @throws std::invalid_argument If another image already uses the same id.
     */
    void addImage(FpgaImageSpec image);

    /**
     * @brief Look up a registered image by id.
     *
     * @throws std::out_of_range If @p imageId is unknown.
     */
    const FpgaImageSpec& image(const std::string& imageId) const;

    /**
     * @brief Look up a kernel inside a registered image.
     *
     * @throws std::out_of_range If the image or kernel name is unknown.
     */
    const FpgaKernelSpec& kernel(const std::string& imageId,
                                 const std::string& kernelName) const;

    /**
     * @brief Return true if an image with @p imageId is registered.
     */
    bool hasImage(const std::string& imageId) const;

    /**
     * @brief Return whether no images have been registered.
     */
    bool empty() const noexcept { return images_.empty(); }

    /**
     * @brief Return the sole image id when exactly one image is registered.
     *
     * @throws std::runtime_error If there are zero images or more than one image.
     */
    std::string defaultImageId() const;

    /**
     * @brief Return all registered images keyed by image id.
     */
    const std::map<std::string, FpgaImageSpec>& images() const noexcept { return images_; }

   private:
    std::map<std::string, FpgaImageSpec> images_;
};

/**
 * @brief Convert classic VRT functional arguments into graph IO type metadata.
 */
IOTypeMap ioTypeMapFromFunctionalArgs(const std::vector<FunctionalArg>& args);

/**
 * @brief Convert a loaded classic VRT Kernel into a graph FPGA kernel spec.
 *
 * @param kernel              Loaded VRT kernel metadata source.
 * @param userRegionHostBase  Host-visible base of the exclusive user region.
 * @param userRegionR5Base    RP1/R5-visible base corresponding to that region.
 */
FpgaKernelSpec fpgaKernelSpecFromKernel(const Kernel& kernel,
                                        std::uint64_t userRegionHostBase =
                                            kDefaultUserRegionHostBase,
                                        std::uint32_t userRegionR5Base =
                                            kDefaultUserRegionR5Base);

}  // namespace vrt::graph::fpga

#endif  // VRT_GRAPH_DEVICE_FPGA_VBIN_SPEC_HPP
