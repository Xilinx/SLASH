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

#include <getopt.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
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

constexpr std::uint32_t kDefaultLength = 0x1000000u;
constexpr std::uint32_t kDefaultRepetitions = 1u;
constexpr std::uint32_t kWriteMode = 0u;
constexpr std::uint32_t kReadMode = 1u;

constexpr std::uint32_t kOutAccDataOffset = 0x34u;
constexpr std::uint32_t kOutAccCtrlOffset = 0x38u;

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
    std::size_t lowerKernelIndex = 0;
    std::size_t upperKernelIndex = kDefaultKernels;
    std::uint32_t perfLength = kDefaultLength;
    std::uint32_t repetitions = kDefaultRepetitions;

    auto printUsage = [&]() {
        std::cerr << "Usage: " << argv[0]
                  << " <BDF> <vrtbin file>"
                     " [-b LO] [-e HI] [-l LEN] [-r REPS]\n"
                     "  -b, --begin LO    first kernel index (default 0)\n"
                     "  -e, --end   HI    one past last kernel index (default "
                  << kDefaultKernels << ", max " << kTotalKernels << ")\n"
                     "  -l, --length LEN  256-bit words per kernel buffer (default 0x"
                  << std::hex << kDefaultLength << std::dec << ")\n"
                     "  -r, --reps REPS   repetitions per phase (default "
                  << kDefaultRepetitions << ")\n";
    };

    static const struct option longOpts[] = {
        {"begin",  required_argument, nullptr, 'b'},
        {"end",    required_argument, nullptr, 'e'},
        {"length", required_argument, nullptr, 'l'},
        {"reps",   required_argument, nullptr, 'r'},
        {"help",   no_argument,       nullptr, 'h'},
        {nullptr,  0,                 nullptr, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:e:l:r:h", longOpts, nullptr)) != -1) {
        char* end = nullptr;
        unsigned long long v = (opt == 'h') ? 0ull : std::strtoull(optarg, &end, 0);
        if (opt != 'h' && (end == optarg || *end != '\0')) {
            std::cerr << "Invalid value for -" << static_cast<char>(opt) << ": " << optarg << std::endl;
            return 1;
        }
        switch (opt) {
            case 'b': lowerKernelIndex = static_cast<std::size_t>(v); break;
            case 'e': upperKernelIndex = static_cast<std::size_t>(v); break;
            case 'l':
                if (v == 0 || v > 0xFFFFFFFFull) {
                    std::cerr << "--length must be in (0, 2^32)" << std::endl;
                    return 1;
                }
                perfLength = static_cast<std::uint32_t>(v);
                break;
            case 'r':
                if (v == 0 || v > 0xFFFFFFFFull) {
                    std::cerr << "--reps must be in (0, 2^32)" << std::endl;
                    return 1;
                }
                repetitions = static_cast<std::uint32_t>(v);
                break;
            case 'h':
            default:
                printUsage();
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (argc - optind != 2) {
        printUsage();
        return 1;
    }
    const std::string bdf = argv[optind];
    const std::string vrtbinFile = argv[optind + 1];

    if (lowerKernelIndex >= upperKernelIndex || upperKernelIndex > kTotalKernels) {
        std::cerr << "kernel range must satisfy 0 <= begin < end <= " << kTotalKernels << std::endl;
        return 1;
    }
    std::size_t kernelCount = upperKernelIndex - lowerKernelIndex;

    try {
        vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::INFO);

        const std::uint64_t bytesPerKernel = static_cast<std::uint64_t>(perfLength) * sizeof(Word256);
        const double bufferFootprintGiB =
            (static_cast<double>(bytesPerKernel) * static_cast<double>(kernelCount)) /
            (1024.0 * 1024.0 * 1024.0);

        std::cout << "VRT Version: " << vrt::getVersion() << std::endl;
        std::cout << "Launching " << kernelCount << " perf kernels (indices ["
                  << lowerKernelIndex << ", " << upperKernelIndex << "))" << std::endl;
        std::cout << "Length: " << perfLength << " words ("
                  << (bytesPerKernel >> 20) << " MiB per kernel)"
                  << ", reps: " << repetitions << std::endl;
        std::cout << std::fixed << std::setprecision(2)
                  << "Aggregate buffer footprint: " << bufferFootprintGiB << " GiB" << std::endl;

        vrt::Device device(bdf, vrtbinFile);
        const bool isEmu = (device.getPlatform() == vrt::Platform::EMULATION) || (device.getPlatform() == vrt::Platform::SIMULATION);
        double expected_rate_gibs = static_cast<double>(device.getFrequency()) * sizeof(Word256) * kernelCount / (1024 * 1024 * 1024);

        std::vector<vrt::Kernel> kernels;
        std::vector<vrt::Buffer<Word256>> buffers;
        kernels.reserve(kernelCount);
        buffers.reserve(kernelCount);
        for (std::size_t i = lowerKernelIndex; i < upperKernelIndex; ++i) {
            vrt::Kernel kernel(device, "perf_" + std::to_string(i));
            kernels.emplace_back(kernel);
            buffers.emplace_back(device, perfLength, kernel.argMemoryConfig("mem_ptr"));
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
                kernels[i].setArg(1, repetitions);
                kernels[i].setArg(2, perfLength);
                kernels[i].setArg(3, wr);
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

            const std::uint64_t totalBytes = static_cast<std::uint64_t>(repetitions) *
                                             bytesPerKernel *
                                             static_cast<std::uint64_t>(kernelCount);
            double measured_rate_gibs = gibPerSecond(totalBytes, elapsed);

            std::cout << label << " phase time: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                      << " ms";
            std::cout << " (" << std::fixed << std::setprecision(2)
                      << measured_rate_gibs << " GiB/s aggregate, "
                      << measured_rate_gibs / expected_rate_gibs * 100.0 << " \% of expected rate "
                      << expected_rate_gibs << " GiB/s aggregate)"
                      << std::endl;
            return elapsed;
        };

        const auto writeElapsed = runPhase(kWriteMode, "Write");
        const auto readElapsed = runPhase(kReadMode, "Read");

        std::size_t failures = 0;

        for (std::size_t i = 0; i < kernelCount; i++) {
            std::size_t kernelIndex = lowerKernelIndex + i;
            buffers[i].sync(vrt::SyncType::DEVICE_TO_HOST);
            for (std::size_t j = 0; j < perfLength; j++) {
                if (buffers[i][j].lane[0] != j) {
                    std::cerr << "Kernel perf_" << kernelIndex << " (" << memoryGroupName(kernelIndex)
                              << ") wrote incorrect data to memory" << std::endl;
                    failures++;
                    break; 
                }
            }
        }
        
        const std::uint32_t expectedAcc = xorZeroToN(perfLength - 1u);
        for (std::size_t i = 0; i < kernelCount; ++i) {
            std::size_t kernelIndex = lowerKernelIndex + i;
            const std::uint32_t outAccCtrl = kernels[i].read(kOutAccCtrlOffset);
            const std::uint32_t outAcc = kernels[i].read(kOutAccDataOffset);
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
        const std::uint64_t totalBytes = 2ull * static_cast<std::uint64_t>(repetitions) *
                                         bytesPerKernel * static_cast<std::uint64_t>(kernelCount);
        const double measured_rate_gibs = gibPerSecond(totalBytes, totalElapsed);
        std::cout << std::fixed << std::setprecision(2)
                  << "Combined read+write throughput: " << measured_rate_gibs << " GiB/s aggregate, "
                  << measured_rate_gibs / expected_rate_gibs * 100.0 << " \% of expected rate "
                  << expected_rate_gibs << " GiB/s aggregate."
                  << std::endl;

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
        std::cerr << "Try a smaller --length or fewer kernels to reduce host/device memory usage."
                  << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
