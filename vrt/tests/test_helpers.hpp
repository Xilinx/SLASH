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
#ifndef VRT_TEST_HELPERS_HPP
#define VRT_TEST_HELPERS_HPP

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

class ScopedEnv {
   public:
    explicit ScopedEnv(const char* name, std::optional<std::string> value = std::nullopt)
        : name_(name) {
        const char* prev = std::getenv(name);
        if (prev) {
            oldValue_ = prev;
        }
        if (value) {
            setenv(name, value->c_str(), 1);
        } else {
            unsetenv(name);
        }
    }

    ~ScopedEnv() {
        if (oldValue_) {
            setenv(name_.c_str(), oldValue_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

   private:
    std::string name_;
    std::optional<std::string> oldValue_;
};

inline std::filesystem::path makeTempDir(const std::string& prefix) {
    std::string tmpl = (std::filesystem::temp_directory_path() / (prefix + "-XXXXXX")).string();
    char* result = mkdtemp(tmpl.data());
    if (!result) {
        throw std::runtime_error("Failed to create temp directory");
    }
    return result;
}

inline std::string writeTempFile(const std::filesystem::path& dir, const std::string& name,
                                 const std::string& content) {
    auto path = dir / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path);
    if (!ofs) {
        throw std::runtime_error("Failed to create temp file: " + path.string());
    }
    ofs << content;
    ofs.close();
    return path.string();
}

#endif  // VRT_TEST_HELPERS_HPP
