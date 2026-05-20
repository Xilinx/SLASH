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

#include <filesystem>
#include <string>

#include "test_helpers.hpp"

class VrtbinHelperTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;
    ScopedEnv* envSlashCache = nullptr;

    void SetUp() override {
        tmpDir = makeTempDir("vrtbin-test");
        envSlashCache = new ScopedEnv("SLASH_CACHE_PATH", tmpDir.string());
    }

    void TearDown() override {
        delete envSlashCache;
        std::filesystem::remove_all(tmpDir);
    }
};

TEST_F(VrtbinHelperTest, GetSystemMapPathFromBdf) {
    auto path = vrt::Vrtbin::getSystemMapPathFromBdf("0000:01:00.0");
    EXPECT_NE(path.find("metadata_0000_01_00_0"), std::string::npos);
    EXPECT_NE(path.find("system_map.xml"), std::string::npos);
}

TEST_F(VrtbinHelperTest, GetUtilizationReportPathFromBdf) {
    auto path = vrt::Vrtbin::getUtilizationReportPathFromBdf("0000:01:00.0");
    EXPECT_NE(path.find("metadata_0000_01_00_0"), std::string::npos);
    EXPECT_NE(path.find("report_utilization.xml"), std::string::npos);
}

TEST_F(VrtbinHelperTest, SanitizeAlnum) {
    auto path = vrt::Vrtbin::getSystemMapPathFromBdf("abc123");
    EXPECT_NE(path.find("metadata_abc123"), std::string::npos);
}

TEST_F(VrtbinHelperTest, SanitizeSpecialChars) {
    auto path = vrt::Vrtbin::getSystemMapPathFromBdf("0000:01:00.0");
    EXPECT_NE(path.find("metadata_0000_01_00_0"), std::string::npos);
    EXPECT_EQ(path.find(":"), std::string::npos);
}

TEST_F(VrtbinHelperTest, SanitizeEmpty) {
    auto path = vrt::Vrtbin::getSystemMapPathFromBdf("");
    EXPECT_NE(path.find("metadata_default"), std::string::npos);
}

TEST_F(VrtbinHelperTest, PathStartsWithCacheDir) {
    auto path = vrt::Vrtbin::getSystemMapPathFromBdf("test");
    EXPECT_EQ(path.rfind(tmpDir.string(), 0), 0u);
}
