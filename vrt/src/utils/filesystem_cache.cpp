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

#include <vrt/utils/filesystem_cache.hpp>

#include <filesystem>
#include <cstdlib>
#include <unistd.h>

std::filesystem::path FilesystemCache::getCachePath() {
    std::filesystem::path path;

    // 1. Try $SLASH_CACHE_PATH
    if (const char* slashCache = std::getenv("SLASH_CACHE_PATH")) {
        path = slashCache;
    }

    // 2. Try $XDG_CACHE_HOME/SLASH
    else if (const char* xdgCache = std::getenv("XDG_CACHE_HOME")) {
        path = std::filesystem::path(xdgCache) / "SLASH" / "vrt";
    }

    // 3. Try $HOME/.cache/SLASH
    else if (const char* home = std::getenv("HOME")) {
        path = std::filesystem::path(home) / ".cache" / "SLASH" / "vrt";
    }

    // 4. Fallback: /tmp/SLASH-cache-<uid>
    else {
        path = "/tmp/SLASH-cache-" + std::to_string(getuid()) + "/vrt";
    }

	ensureDirExists(path);

	return path;
}

std::filesystem::path FilesystemCache::getRuntimePath() {
    std::filesystem::path path;

    // 1. Try $SLASH_RUNTIME_PATH
    if (const char* slashCache = std::getenv("SLASH_RUNTIME_PATH")) {
        path = slashCache;
    }

    // 2. Try XDG_RUNTIME_DIR/SLASH
    else if (const char* xdgCache = std::getenv("XDG_RUNTIME_DIR")) {
        path = std::filesystem::path(xdgCache) / "SLASH" / "vrt";
    }

    // 3. Fallback: /tmp/SLASH-run-<uid>
    else {
        path = "/tmp/SLASH-run-" + std::to_string(getuid()) + "/vrt";
    }

	ensureDirExists(path);

	return path;
}

void FilesystemCache::ensureDirExists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        throw std::system_error(ec);
    }
}
