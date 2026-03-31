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
#include <cstring>

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
        vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::DEBUG);
        uint32_t size = 1024;

        vrt::Device device(bdf, vrtbinFile);
        vrt::Kernel vadd_0(device, "vadd_0");

        std::cout << "Current set frequency: "<< device.getFrequency() << " Hz" << std::endl;
        std::cout << "Max frequency: "<< device.getMaxFrequency() << " Hz" << std::endl;

        device.setFrequency(300000000);
        std::cout << "Current set frequency: "<< device.getFrequency() << " Hz" << std::endl;

        vrt::Buffer<int> a(device, size, vrt::MemoryRangeType::HBM, 0);
        vrt::Buffer<int> b(device, size, vrt::MemoryRangeType::HBM, 1);
        vrt::Buffer<int> c(device, size, vrt::MemoryRangeType::HBM, 2);

        for (int i = 0; i < size; i++) {
            a[i] = i;
            b[i] = i;
        }
        a.sync(vrt::SyncType::HOST_TO_DEVICE);
        b.sync(vrt::SyncType::HOST_TO_DEVICE);
        vadd_0.setArg(0, a);
        vadd_0.setArg(1, b);
        vadd_0.setArg(2, c);
        vadd_0.setArg(3, size);
        vadd_0.start();
        //vadd_0.start(a, b, c, size);
        vadd_0.wait();
        c.sync(vrt::SyncType::DEVICE_TO_HOST);
        for (int i = 0; i < size; i++) {
            if (c[i] != a[i] + b[i]) {
                std::cerr << "Test failed (accuracy)" << std::endl;
                std::cerr << "Error: " << c[i] << " != " << a[i] << " + " << b[i] << std::endl;
                device.cleanup();
                return 2;
            }
        }
        std::cout << "Test passed" << std::endl;
        device.cleanup();
     } catch (std::exception const& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
