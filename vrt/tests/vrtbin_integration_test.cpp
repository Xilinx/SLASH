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
#include <vrt/vrtbin.hpp>
#include <vrt/utils/platform.hpp>

#include <filesystem>

#include "test_helpers.hpp"

class VrtbinEmuTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;
    ScopedEnv* envCache = nullptr;

    void SetUp() override {
        tmpDir = makeTempDir("vrtbin-emu-test");
        envCache = new ScopedEnv("SLASH_CACHE_PATH", tmpDir.string());
    }

    void TearDown() override {
        delete envCache;
        std::filesystem::remove_all(tmpDir);
    }
};

TEST_F(VrtbinEmuTest, ExtractAndFindSystemMap) {
    vrt::Vrtbin vrtbin(STUB_EMU_VBIN_PATH, "0000:00:00");
    EXPECT_FALSE(vrtbin.getSystemMapPath().empty());
    EXPECT_TRUE(std::filesystem::exists(vrtbin.getSystemMapPath()));
}

TEST_F(VrtbinEmuTest, DetectsPlatformEmulation) {
    vrt::Vrtbin vrtbin(STUB_EMU_VBIN_PATH, "0000:00:00");
    EXPECT_EQ(vrtbin.getPlatform(), vrt::Platform::EMULATION);
}

TEST_F(VrtbinEmuTest, FindsEmulationExec) {
    vrt::Vrtbin vrtbin(STUB_EMU_VBIN_PATH, "0000:00:00");
    EXPECT_FALSE(vrtbin.getEmulationExec().empty());
    EXPECT_TRUE(std::filesystem::exists(vrtbin.getEmulationExec()));
}

TEST_F(VrtbinEmuTest, FindsEmulationManifest) {
    vrt::Vrtbin vrtbin(STUB_EMU_VBIN_PATH, "0000:00:00");
    EXPECT_FALSE(vrtbin.getEmulationManifest().empty());
    EXPECT_TRUE(std::filesystem::exists(vrtbin.getEmulationManifest()));
}

TEST_F(VrtbinEmuTest, NoPdiFilesForEmulation) {
    vrt::Vrtbin vrtbin(STUB_EMU_VBIN_PATH, "0000:00:00");
    EXPECT_TRUE(vrtbin.getPdiPaths().empty());
}

class VrtbinSimTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;
    ScopedEnv* envCache = nullptr;

    void SetUp() override {
        tmpDir = makeTempDir("vrtbin-sim-test");
        envCache = new ScopedEnv("SLASH_CACHE_PATH", tmpDir.string());
    }

    void TearDown() override {
        delete envCache;
        std::filesystem::remove_all(tmpDir);
    }
};

TEST_F(VrtbinSimTest, ExtractAndFindSystemMap) {
    vrt::Vrtbin vrtbin(STUB_SIM_VBIN_PATH, "0000:00:00");
    EXPECT_FALSE(vrtbin.getSystemMapPath().empty());
    EXPECT_TRUE(std::filesystem::exists(vrtbin.getSystemMapPath()));
}

TEST_F(VrtbinSimTest, DetectsPlatformSimulation) {
    vrt::Vrtbin vrtbin(STUB_SIM_VBIN_PATH, "0000:00:00");
    EXPECT_EQ(vrtbin.getPlatform(), vrt::Platform::SIMULATION);
}

TEST_F(VrtbinSimTest, FindsSimulationExec) {
    vrt::Vrtbin vrtbin(STUB_SIM_VBIN_PATH, "0000:00:00");
    EXPECT_FALSE(vrtbin.getSimulationExec().empty());
    EXPECT_TRUE(std::filesystem::exists(vrtbin.getSimulationExec()));
}

TEST_F(VrtbinSimTest, NoPdiFilesForSimulation) {
    vrt::Vrtbin vrtbin(STUB_SIM_VBIN_PATH, "0000:00:00");
    EXPECT_TRUE(vrtbin.getPdiPaths().empty());
}

TEST(VrtbinErrorTest, NonexistentVbinThrows) {
    EXPECT_THROW(vrt::Vrtbin("/nonexistent/path.vbin", "0000:00:00"), std::runtime_error);
}
