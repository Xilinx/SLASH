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

#include "system_map.h" // VbinError / VbinResult

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace slash_sysemu {

// ─────────────────────────────────────────────────────────────────────────────
// VbinStore — per-BDF on-disk main + staging VBIN files
// ─────────────────────────────────────────────────────────────────────────────
//
// Each emulated accelerator owns two VBIN files on disk:
//
//   * <base>/<bdf>/main.vbin    — the last successfully launched model program.
//   * <base>/<bdf>/staging.vbin — user-written reconfiguration buffer (the QDMA
//                                 subsystem appends chunks here at device address
//                                 0x102100000).
//
// Why the FULL VBIN is staged (not just the DCP/PDI bitstream real hardware
// receives over QDMA): the daemon needs the VBIN's system map to tell whether the
// target platform is emulation or simulation and to reverse the register→address
// mapping, and the simulation model itself ships as a separate shared object that
// the `vpp_sim` executable only wraps.  So a reconfiguration must carry the whole
// container, not just the programmable-logic image.  VRT writes it in ≤64 KiB
// chunks (always at device address 0x102100000), then REMOVEs PF2 and RESCANs to
// launch the staged model.
//
// Lifecycle rules from the spec:
//   * The files PERSIST across accelerator teardown — tearing an accelerator down
//     must NOT remove them (they are needed for a later RESCAN).  They are only
//     removed on daemon startup/shutdown ("cold reboot"), modelled here as the
//     explicit cold_reboot_cleanup() call (invoked by the daemon, never by
//     per-accelerator teardown).
//   * bootstrap(): if no main.vbin exists yet, copy the configured default VBIN
//     into main.vbin and create an empty staging.vbin.  If main already exists,
//     bootstrap only ensures a staging file exists and is left untouched.
//   * The staging file is "empty" when it has zero length.
//   * replace_main_with_staging(): atomically make staging.vbin the new main
//     (rename), then recreate an empty staging.vbin.
//   * clear_staging(): truncate staging.vbin back to zero length (do NOT delete).
//
// All operations that touch the filesystem return VbinResult<...> with a
// VbinErrorKind::Io on failure, matching the VBIN error taxonomy (vbin.h).

class VbinStore {
public:
    /**
     * @brief Construct a store for @p bdf rooted at @p base_dir.
     *
     * No filesystem side effects occur in the constructor; call bootstrap()
     * (which creates <base>/<bdf>/) before the other operations.  @p bdf is used
     * verbatim as a directory component (callers pass a validated BoardBdf
     * string, which contains only hex digits and colons).
     */
    VbinStore(std::filesystem::path base_dir, std::string bdf);

    /** Path to the per-accelerator directory (<base>/<bdf>). */
    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }
    /** Path to main.vbin (may not exist yet). */
    [[nodiscard]] const std::filesystem::path& main_path() const noexcept { return main_; }
    /** Path to staging.vbin (may not exist yet). */
    [[nodiscard]] const std::filesystem::path& staging_path() const noexcept { return staging_; }

    /** True if main.vbin exists and is a regular file. */
    [[nodiscard]] bool has_main() const;
    /** True if staging.vbin exists and has non-zero length. */
    [[nodiscard]] bool staging_nonempty() const;

    /**
     * @brief Ensure the per-accelerator directory and the two VBIN files exist.
     *
     * Creates <base>/<bdf>/ if needed.  If main.vbin does not yet exist, copies
     * @p default_vbin into it (the "default VBIN" bootstrap path) and creates an
     * empty staging.vbin.  If main.vbin already exists, leaves it untouched and
     * only ensures an (empty) staging.vbin exists.
     *
     * @param default_vbin  Path to the default VBIN to seed a fresh accelerator.
     * @return ok on success; Io error if the directory or files cannot be
     *         created, or if @p default_vbin is needed but cannot be read.
     */
    VbinResult<void> bootstrap(const std::filesystem::path& default_vbin);

    /**
     * @brief Append @p bytes to staging.vbin (creating it if absent).
     *
     * Models the QDMA reconfiguration-aperture write path: each written
     * chunk is appended to the staging VBIN file.
     *
     * ACCEPTED INACCURACY (non-atomic reconfiguration): staging writes and the
     * PF2 REMOVE/RESCAN that consumes them are separate, unsynchronised user
     * operations.  A user that writes only SOME chunks and then triggers a RESCAN
     * makes PF2 restoration evaluate an incomplete VBIN.  The daemon cannot
     * safeguard against this beyond rejecting a staging VBIN that fails to
     * unpack/parse; avoiding a partial-but-plausible image is the user's
     * responsibility.
     */
    VbinResult<void> append_staging(std::span<const uint8_t> bytes);

    /** @brief Read the entire staging.vbin contents (empty vector if absent). */
    [[nodiscard]] VbinResult<std::vector<uint8_t>> read_staging() const;

    /** @brief Truncate staging.vbin to zero length (does not delete the file). */
    VbinResult<void> clear_staging();

    /**
     * @brief Atomically replace main.vbin with the current staging.vbin.
     *
     * Renames staging.vbin → main.vbin (atomic within the same directory), then
     * recreates an empty staging.vbin.  Called after a staging VBIN has been
     * successfully launched.
     */
    VbinResult<void> replace_main_with_staging();

    /**
     * @brief Remove the entire per-accelerator directory ("cold reboot").
     *
     * Deletes <base>/<bdf>/ and both VBIN files.  Per the spec this is ONLY
     * called during daemon startup/shutdown, never during per-accelerator
     * teardown (the files must survive a teardown for a later RESCAN).
     */
    VbinResult<void> cold_reboot_cleanup();

private:
    std::filesystem::path dir_;
    std::filesystem::path main_;
    std::filesystem::path staging_;
};

} // namespace slash_sysemu
