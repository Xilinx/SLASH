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

/// @file validate.cpp
/// @brief Implementation of the Validate command.
///
/// Resets a V80 board, then exercises HBM and DDR memory over PCIe:
///   1. Data integrity checks (write pattern, read back, verify).
///   2. Parallel bandwidth measurements (N threads, one per buffer).
///
/// We use libvrtdpp (vrtd::Session / vrtd::Device / vrtd::Buffer) directly
/// rather than the higher-level vrt::Device because vrt::Device requires a
/// vrtbin path for system-map parsing, which is unnecessary for raw memory
/// validation.
///
/// TODO: Decide whether vrt::Device should gain a vrtbin-less constructor so
///       that commands like validate can go through the standard vrt:: layer.

#include "validate.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include <vrtd/session.hpp>

#include "bdf.hpp"

/// Buffer size for each allocation (64 MB — one allocator subregion).
static constexpr uint64_t BUFFER_SIZE = 64ULL * 1024 * 1024;

/// Fill @p buf with a deterministic pattern seeded by @p seed.
static void fillPattern(void* buf, uint64_t size, uint32_t seed) {
    auto* p = static_cast<uint32_t*>(buf);
    uint64_t count = size / sizeof(uint32_t);
    for (uint64_t i = 0; i < count; ++i) {
        p[i] = static_cast<uint32_t>(i) ^ seed;
    }
}

/// Verify @p buf matches the pattern produced by fillPattern().
/// Returns true on match, false on first mismatch.
static bool verifyPattern(const void* buf, uint64_t size, uint32_t seed) {
    auto* p = static_cast<const uint32_t*>(buf);
    uint64_t count = size / sizeof(uint32_t);
    for (uint64_t i = 0; i < count; ++i) {
        if (p[i] != (static_cast<uint32_t>(i) ^ seed)) {
            return false;
        }
    }
    return true;
}

/// Run data integrity on every buffer: write pattern → sync to device →
/// clear host → sync from device → verify.
/// @return true if all buffers pass.
static bool testDataIntegrity(std::vector<vrtd::Buffer>& buffers,
                              const std::string& label) {
    bool allPassed = true;

    for (size_t i = 0; i < buffers.size(); ++i) {
        auto& buf = buffers[i];
        uint32_t seed = static_cast<uint32_t>(i);
        uint64_t size = buf.getSize();

        fillPattern(buf.data(), size, seed);
        buf.syncToDevice(0, size);

        std::memset(buf.data(), 0, size);
        buf.syncFromDevice(0, size);

        bool ok = verifyPattern(buf.data(), size, seed);
        std::cout << "    " << label << i << ": "
                  << (ok ? "OK" : "FAIL") << std::endl;

        if (!ok) {
            allPassed = false;
        }
    }

    return allPassed;
}

/// Measure aggregate write and read bandwidth across all buffers in parallel
/// (one std::thread per buffer).
static void testBandwidth(std::vector<vrtd::Buffer>& buffers) {
    uint64_t totalBytes = 0;
    for (auto& buf : buffers) {
        std::memset(buf.data(), 0xAB, buf.getSize());
        totalBytes += buf.getSize();
    }

    // -- Write (H2C) bandwidth --
    auto writeStart = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> threads;
        threads.reserve(buffers.size());
        for (auto& buf : buffers) {
            threads.emplace_back([&buf] {
                buf.syncToDevice(0, buf.getSize());
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    }
    auto writeEnd = std::chrono::steady_clock::now();

    // -- Read (C2H) bandwidth --
    auto readStart = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> threads;
        threads.reserve(buffers.size());
        for (auto& buf : buffers) {
            threads.emplace_back([&buf] {
                buf.syncFromDevice(0, buf.getSize());
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    }
    auto readEnd = std::chrono::steady_clock::now();

    double writeSec = std::chrono::duration<double>(writeEnd - writeStart).count();
    double readSec  = std::chrono::duration<double>(readEnd - readStart).count();
    double totalMB  = static_cast<double>(totalBytes) / (1024.0 * 1024.0);

    std::cout << "    Write: " << std::fixed << std::setprecision(2)
              << (totalMB / writeSec) << " MB/s" << std::endl;
    std::cout << "    Read:  " << std::fixed << std::setprecision(2)
              << (totalMB / readSec) << " MB/s" << std::endl;
}

int Validate::run(const Options& options) {
    std::string bdf = resolveBoardBdf(options.bdf, "validate");
    unsigned N = options.threads;

    // -- Step 1: (Optional) Reset the device via vrtd --
    if (!options.noReset) {
        std::cout << "Resetting device " << bdf << "..." << std::endl;
        {
            vrtd::Session session;
            auto device = session.getDeviceByBdf(bdf);
            device.hotplugOp(vrtd::HotplugOp::ResetSequence);
        }
        // Session is torn down; the daemon has re-discovered the device.
    }

    vrtd::Session session;
    auto device = session.getDeviceByBdf(bdf);

    // -- Step 2: HBM — integrity then bandwidth --
    std::cout << "Testing HBM data integrity (" << N << " regions)..." << std::endl;
    {
        std::vector<vrtd::Buffer> hbmBuffers;
        hbmBuffers.reserve(N);
        for (unsigned i = 0; i < N; ++i) {
            hbmBuffers.push_back(device.openHbmBuffer(i, BUFFER_SIZE));
        }

        if (!testDataIntegrity(hbmBuffers, "HBM")) {
            std::cerr << "HBM data integrity check failed" << std::endl;
            return 1;
        }

        std::cout << "Testing HBM bandwidth (" << N << " threads)..." << std::endl;
        testBandwidth(hbmBuffers);
    }
    // HBM buffers released.

    // -- Step 3: DDR — integrity then bandwidth --
    std::cout << "Testing DDR data integrity (" << N << " buffers)..." << std::endl;
    {
        std::vector<vrtd::Buffer> ddrBuffers;
        ddrBuffers.reserve(N);
        for (unsigned i = 0; i < N; ++i) {
            ddrBuffers.push_back(device.openDdrBuffer(BUFFER_SIZE));
        }

        if (!testDataIntegrity(ddrBuffers, "DDR")) {
            std::cerr << "DDR data integrity check failed" << std::endl;
            return 1;
        }

        std::cout << "Testing DDR bandwidth (" << N << " threads)..." << std::endl;
        testBandwidth(ddrBuffers);
    }
    // DDR buffers released.

    return 0;
}
