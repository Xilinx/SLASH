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
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/kernel.hpp>
#include <vrt/streaming_buffer.hpp>
#include <vrt/utils/platform.hpp>

#include <filesystem>
#include <thread>

#include "test_helpers.hpp"

using ::testing::Contains;

// The vbin integration tests launch a Python ZeroMQ stub server
// (tests/fixtures/stub_vbin/vrt_stub_server.py) to emulate the FPGA runtime, which requires
// pyzmq. This meta-test verifies the dependency is present and fails fast with a clear
// diagnostic if it is missing.
TEST(PythonEnvTest, PyzmqIsImportable) {
    int rc = std::system("python3 -c 'import zmq' >/dev/null 2>&1");
    int exit_status = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    ASSERT_EQ(exit_status, 0)
        << "pyzmq is not importable in the active Python environment.\n"
        << "It is required by the Python stub server used by the VRT vbin integration tests.\n"
        << "Install it with:  pip install pyzmq";
}

class DeviceTest : public ::testing::Test, public ::testing::WithParamInterface<vrt::Platform> {
   protected:
    std::filesystem::path tmpDir;
    ScopedEnv* envCache = nullptr;
    vrt::Platform platform;
    vrt::Device device;

    void SetUp() override {
        tmpDir = makeTempDir("device-test");
        envCache = new ScopedEnv("SLASH_CACHE_PATH", tmpDir.string());

        platform = GetParam();
        std::array<vrt::Platform,2> supported_platforms{vrt::Platform::EMULATION, vrt::Platform::SIMULATION};
        EXPECT_THAT(supported_platforms, Contains(platform));

        std::string vbin_path;
        if (platform == vrt::Platform::EMULATION) {
            vbin_path = STUB_EMU_VBIN_PATH;
        } else {
            vbin_path = STUB_SIM_VBIN_PATH;
        }
        device = vrt::Device("0000:00:00", vbin_path, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    void TearDown() override {
        device.cleanup();
        delete envCache;
        std::filesystem::remove_all(tmpDir);
    }
};

TEST_P(DeviceTest, Construction) {
    SUCCEED();
}

TEST_P(DeviceTest, GetPlatform) {
    EXPECT_EQ(device.getPlatform(), platform);
}

TEST_P(DeviceTest, GetFrequency) {
    EXPECT_EQ(device.getFrequency(), 0u);
}

TEST_P(DeviceTest, GetKernelVadd) {
    auto kernel = device.getKernel("vadd");
    EXPECT_EQ(kernel.getName(), "vadd");
    EXPECT_EQ(kernel.getPhysAddr(), 0x10000u);
}

TEST_P(DeviceTest, GetKernelPassthrough) {
    auto kernel = device.getKernel("passthrough");
    EXPECT_EQ(kernel.getName(), "passthrough");
    EXPECT_EQ(kernel.getPhysAddr(), 0x20000u);
}

TEST_P(DeviceTest, GetKernelUnknownThrows) {
    EXPECT_THROW(device.getKernel("nonexistent"), std::runtime_error);
}

TEST_P(DeviceTest, GetQdmaConnections) {
    auto conns = device.getHandle()->getQdmaConnections();
    ASSERT_EQ(conns.size(), 2u);
    EXPECT_EQ(conns[0].getKernel(), "vadd");
    EXPECT_EQ(conns[0].getInterface(), "axis_in");
    EXPECT_EQ(conns[0].getDirection(), vrt::StreamDirection::HOST_TO_DEVICE);
    EXPECT_EQ(conns[0].getQid(), 0u);
    EXPECT_EQ(conns[1].getInterface(), "axis_out");
    EXPECT_EQ(conns[1].getDirection(), vrt::StreamDirection::DEVICE_TO_HOST);
    EXPECT_EQ(conns[1].getQid(), 1u);
}

TEST_P(DeviceTest, KernelWrite) {
    auto kernel = device.getKernel("vadd");
    EXPECT_NO_THROW(kernel.write(0x10, 0xDEAD));
}

TEST_P(DeviceTest, KernelRead) {
    auto kernel = device.getKernel("vadd");
    uint32_t val = kernel.read(0x10);
    EXPECT_EQ(val, 0u);
}

TEST_P(DeviceTest, BufferDDRConstruction) {
    EXPECT_NO_THROW({
        vrt::Buffer<int> buf(device, 64, vrt::MemoryRangeType::DDR);
    });
}

TEST_P(DeviceTest, BufferHBMWithPort) {
    EXPECT_NO_THROW({
        vrt::Buffer<int> buf(device, 64, vrt::MemoryRangeType::HBM, 0);
    });
}

TEST_P(DeviceTest, BufferHBMVnoc) {
    EXPECT_NO_THROW({
        vrt::Buffer<int> buf(device, 64, vrt::MemoryRangeType::HBM_VNOC);
    });
}

TEST_P(DeviceTest, BufferSyncRoundTrip) {
    vrt::Buffer<int> buf(device, 4, vrt::MemoryRangeType::DDR);
    buf[0] = 10;
    buf[1] = 20;
    buf[2] = 30;
    buf[3] = 40;
    buf.sync(vrt::SyncType::HOST_TO_DEVICE);
    buf[0] = 0;
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 0;
    buf.sync(vrt::SyncType::DEVICE_TO_HOST);
    EXPECT_EQ(buf[0], 10);
    EXPECT_EQ(buf[1], 20);
    EXPECT_EQ(buf[2], 30);
    EXPECT_EQ(buf[3], 40);
}

TEST_P(DeviceTest, StreamingBufferH2D) {
    if (platform == vrt::Platform::SIMULATION) {
        GTEST_SKIP();
    }
    auto kernel = device.getKernel("vadd");
    vrt::StreamingBuffer<int> sbuf(device, kernel, "axis_in", 16);
    sbuf[0] = 42;
    EXPECT_NO_THROW(sbuf.sync());
}

TEST_P(DeviceTest, StreamingBufferD2H) {
    if (platform == vrt::Platform::SIMULATION) {
        GTEST_SKIP();
    }
    auto kernel = device.getKernel("vadd");
    vrt::StreamingBuffer<int> sbuf(device, kernel, "axis_out", 16);
    EXPECT_NO_THROW(sbuf.sync());
}

TEST_P(DeviceTest, StreamingBufferWrongPortThrows) {
    if (platform == vrt::Platform::SIMULATION) {
        GTEST_SKIP();
    }
    auto kernel = device.getKernel("vadd");
    EXPECT_THROW(
        vrt::StreamingBuffer<int>(device, kernel, "nonexistent_port", 16),
        std::runtime_error);
}

TEST_P(DeviceTest, StreamingBufferThrowsNotImplemented) {
    if (platform != vrt::Platform::SIMULATION) {
        GTEST_SKIP();
    }
    auto kernel = device.getKernel("vadd");
    vrt::StreamingBuffer<int> sbuf(device, kernel, "axis_in", 16);
    EXPECT_THROW(sbuf.sync(), std::runtime_error);
}

TEST_P(DeviceTest, KernelVaddRoundTrip) {
    constexpr int N = 4;
    vrt::Kernel kernel = device.getKernel("vadd");
    vrt::Buffer<int> in1(device, N, vrt::MemoryRangeType::HBM, 0);
    vrt::Buffer<int> in2(device, N, vrt::MemoryRangeType::DDR);
    vrt::Buffer<int> out(device, N, vrt::MemoryRangeType::HBM_VNOC);

    for (int i = 0; i < N; ++i) {
        in1[i] = i + 1;
        in2[i] = (i + 1) * 10;
    }
    in1.sync(vrt::SyncType::HOST_TO_DEVICE);
    in2.sync(vrt::SyncType::HOST_TO_DEVICE);

    kernel.setArg(0, static_cast<uint64_t>(in1.getPhysAddr()));
    kernel.setArg(1, static_cast<uint64_t>(in2.getPhysAddr()));
    kernel.setArg(2, static_cast<uint64_t>(out.getPhysAddr()));
    kernel.setArg(3, static_cast<uint64_t>(N));
    ASSERT_NO_THROW(kernel.call());

    out.sync(vrt::SyncType::DEVICE_TO_HOST);
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(out[i], in1[i] + in2[i]);
    }
}

INSTANTIATE_TEST_SUITE_P(DeviceTestSuite, DeviceTest, ::testing::Values(vrt::Platform::EMULATION, vrt::Platform::SIMULATION));
