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

#include <vrt/graph/device/fpga/vbin_spec.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <vrt/parser/xml_parser.hpp>
#include <vrt/vrtbin.hpp>

namespace vrt::graph::fpga {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

ScalarType scalarTypeFromArg(const FunctionalArg& arg) {
    const std::string t = lower(arg.type);
    const std::uint32_t bits = (arg.range == 0) ? 32u : arg.range;
    if (t.find("float") != std::string::npos && bits <= 32) return ScalarType::F32;
    if (t.find("double") != std::string::npos || (t.find("float") != std::string::npos && bits > 32)) {
        return ScalarType::F64;
    }
    const bool isSigned = t.find("int") != std::string::npos &&
                          t.find("uint") == std::string::npos &&
                          t.find("unsigned") == std::string::npos;
    if (isSigned) {
        if (bits <= 8) return ScalarType::I8;
        if (bits <= 16) return ScalarType::I16;
        if (bits <= 32) return ScalarType::I32;
        return ScalarType::I64;
    }
    if (bits <= 8) return ScalarType::U8;
    if (bits <= 16) return ScalarType::U16;
    if (bits <= 32) return ScalarType::U32;
    return ScalarType::U64;
}

std::uint32_t r5AddressFor(std::uint64_t hostBase,
                           std::uint64_t userRegionHostBase,
                           std::uint32_t userRegionR5Base) {
    if (hostBase < userRegionHostBase) {
        throw std::runtime_error(
            "FpgaVbinSpec: kernel base address is below the user-region base");
    }
    const std::uint64_t offset = hostBase - userRegionHostBase;
    const std::uint64_t r5 = static_cast<std::uint64_t>(userRegionR5Base) + offset;
    if (r5 > UINT32_MAX) {
        throw std::runtime_error("FpgaVbinSpec: computed R5 address exceeds 32 bits");
    }
    return static_cast<std::uint32_t>(r5);
}

std::vector<std::uint8_t> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("FpgaVbinSpec: cannot open PDI file '" + path + "'");
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

}  // namespace

IOTypeMap ioTypeMapFromFunctionalArgs(const std::vector<FunctionalArg>& args) {
    IOTypeMap ioType;
    for (const FunctionalArg& arg : args) {
        const std::string type = lower(arg.type);
        const bool registerOutput =
            arg.readable && !arg.writable && arg.port.empty();
        if (!registerOutput &&
            (type == "buffer" || type.find('*') != std::string::npos)) {
            BufferPort port{arg.name, BufferType::U8};
            // system_map does not currently distinguish buffer direction
            // precisely enough for graph tokens. Treat readable+writable
            // buffers as RW and otherwise use the write flag as "kernel
            // consumes this pointer" to match HLS AXI-Lite address args.
            if (arg.readable && !arg.writable) {
                ioType.outputs.push_back(port);
            } else if (arg.readable && arg.writable) {
                ioType.inouts.push_back(RWBufferPort{port, BufferPort{arg.name + "_out", port.type}});
            } else {
                ioType.inputs.push_back(port);
            }
            continue;
        }

        ScalarPort port{arg.name, scalarTypeFromArg(arg)};
        if (arg.readable && !arg.writable) {
            ioType.outputScalars.push_back(port);
        } else {
            ioType.inputScalars.push_back(port);
        }
    }
    return ioType;
}

FpgaKernelSpec fpgaKernelSpecFromKernel(const Kernel& kernel,
                                        std::uint64_t userRegionHostBase,
                                        std::uint32_t userRegionR5Base) {
    FpgaKernelSpec spec;
    spec.name = kernel.getName();
    spec.system_map_base_addr = kernel.getPhysAddr();
    spec.r5_base_addr = r5AddressFor(spec.system_map_base_addr,
                                     userRegionHostBase,
                                     userRegionR5Base);
    const auto& functionalArgs = kernel.getFunctionalArgs();
    spec.ioType = ioTypeMapFromFunctionalArgs(functionalArgs);
    spec.args.reserve(functionalArgs.size());
    for (const FunctionalArg& arg : functionalArgs) {
        spec.args.push_back(FpgaKernelArgSpec{
            arg.idx,
            arg.name,
            arg.type,
            arg.offset,
            arg.range,
            arg.readable,
            arg.writable,
            arg.port});

        // Record the m_axi memory region for buffer arguments so the graph
        // backend can allocate kernel buffers where the kernel master can
        // actually reach them.  Scalars have no AXI port; buffer args without
        // a system_map <connection> are simply left unmapped (the backend
        // falls back to its BAR-window arena in that case).
        const std::string type = lower(arg.type);
        const bool isBuffer = (type == "buffer" || type.find('*') != std::string::npos);
        if (isBuffer && !arg.port.empty()) {
            try {
                spec.argMemory[arg.name] = kernel.argMemoryConfig(arg.name);
            } catch (const std::exception&) {
                // No connection recorded for this port; leave unmapped.
            }
        }
    }
    return spec;
}

FpgaImageSpec FpgaVbinSpec::loadImage(const std::string& imageId,
                                      const std::string& vbinPath,
                                      const std::string& bdf,
                                      std::uint64_t userRegionHostBase,
                                      std::uint32_t userRegionR5Base) {
    Vrtbin vbin(vbinPath, bdf);
    XMLParser parser(vbin.getSystemMapPath());
    parser.parseXML();

    FpgaImageSpec image;
    image.id = imageId;
    image.vbinPath = vbinPath;
    image.pdiPath = vbin.getPdiPath();
    image.pdiBytes = readFileBytes(image.pdiPath);
    image.platform = parser.getPlatform();

    auto kernels = parser.getKernels();
    for (const auto& [name, kernel] : kernels) {
        image.kernels.emplace(name,
                              fpgaKernelSpecFromKernel(kernel,
                                                       userRegionHostBase,
                                                       userRegionR5Base));
    }
    return image;
}

void FpgaVbinSpec::addImage(FpgaImageSpec image) {
    if (image.id.empty()) {
        throw std::invalid_argument("FpgaVbinSpec::addImage: image id must not be empty");
    }
    auto [it, inserted] = images_.emplace(image.id, std::move(image));
    if (!inserted) {
        throw std::invalid_argument("FpgaVbinSpec::addImage: duplicate image id '" +
                                    it->first + "'");
    }
}

const FpgaImageSpec& FpgaVbinSpec::image(const std::string& imageId) const {
    auto it = images_.find(imageId);
    if (it == images_.end()) {
        throw std::out_of_range("FpgaVbinSpec: unknown image '" + imageId + "'");
    }
    return it->second;
}

const FpgaKernelSpec& FpgaVbinSpec::kernel(const std::string& imageId,
                                           const std::string& kernelName) const {
    const FpgaImageSpec& img = image(imageId);
    auto it = img.kernels.find(kernelName);
    if (it == img.kernels.end()) {
        throw std::out_of_range("FpgaVbinSpec: image '" + imageId +
                                "' has no kernel '" + kernelName + "'");
    }
    return it->second;
}

bool FpgaVbinSpec::hasImage(const std::string& imageId) const {
    return images_.count(imageId) != 0;
}

std::string FpgaVbinSpec::defaultImageId() const {
    if (images_.empty()) return {};
    return images_.begin()->first;
}

}  // namespace vrt::graph::fpga
