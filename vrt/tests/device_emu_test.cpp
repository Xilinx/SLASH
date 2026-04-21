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
#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/kernel.hpp>
#include <vrt/streaming_buffer.hpp>
#include <vrt/utils/platform.hpp>

#include <filesystem>
#include <thread>

#include "test_helpers.hpp"

class DeviceEmuTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;
    ScopedEnv* envCache = nullptr;
    vrt::Device device;

    void SetUp() override {
        tmpDir = makeTempDir("device-emu-test");
        envCache = new ScopedEnv("SLASH_CACHE_PATH", tmpDir.string());
        device = vrt::Device("0000:00:00", STUB_EMU_VBIN_PATH, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    void TearDown() override {
        device.cleanup();
        delete envCache;
        std::filesystem::remove_all(tmpDir);
    }
};

TEST_F(DeviceEmuTest, Construction) {
    SUCCEED();
}

TEST_F(DeviceEmuTest, GetPlatform) {
    EXPECT_EQ(device.getPlatform(), vrt::Platform::EMULATION);
}

TEST_F(DeviceEmuTest, GetFrequency) {
    EXPECT_EQ(device.getFrequency(), 0u);
}

TEST_F(DeviceEmuTest, GetKernelVadd) {
    auto kernel = device.getKernel("vadd");
    EXPECT_EQ(kernel.getName(), "vadd");
    EXPECT_EQ(kernel.getPhysAddr(), 0x10000u);
}

TEST_F(DeviceEmuTest, GetKernelPassthrough) {
    auto kernel = device.getKernel("passthrough");
    EXPECT_EQ(kernel.getName(), "passthrough");
    EXPECT_EQ(kernel.getPhysAddr(), 0x20000u);
}

TEST_F(DeviceEmuTest, GetKernelUnknownThrows) {
    EXPECT_THROW(device.getKernel("nonexistent"), std::runtime_error);
}

TEST_F(DeviceEmuTest, GetQdmaConnections) {
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

TEST_F(DeviceEmuTest, KernelSetArgAndCall) {
    auto kernel = device.getKernel("vadd");
    kernel.setArg(0, static_cast<uint64_t>(0x1000));
    kernel.setArg(1, static_cast<uint64_t>(0x2000));
    kernel.setArg(2, static_cast<uint64_t>(0x3000));
    kernel.setArg(3, static_cast<uint64_t>(64));
    EXPECT_NO_THROW(kernel.call());
}

TEST_F(DeviceEmuTest, KernelCallByName) {
    auto kernel = device.getKernel("vadd");
    kernel.setArg("in1", static_cast<uint64_t>(0x1000));
    kernel.setArg("in2", static_cast<uint64_t>(0x2000));
    kernel.setArg("out", static_cast<uint64_t>(0x3000));
    kernel.setArg("size", static_cast<uint64_t>(64));
    EXPECT_NO_THROW(kernel.call());
}

TEST_F(DeviceEmuTest, DISABLED_KernelStartAndWait) {
    auto kernel = device.getKernel("passthrough");
    kernel.setArg(0, static_cast<uint64_t>(42));
    EXPECT_NO_THROW(kernel.start());
    EXPECT_NO_THROW(kernel.wait());
}

TEST_F(DeviceEmuTest, KernelRead) {
    auto kernel = device.getKernel("vadd");
    uint32_t val = kernel.read(0x00);
    EXPECT_EQ(val, 0u);
}

TEST_F(DeviceEmuTest, BufferDDRConstruction) {
    EXPECT_NO_THROW({
        vrt::Buffer<int> buf(device, 64, vrt::MemoryRangeType::DDR);
    });
}

TEST_F(DeviceEmuTest, BufferHBMWithPort) {
    EXPECT_NO_THROW({
        vrt::Buffer<int> buf(device, 64, vrt::MemoryRangeType::HBM, 0);
    });
}

TEST_F(DeviceEmuTest, BufferHBMVnoc) {
    EXPECT_NO_THROW({
        vrt::Buffer<int> buf(device, 64, vrt::MemoryRangeType::HBM_VNOC);
    });
}

TEST_F(DeviceEmuTest, BufferSyncRoundTrip) {
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

TEST_F(DeviceEmuTest, StreamingBufferH2D) {
    auto kernel = device.getKernel("vadd");
    vrt::StreamingBuffer<int> sbuf(device, kernel, "axis_in", 16);
    sbuf[0] = 42;
    EXPECT_NO_THROW(sbuf.sync());
}

TEST_F(DeviceEmuTest, StreamingBufferD2H) {
    auto kernel = device.getKernel("vadd");
    vrt::StreamingBuffer<int> sbuf(device, kernel, "axis_out", 16);
    EXPECT_NO_THROW(sbuf.sync());
}

TEST_F(DeviceEmuTest, StreamingBufferWrongPortThrows) {
    auto kernel = device.getKernel("vadd");
    EXPECT_THROW(
        vrt::StreamingBuffer<int>(device, kernel, "nonexistent_port", 16),
        std::runtime_error);
}
