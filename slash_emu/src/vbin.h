// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

#pragma once

#include "system_map.h"

#include <filesystem>
#include <string>
#include <utility>

namespace slash_emu {

// ─────────────────────────────────────────────────────────────────────────────
// TempDir — RAII owning a directory removed (recursively) on destruction
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief An owned temporary directory that is recursively removed on destruction.
 *
 * Move-only.  A default-constructed / moved-from instance owns nothing and
 * removes nothing.  The path stays valid for the lifetime of the object, which
 * is what lets Step 6 launch the extracted executable before cleanup.
 */
class TempDir {
public:
    TempDir() = default;
    explicit TempDir(std::filesystem::path path) : path_(std::move(path)), owned_(true) {}

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    TempDir(TempDir&& o) noexcept : path_(std::move(o.path_)), owned_(o.owned_) {
        o.owned_ = false;
    }
    TempDir& operator=(TempDir&& o) noexcept {
        if (this != &o) {
            reset();
            path_ = std::move(o.path_);
            owned_ = o.owned_;
            o.owned_ = false;
        }
        return *this;
    }

    ~TempDir() { reset(); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool valid() const noexcept { return owned_; }

    /** Relinquish ownership; the directory will no longer be auto-removed. */
    std::filesystem::path release() noexcept {
        owned_ = false;
        return path_;
    }

    /** Remove the owned directory now (if any) and clear ownership. */
    void reset() noexcept;

private:
    std::filesystem::path path_;
    bool                  owned_{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// Vbin — an unpacked VBIN container
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Handle to an unpacked VBIN: temp dir, executable path, and system map.
 *
 * The container (a POSIX tar, optionally gzip-compressed) is extracted into a
 * unique temporary directory owned by @ref temp_dir; when the Vbin is destroyed
 * the directory and its contents are removed.  Extraction-to-disk (rather than
 * in-memory parsing) is deliberate: Step 6 must exec the emulation/simulation
 * binary and reference the xsim.dir tree, which only exist on disk.
 */
struct Vbin {
    TempDir               temp_dir;      /**< Owns the extraction directory. */
    std::filesystem::path system_map;    /**< Path to the extracted system_map.xml. */
    std::filesystem::path executable;    /**< Path to vpp_emu / vpp_sim. */
    SystemMap             map;           /**< Parsed system map. */
};

/**
 * @brief Unpack and parse a VBIN file at @p path.
 *
 * Steps:
 *   1. Detect gzip (magic 0x1F 0x8B) and, if present, decompress; otherwise
 *      treat the input as a raw tar archive.
 *   2. Safely extract members into a fresh temp dir under @p temp_root
 *      (default: the system temp directory).  Absolute paths, ".." traversal,
 *      oversized/corrupt headers, and bad checksums are rejected.
 *   3. Locate and parse system_map.xml.
 *   4. Locate the platform-appropriate executable (vpp_emu for Emulation,
 *      vpp_sim for Simulation).  Hardware VBINs have no such executable and are
 *      reported as a Contents error (the daemon does not run Hardware VBINs).
 *
 * @return VbinResult<Vbin> with the unpacked handle, or a VbinError describing
 *         which file/what was wrong (kind Io / Archive / Contents / Parse).
 */
VbinResult<Vbin> unpack_vbin(const std::string& path,
                             const std::filesystem::path& temp_root = std::filesystem::temp_directory_path());

} // namespace slash_emu
