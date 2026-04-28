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
#include <vrt/utils/logger.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "test_helpers.hpp"

class LoggerTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;
    std::string logFile;

    void SetUp() override {
        tmpDir = makeTempDir("logger-test");
        logFile = (tmpDir / "test.log").string();
        vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::DEBUG);
        vrt::utils::Logger::setOutput(logFile);
    }

    void TearDown() override {
        vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::INFO);
        std::filesystem::remove_all(tmpDir);
    }

    std::string readLog() {
        std::ifstream ifs(logFile);
        return std::string((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());
    }
};

TEST_F(LoggerTest, GeneralPlaceholder) {
    vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "hello {}", "world");
    EXPECT_NE(readLog().find("hello world"), std::string::npos);
}

TEST_F(LoggerTest, GeneralPlaceholderInt) {
    vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "value={}", 42);
    EXPECT_NE(readLog().find("value=42"), std::string::npos);
}

TEST_F(LoggerTest, HexPlaceholder) {
    vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "addr={x}", 255);
    std::string log = readLog();
    EXPECT_NE(log.find("0xff"), std::string::npos);
}

TEST_F(LoggerTest, BinaryPlaceholder) {
    vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "bits={b}",
                            static_cast<uint8_t>(5));
    std::string log = readLog();
    EXPECT_NE(log.find("0b00000101"), std::string::npos);
}

TEST_F(LoggerTest, OctalPlaceholder) {
    vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "oct={o}", 8);
    std::string log = readLog();
    EXPECT_NE(log.find("0o"), std::string::npos);
}

TEST_F(LoggerTest, MultiplePlaceholders) {
    vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "{} + {} = {}", 1, 2, 3);
    EXPECT_NE(readLog().find("1 + 2 = 3"), std::string::npos);
}

TEST_F(LoggerTest, NoPlaceholders) {
    vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "literal message");
    EXPECT_NE(readLog().find("literal message"), std::string::npos);
}

TEST_F(LoggerTest, TooFewArgsThrows) {
    EXPECT_THROW(
        vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "{} {}", "only_one"),
        std::runtime_error);
}

TEST_F(LoggerTest, TooManyArgsThrows) {
    EXPECT_THROW(vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "{}", 1, 2),
                 std::runtime_error);
}

TEST_F(LoggerTest, SetLogLevelFilters) {
    vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::WARN);
    vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "should not appear");
    EXPECT_TRUE(readLog().empty());
}

TEST_F(LoggerTest, NoneBlocksAll) {
    vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::NONE);
    vrt::utils::Logger::log(vrt::utils::LogLevel::ERROR, "test", "blocked");
    vrt::utils::Logger::log(vrt::utils::LogLevel::WARN, "test", "blocked");
    EXPECT_TRUE(readLog().empty());
}

TEST_F(LoggerTest, WarnLevelPassesWarn) {
    vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::WARN);
    vrt::utils::Logger::log(vrt::utils::LogLevel::WARN, "test", "warn msg");
    EXPECT_NE(readLog().find("warn msg"), std::string::npos);
}

TEST_F(LoggerTest, ErrorLevelBlockedByWarnThreshold) {
    vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::WARN);
    vrt::utils::Logger::log(vrt::utils::LogLevel::ERROR, "test", "error msg");
    EXPECT_EQ(readLog().find("error msg"), std::string::npos);
}

TEST_F(LoggerTest, SetOutputToFile) {
    vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "file output");
    std::string log = readLog();
    EXPECT_FALSE(log.empty());
    EXPECT_NE(log.find("file output"), std::string::npos);
}

TEST_F(LoggerTest, SetOutputInvalidPathFallsBack) {
    EXPECT_NO_THROW(vrt::utils::Logger::setOutput("/nonexistent/path/log.txt"));
}

TEST_F(LoggerTest, TimestampFormat) {
    vrt::utils::Logger::log(vrt::utils::LogLevel::INFO, "test", "ts check");
    std::string log = readLog();
    std::regex tsPattern(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})");
    EXPECT_TRUE(std::regex_search(log, tsPattern));
}
