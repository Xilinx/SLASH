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

#include <iostream>
#include <cstdint>
#include <vrt/utils/logger.hpp>
#include <vrt/device.hpp>
#include <vrt/buffer.hpp>
#include <vrt/kernel.hpp>

int main(int argc, char* argv[]) {
    try {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " <BDF> <vrtbin file>" << std::endl;
            return 1;
        }
        std::string bdf = argv[1];
        std::string vrtbinFile = argv[2];
        uint32_t size = 1024*4;
        vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::DEBUG);
        vrt::Device device(bdf, vrtbinFile);

        vrt::Kernel dma_in(device, "dma_in_0");
        vrt::Kernel dma_out(device, "dma_out_0");
        vrt::Buffer<uint64_t> buffer_in(device, size, dma_in.argMemoryConfig("in"));
        vrt::Buffer<uint64_t> buffer_out(device, size, dma_out.argMemoryConfig("out"));
        for(uint32_t i = 0; i < size; i++) {
            buffer_in[i] = static_cast<uint64_t>(i);
        }
        buffer_in.sync(vrt::SyncType::HOST_TO_DEVICE);
        dma_in.setArg(0, buffer_in);
        dma_in.setArg(1, size);
        dma_out.setArg(0, size);
        dma_out.setArg(1, buffer_out);
        dma_in.start();
        dma_out.start();
        dma_in.wait();
        dma_out.wait();
        buffer_out.sync(vrt::SyncType::DEVICE_TO_HOST);
        for(uint32_t i = 0; i < size; i++) {
            if(buffer_in[i] != buffer_out[i]) {
                vrt::utils::Logger::log(vrt::utils::LogLevel::ERROR, __PRETTY_FUNCTION__, "Test failed (accuracy)");
                device.cleanup();
                return 2;
            }
        }
        vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Test passed");
        device.cleanup();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
