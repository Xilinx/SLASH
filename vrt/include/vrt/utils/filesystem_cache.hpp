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

#ifndef VRT_FILESYSTEM_CACHE_HPP
#define VRT_FILESYSTEM_CACHE_HPP

#include <filesystem>

/**
 * @brief Static class for managing the filesystem cache - i.e. where trivial temporary files can be stored.
 *
 * There are two directories with different pourposes:
 * 1. Runtime directory, generally stored as tmpfs on Linux (in RAM). Locks or really small files go here.
 * 2. Cache directory, generally stored on disk.
 *
 * For more information see: https://specifications.freedesktop.org/basedir-spec/0.8/
 *
 * SLASH uses subdirectories within each of the main directories, and vrt uses a subdirectory within the SLASH
 * directory.
 *
 * See each function for a description of paths used and what enviornment variables to set to override these choices.
 * However, the default behaviour (where the user sets nothing extra) should be reasonable for most Linux installations.
 */
class FilesystemCache {
    static void ensureDirExists(const std::filesystem::path& path);
   public:
    /**
     * @brief Disable construction of static class.
     */
    FilesystemCache() = delete;

    /**
     * @brief Get path to the cache directory.
     *
     * This function creates the cache directory if it does not exist.
     *
     * The following paths are used:
     *
     * 1. $SLASH_CACHE_PATH/vrt, if $SLASH_CACHE_PATH is set in the environment.
     * 2. $XDG_CACHE_HOME/SLASH/vrt, if $XDG_CACHE_HOME is set.
     * 3. $HOME/.cache/SLASH/vrt, if $HOME is set.
     * 4. /tmp/SLASH-cache-<uid>/vrt, as a final fallback.
     */
    static std::filesystem::path getCachePath();

    /**
     * @brief Get path to the runtime directory.
     *
     * This function creates the runtime directory if it does not exist.
     *
     * The following paths are used:
     *
     * 1. $SLASH_RUNTIME_PATH/vrt, if $SLASH_RUNTIME_PATH is set in the environment.
     * 2. $XDG_RUNTIME_DIR/SLASH/vrt, if $XDG_RUNTIME_DIR is set.
     * 3. /tmp/SLASH-run-<uid>/vrt, as a final fallback.
     */
    static std::filesystem::path getRuntimePath();
};

#endif  // VRT_FILESYSTEM_CACHE_HPP
