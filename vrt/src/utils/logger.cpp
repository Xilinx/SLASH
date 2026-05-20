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

#include <vrt/utils/logger.hpp>

#include <fstream>

namespace vrt {
namespace utils {

std::unique_ptr<std::ofstream> Logger::fileStream_ = nullptr;
std::ostream* Logger::output_ = &std::cout;
LogLevel Logger::currentLogLevel_ = LogLevel::INFO;

void Logger::setOutput(const std::string& filename) {
    fileStream_ = std::make_unique<std::ofstream>(filename);
    if (fileStream_->is_open()) {
        output_ = fileStream_.get();
    } else {
        output_ = &std::cout;
    }
}

std::string Logger::getColor(LogLevel level) {
    switch (level) {
        case LogLevel::INFO:
            return "\033[32m";  // Green
        case LogLevel::ERROR:
            return "\033[31m";  // Red
        case LogLevel::DEBUG:
            return "\033[34m";  // Blue
        case LogLevel::WARN:
            return "\033[33m";  // Orange (Yellow)
        default:
            return "\033[0m";  // Reset
    }
}

std::string Logger::getLevelString(LogLevel level) {
    switch (level) {
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::ERROR:
            return "ERROR";
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::WARN:
            return "WARN";
        default:
            return "UNKNOWN";
    }
}

std::string Logger::getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&now_time_t), "%Y-%m-%d %H:%M:%S") << '.'
        << std::setfill('0') << std::setw(3) << now_ms.count();
    return oss.str();
}

void Logger::setLogLevel(LogLevel level) { currentLogLevel_ = level; }

}  // namespace utils

}  // namespace vrt