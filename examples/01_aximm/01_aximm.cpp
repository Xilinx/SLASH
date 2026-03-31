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
#include <cstring> // for std::memcpy
#include <cstdint>

#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <iostream>


#include <vrt/device.hpp>
#include <vrt/buffer.hpp>
#include <vrt/kernel.hpp>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <BDF> <vrtbin file>" << std::endl;
        return 1;
    }
    std::string bdf = argv[1];
    std::string vrtbinFile = argv[2];
    uint32_t size = 1024;
    uint32_t m = 3;
    uint32_t n = 2;
    try {
        vrt::Device device(bdf, vrtbinFile);
        vrt::Kernel dma(device, "dma_0");
        vrt::Kernel offset(device, "offset_0");

        vrt::Buffer<uint32_t> in_buff(device, size, offset.argMemoryConfig("input"));
        vrt::Buffer<uint32_t> out_buff(device, size, dma.argMemoryConfig("out"));
        for(uint32_t i = 0; i < size; i++) {
            in_buff[i] = i;
        }
        in_buff.sync(vrt::SyncType::HOST_TO_DEVICE);
        offset.setArg(0, size);
        offset.setArg(1, in_buff);
        offset.setArg(2, m);
        offset.setArg(3, n);
        dma.setArg(0, size);
        dma.setArg(1, out_buff);
        offset.start();
        dma.start();
        offset.wait();
        dma.wait();
        out_buff.sync(vrt::SyncType::DEVICE_TO_HOST);
        for(uint32_t i = 0; i < size; i++) {
            if(out_buff[i] != in_buff[i] * m + n) {
                std::cerr << "Test failed (accuracy)" << std::endl;
                std::cerr << "Error: " << i << " " << out_buff[i] << " " << in_buff[i] << std::endl;
                device.cleanup();
                return 2;
            }
        }
        std::cout << "Test passed" << std::endl;
        device.cleanup();
    } catch(const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}

