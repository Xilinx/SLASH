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
#include <vrt/utils/filesystem_cache.hpp>

#include <filesystem>
#include <unistd.h>

#include "test_helpers.hpp"

class FilesystemCacheTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;

    ScopedEnv* envSlashCache = nullptr;
    ScopedEnv* envXdgCache = nullptr;
    ScopedEnv* envHome = nullptr;
    ScopedEnv* envSlashRuntime = nullptr;
    ScopedEnv* envXdgRuntime = nullptr;

    void SetUp() override { tmpDir = makeTempDir("fscache-test"); }

    void TearDown() override {
        delete envSlashCache;
        delete envXdgCache;
        delete envHome;
        delete envSlashRuntime;
        delete envXdgRuntime;
        std::filesystem::remove_all(tmpDir);
    }

    void clearCacheEnvVars() {
        envSlashCache = new ScopedEnv("SLASH_CACHE_PATH");
        envXdgCache = new ScopedEnv("XDG_CACHE_HOME");
        envHome = new ScopedEnv("HOME");
    }

    void clearRuntimeEnvVars() {
        envSlashRuntime = new ScopedEnv("SLASH_RUNTIME_PATH");
        envXdgRuntime = new ScopedEnv("XDG_RUNTIME_DIR");
    }
};

TEST_F(FilesystemCacheTest, CachePathFromSlashCachePath) {
    clearCacheEnvVars();
    std::string target = (tmpDir / "slash-cache").string();
    ScopedEnv env("SLASH_CACHE_PATH", target);
    auto path = FilesystemCache::getCachePath();
    EXPECT_EQ(path, std::filesystem::path(target));
}

TEST_F(FilesystemCacheTest, CachePathFromXdgCacheHome) {
    clearCacheEnvVars();
    std::string xdg = (tmpDir / "xdg-cache").string();
    ScopedEnv env("XDG_CACHE_HOME", xdg);
    auto path = FilesystemCache::getCachePath();
    EXPECT_EQ(path, std::filesystem::path(xdg) / "SLASH" / "vrt");
}

TEST_F(FilesystemCacheTest, CachePathFromHome) {
    clearCacheEnvVars();
    std::string home = (tmpDir / "home").string();
    ScopedEnv env("HOME", home);
    auto path = FilesystemCache::getCachePath();
    EXPECT_EQ(path, std::filesystem::path(home) / ".cache" / "SLASH" / "vrt");
}

TEST_F(FilesystemCacheTest, CachePathFallback) {
    clearCacheEnvVars();
    auto path = FilesystemCache::getCachePath();
    std::string expected = "/tmp/SLASH-cache-" + std::to_string(getuid()) + "/vrt";
    EXPECT_EQ(path, std::filesystem::path(expected));
}

TEST_F(FilesystemCacheTest, RuntimePathFromSlashRuntimePath) {
    clearRuntimeEnvVars();
    std::string target = (tmpDir / "slash-runtime").string();
    ScopedEnv env("SLASH_RUNTIME_PATH", target);
    auto path = FilesystemCache::getRuntimePath();
    EXPECT_EQ(path, std::filesystem::path(target));
}

TEST_F(FilesystemCacheTest, RuntimePathFromXdgRuntimeDir) {
    clearRuntimeEnvVars();
    std::string xdg = (tmpDir / "xdg-runtime").string();
    ScopedEnv env("XDG_RUNTIME_DIR", xdg);
    auto path = FilesystemCache::getRuntimePath();
    EXPECT_EQ(path, std::filesystem::path(xdg) / "SLASH" / "vrt");
}

TEST_F(FilesystemCacheTest, RuntimePathFallback) {
    clearRuntimeEnvVars();
    ScopedEnv envHome("HOME");
    auto path = FilesystemCache::getRuntimePath();
    std::string expected = "/tmp/SLASH-run-" + std::to_string(getuid()) + "/vrt";
    EXPECT_EQ(path, std::filesystem::path(expected));
}

TEST_F(FilesystemCacheTest, CachePathCreatesDirectory) {
    clearCacheEnvVars();
    std::string target = (tmpDir / "new-cache-dir").string();
    ScopedEnv env("SLASH_CACHE_PATH", target);
    auto path = FilesystemCache::getCachePath();
    EXPECT_TRUE(std::filesystem::is_directory(path));
}

TEST_F(FilesystemCacheTest, RuntimePathCreatesDirectory) {
    clearRuntimeEnvVars();
    std::string target = (tmpDir / "new-runtime-dir").string();
    ScopedEnv env("SLASH_RUNTIME_PATH", target);
    auto path = FilesystemCache::getRuntimePath();
    EXPECT_TRUE(std::filesystem::is_directory(path));
}
