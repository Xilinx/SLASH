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
