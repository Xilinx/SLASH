#include <gtest/gtest.h>
#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/kernel.hpp>
#include <vrt/streaming_buffer.hpp>
#include <vrt/utils/platform.hpp>

#include <filesystem>
#include <thread>

#include "test_helpers.hpp"

class DeviceSimTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;
    ScopedEnv* envCache = nullptr;
    vrt::Device device;

    void SetUp() override {
        tmpDir = makeTempDir("device-sim-test");
        envCache = new ScopedEnv("SLASH_CACHE_PATH", tmpDir.string());
        device = vrt::Device("0000:00:00", STUB_SIM_VBIN_PATH, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    void TearDown() override {
        device.cleanup();
        delete envCache;
        std::filesystem::remove_all(tmpDir);
    }
};

TEST_F(DeviceSimTest, Construction) {
    SUCCEED();
}

TEST_F(DeviceSimTest, GetPlatform) {
    EXPECT_EQ(device.getPlatform(), vrt::Platform::SIMULATION);
}

TEST_F(DeviceSimTest, GetKernelVadd) {
    auto kernel = device.getKernel("vadd");
    EXPECT_EQ(kernel.getName(), "vadd");
    EXPECT_EQ(kernel.getPhysAddr(), 0x10000u);
}

TEST_F(DeviceSimTest, KernelWrite) {
    auto kernel = device.getKernel("vadd");
    EXPECT_NO_THROW(kernel.write(0x10, 0xDEAD));
}

TEST_F(DeviceSimTest, KernelRead) {
    auto kernel = device.getKernel("vadd");
    uint32_t val = kernel.read(0x10);
    EXPECT_EQ(val, 0u);
}

TEST_F(DeviceSimTest, BufferConstruction) {
    EXPECT_NO_THROW({
        vrt::Buffer<int> buf(device, 64, vrt::MemoryRangeType::DDR);
    });
}

TEST_F(DeviceSimTest, BufferSyncRoundTrip) {
    vrt::Buffer<int> buf(device, 4, vrt::MemoryRangeType::DDR);
    buf[0] = 100;
    buf[1] = 200;
    buf[2] = 300;
    buf[3] = 400;
    buf.sync(vrt::SyncType::HOST_TO_DEVICE);
    buf[0] = 0;
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 0;
    buf.sync(vrt::SyncType::DEVICE_TO_HOST);
    EXPECT_EQ(buf[0], 100);
    EXPECT_EQ(buf[1], 200);
    EXPECT_EQ(buf[2], 300);
    EXPECT_EQ(buf[3], 400);
}

TEST_F(DeviceSimTest, StreamingBufferThrowsNotImplemented) {
    auto kernel = device.getKernel("vadd");
    vrt::StreamingBuffer<int> sbuf(device, kernel, "axis_in", 16);
    EXPECT_THROW(sbuf.sync(), std::runtime_error);
}
