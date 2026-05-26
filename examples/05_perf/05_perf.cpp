/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/kernel.hpp>
#include <vrt/utils/logger.hpp>

namespace {

constexpr std::size_t kTotalKernels = 76;
constexpr std::size_t kDefaultKernels = 2;
constexpr std::size_t kHbmKernels = 64;
constexpr std::size_t kMemKernels = 8;
constexpr std::size_t kDdrKernels = 4;
static_assert(kHbmKernels + kMemKernels + kDdrKernels == kTotalKernels,
              "Kernel group counts must match config.cfg");

constexpr std::uint32_t kPerfLength = 0x1000000u;
constexpr std::uint32_t kRepetitions = 1;
constexpr std::uint32_t kWriteMode = 0u;
constexpr std::uint32_t kReadMode = 1u;

constexpr std::uint32_t kOutAccDataOffset = 0x2cu;
constexpr std::uint32_t kOutAccCtrlOffset = 0x30u;

struct alignas(32) Word256 {
    std::uint32_t lane[8];
};

static_assert(sizeof(Word256) == 32, "Word256 must match 256-bit kernel data width");

std::uint32_t xorZeroToN(std::uint32_t n) {
    switch (n & 0x3u) {
        case 0u:
            return n;
        case 1u:
            return 1u;
        case 2u:
            return n + 1u;
        default:
            return 0u;
    }
}

double gibPerSecond(std::uint64_t bytes, std::chrono::nanoseconds elapsed) {
    if (elapsed.count() == 0) {
        return 0.0;
    }
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    return (static_cast<double>(bytes) / kGiB) /
           (static_cast<double>(elapsed.count()) / 1'000'000'000.0);
}

const char* memoryGroupName(std::size_t kernelIdx) {
    if (kernelIdx < kHbmKernels) {
        return "HBM";
    }
    if (kernelIdx < (kHbmKernels + kMemKernels)) {
        return "MEM";
    }
    return "DDR";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 5) {
        std::cerr << "Usages: " << argv[0] << " <BDF> <vrtbin file>" << std::endl
                  << "        " << argv[0] << " <BDF> <vrtbin file> <kernel count>" << std::endl
                  << "        " << argv[0] << " <BDF> <vrtbin file> <lower kernel index> <upper kernel index>" << std::endl;
        return 1;
    }

    const std::string bdf = argv[1];
    const std::string vrtbinFile = argv[2];

    std::size_t lowerKernelIndex = 0;
    std::size_t upperKernelIndex = kDefaultKernels;
    if (argc == 4) {
        try {
            upperKernelIndex = static_cast<std::size_t>(std::stoul(argv[3]));
        } catch (const std::exception&) {
            std::cerr << "Invalid kernel count: " << argv[3] << std::endl;
            return 1;
        }
        if (upperKernelIndex == 0 || upperKernelIndex > kTotalKernels) {
            std::cerr << "kernel count must be in [1, " << kTotalKernels << "]" << std::endl;
            return 1;
        }
    } else if (argc == 5) {
        try {
            lowerKernelIndex = static_cast<std::size_t>(std::stoul(argv[3]));
        } catch (const std::exception&) {
            std::cerr << "Invalid lower kernel index: " << argv[3] << std::endl;
        }
        if (lowerKernelIndex > kTotalKernels) {
            std::cerr << "kernel count must be in [0, " << kTotalKernels << ")" << std::endl;
            return 1;
        }
        try {
            upperKernelIndex = static_cast<std::size_t>(std::stoul(argv[4]));
        } catch (const std::exception&) {
            std::cerr << "Invalid upper kernel index: " << argv[4] << std::endl;
        }
        if (upperKernelIndex <= lowerKernelIndex || upperKernelIndex > kTotalKernels) {
            std::cerr << "kernel count must be in (" << lowerKernelIndex << ", " << kTotalKernels << "]" << std::endl;
            return 1;
        }
    }
    std::size_t kernelCount = upperKernelIndex - lowerKernelIndex;

    try {
        vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::INFO);

        const std::uint64_t bytesPerKernel = static_cast<std::uint64_t>(kPerfLength) * sizeof(Word256);
        const double bufferFootprintGiB =
            (static_cast<double>(bytesPerKernel) * static_cast<double>(kernelCount)) /
            (1024.0 * 1024.0 * 1024.0);

        std::cout << "VRT Version: " << vrt::getVersion() << std::endl;
        std::cout << "Launching " << kernelCount << " perf kernels" << std::endl;
        std::cout << "Per-kernel buffer size: " << (bytesPerKernel >> 20) << " MiB" << std::endl;
        std::cout << std::fixed << std::setprecision(2)
                  << "Aggregate buffer footprint: " << bufferFootprintGiB << " GiB" << std::endl;

        vrt::Device device(bdf, vrtbinFile);
        const bool isEmu = (device.getPlatform() == vrt::Platform::EMULATION) || (device.getPlatform() == vrt::Platform::SIMULATION);

        std::vector<vrt::Kernel> kernels;
        std::vector<vrt::Buffer<Word256>> buffers;
        kernels.reserve(kernelCount);
        buffers.reserve(kernelCount);
        for (std::size_t i = lowerKernelIndex; i < upperKernelIndex; ++i) {
            vrt::Kernel kernel(device, "perf_" + std::to_string(i));
            kernels.emplace_back(kernel);
            buffers.emplace_back(device, kPerfLength, kernel.argMemoryConfig("hbm_ptr"));
        }

        if (isEmu) {
            std::cout << "EMU pre-populating " << kernelCount
                      << " buffer(s) so tb.cpp has buffer mappings..." << std::endl;
            for (std::size_t i = 0; i < kernelCount; ++i) {
                std::size_t kernelIndex = lowerKernelIndex + i;
                if (kernelCount <= 4) {
                    std::cout << "  populate perf_" << kernelIndex << " (" << memoryGroupName(kernelIndex) << ")"
                              << std::endl;
                }
                buffers[i].sync(vrt::SyncType::HOST_TO_DEVICE);
            }
            std::cout << "EMU buffer pre-population complete" << std::endl;
        }

        auto runPhase = [&](std::uint32_t wr, const char* label) {
            std::cout << label << " phase: launching " << kernelCount << " kernel(s)" << std::endl;
            const auto tStart = std::chrono::high_resolution_clock::now();
            for (std::size_t i = 0; i < kernelCount; ++i) {
                std::size_t kernelIndex = lowerKernelIndex + i;
                if (isEmu && kernelCount <= 4) {
                    std::cout << "  " << label << " start perf_" << kernelIndex << "..." << std::endl;
                }
                kernels[i].setArg(0, buffers[i]);
                kernels[i].setArg(1, wr);
                kernels[i].setArg(2, kRepetitions);
                kernels[i].start();
                if (isEmu && kernelCount <= 4) {
                    std::cout << "  " << label << " start perf_" << kernelIndex << " returned" << std::endl;
                }
            }
            for (std::size_t i = 0; i < kernelCount; ++i) {
                kernels[i].wait();
            }
            const auto tEnd = std::chrono::high_resolution_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(tEnd - tStart);

            const std::uint64_t totalBytes = kRepetitions * bytesPerKernel * static_cast<std::uint64_t>(kernelCount);
            std::cout << label << " phase time: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                      << " ms";
            std::cout << " (" << std::fixed << std::setprecision(2)
                      << gibPerSecond(totalBytes, elapsed) << " GiB/s aggregate)" << std::endl;
            return elapsed;
        };

        const auto writeElapsed = runPhase(kWriteMode, "Write");
        const auto readElapsed = runPhase(kReadMode, "Read");

        std::size_t failures = 0;

        for (std::size_t i = 0; i < kernelCount; i++) {
            std::size_t kernelIndex = lowerKernelIndex + i;
            buffers[i].sync(vrt::SyncType::DEVICE_TO_HOST);
            for (std::size_t j = 0; j < kPerfLength; j++) {
                if (buffers[i][j].lane[0] != j) {
                    std::cerr << "Kernel perf_" << kernelIndex << " (" << memoryGroupName(kernelIndex)
                              << ") wrote incorrect data to memory" << std::endl;
                    failures++;
                    break; 
                }
            }
        }
        
        const std::uint32_t expectedAcc = xorZeroToN(kPerfLength - 1u);
        for (std::size_t i = 0; i < kernelCount; ++i) {
            std::size_t kernelIndex = lowerKernelIndex + i;
            const std::uint32_t outAcc = kernels[i].read(kOutAccDataOffset);
            const std::uint32_t outAccCtrl = kernels[i].read(kOutAccCtrlOffset);
            const bool valid = (outAccCtrl & 0x1u) != 0u;

            if (!valid || outAcc != expectedAcc) {
                if (failures < 8) {
                    std::cerr << "Kernel perf_" << kernelIndex << " (" << memoryGroupName(kernelIndex)
                              << ") failed: out_acc=0x" << std::hex << outAcc
                              << ", out_acc_ctrl=0x" << outAccCtrl
                              << ", expected=0x" << expectedAcc << std::dec << std::endl;
                }
                ++failures;
            }
        }

        const auto totalElapsed = writeElapsed + readElapsed;
        const std::uint64_t totalBytes = 2ull * kRepetitions * bytesPerKernel * static_cast<std::uint64_t>(kernelCount);
        std::cout << std::fixed << std::setprecision(2)
                  << "Combined read+write throughput: " << gibPerSecond(totalBytes, totalElapsed)
                  << " GiB/s aggregate" << std::endl;

        if (failures != 0) {
            std::cerr << failures << " kernel(s) produced invalid output" << std::endl;
            device.cleanup();
            return 1;
        }

        std::cout << "Test passed" << std::endl;
        device.cleanup();
        return 0;
    } catch (const std::bad_alloc& e) {
        std::cerr << "Allocation failed: " << e.what() << std::endl;
        std::cerr << "Try a smaller optional kernel_count (1-" << kTotalKernels
                  << ") to reduce host/device memory usage." << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
