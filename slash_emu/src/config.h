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

#include <cstdint>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace slash_emu {

// ─────────────────────────────────────────────────────────────────────────────
// BoardBdf — validated PCI board BDF
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A validated PCI board BDF string in the format "DDDD:BB:DD".
 *
 * The board BDF identifies a PCI device at the board level:
 *   - DDDD  4 hex digits (PCI domain)
 *   - BB    2 hex digits (bus number)
 *   - DD    2 hex digits (device number)
 *
 * A function suffix (e.g. ".2") is explicitly rejected; the daemon manages
 * all functions of a board under a single board BDF.
 *
 * Example valid value: "0000:61:00"
 */
class BoardBdf {
public:
    /**
     * @brief Parse and validate a board BDF string.
     *
     * Returns an engaged optional on success, or std::nullopt if @p raw is not
     * a well-formed board BDF (wrong length, non-hex digits, wrong separators,
     * or a trailing function suffix/garbage).
     */
    static std::optional<BoardBdf> parse(const std::string& raw);

    /** @brief Return the canonical string representation ("DDDD:BB:DD"). */
    const std::string& str() const noexcept { return value_; }

    /** @brief Equality: compare the underlying string. */
    bool operator==(const BoardBdf& other) const noexcept { return value_ == other.value_; }
    bool operator!=(const BoardBdf& other) const noexcept { return value_ != other.value_; }
    bool operator< (const BoardBdf& other) const noexcept { return value_ <  other.value_; }

private:
    explicit BoardBdf(std::string v) : value_(std::move(v)) {}
    std::string value_;
};

// ─────────────────────────────────────────────────────────────────────────────
// BDF validation (free function, kept for backward compatibility with tests)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Return true if @p bdf is a valid board BDF string.
 *
 * Equivalent to `BoardBdf::parse(bdf).has_value()`.
 */
bool is_valid_board_bdf(const std::string& bdf);

// ─────────────────────────────────────────────────────────────────────────────
// Per-accelerator configuration
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Configuration for one emulated accelerator.
 *
 * The `bdf` field uniquely identifies the accelerator.  Additional optional
 * fields (e.g. `vbin_path`) provide per-accelerator overrides and are left
 * empty when not specified in the configuration file; downstream steps fill
 * them in or apply defaults.
 */
struct AcceleratorConfig {
    BoardBdf bdf;  /**< Validated PCI board BDF, e.g. "0000:61:00" */

    /**
     * @brief Optional path to a pre-installed VBIN for this accelerator.
     *
     * When present, this overrides the default VBIN discovery path used by
     * Step 6 (model process lifecycle).  Left empty for most configurations.
     */
    std::optional<std::string> vbin_path;

    // Convenience accessor kept for call sites that just need the string.
    const std::string& board_bdf() const noexcept { return bdf.str(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Daemon-wide configuration
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Complete resolved configuration for the emulation daemon.
 *
 * `base_dir` is always an absolute path.  Socket ownership and permissions are
 * NOT part of this struct: under systemd the daemon runs as User=/Group= and
 * the sockets inherit that identity, while their mode comes from the unit's
 * UMask=.  The daemon therefore performs no chown/chmod of its own.
 */
struct DaemonConfig {
    /**
     * @brief Base directory where all sockets are created.
     *
     * Defaults to the systemd-provided `$RUNTIME_DIRECTORY` (i.e. /run/slash_emu
     * for `RuntimeDirectory=slash_emu`) when set, else /run/slash_emu.  Always an
     * absolute path.  systemd creates and tears down this directory, so the
     * daemon neither creates nor cold-reboot-cleans it.
     */
    std::string base_dir{"/run/slash_emu"};

    /** Path to the INI configuration file that was parsed. */
    std::string config_file;

    /**
     * @brief Daemon-wide default VBIN used to bootstrap a fresh accelerator.
     *
     * Step 6 (model process lifecycle) copies this into a new accelerator's
     * main.vbin when it has none yet.  A per-accelerator
     * AcceleratorConfig::vbin_path, when set, takes precedence over this
     * daemon-wide default.  Empty when not configured (the shipped default-VBIN
     * artifact is out of scope for the current sprint).
     */
    std::optional<std::string> default_vbin_path;

    /** Per-accelerator entries read from the configuration file. */
    std::vector<AcceleratorConfig> accelerators;

    /**
     * @brief Resolve the VBIN source used to bootstrap @p accel.
     *
     * Precedence: the accelerator's own vbin_path if set, else the daemon-wide
     * default_vbin_path.  Returns nullopt if neither is configured.
     */
    [[nodiscard]] std::optional<std::string> resolve_default_vbin(
        const AcceleratorConfig& accel) const {
        if (accel.vbin_path.has_value()) {
            return accel.vbin_path;
        }
        return default_vbin_path;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Socket path helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Compute the filesystem path of the N-th slash_ctl socket.
 *
 * Returns `<base_dir>/slash_ctl<n>`, where N is the zero-based index of the
 * accelerator in the configuration's accelerators list.
 */
std::string socket_path_ctl(const DaemonConfig& cfg, std::size_t n);

/**
 * @brief Compute the filesystem path of the N-th slash_qdma_ctl socket.
 *
 * Returns `<base_dir>/slash_qdma_ctl<n>`.
 */
std::string socket_path_qdma_ctl(const DaemonConfig& cfg, std::size_t n);

/**
 * @brief Compute the filesystem path of the daemon-level slash_hotplug socket.
 *
 * Returns `<base_dir>/slash_hotplug`.
 */
std::string socket_path_hotplug(const DaemonConfig& cfg);

// ─────────────────────────────────────────────────────────────────────────────
// Configuration file parsing
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Result of parsing the INI configuration file.
 */
struct ConfigFileResult {
    bool                       ok{false};
    std::string                error;
    std::vector<AcceleratorConfig> accelerators;
};

/**
 * @brief Parse the INI configuration file at @p path.
 *
 * The file uses the following format:
 *
 * @code
 * [device.DDDD:BB:DD]
 * # Per-accelerator section.  The BDF is embedded in the section name.
 * # Additional per-accelerator keys may be added in future steps.
 * @endcode
 *
 * Each section whose name starts with "device." is treated as an accelerator
 * entry.  The BDF is the remainder of the section name after the prefix.
 *
 * Returns a ConfigFileResult with ok==true and the parsed accelerators on
 * success, or ok==false with a human-readable error string on failure
 * (file not found, parse error, invalid BDF, duplicate BDF).
 */
ConfigFileResult parse_config_file(const std::string& path);

// ─────────────────────────────────────────────────────────────────────────────
// CLI parsing
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Result of parsing the command line.
 */
struct CliResult {
    bool        ok{false};
    std::string error;
    int         exit_code{0};  /**< Non-zero if the process should exit (--help/--version). */
    DaemonConfig config;
};

/**
 * @brief Parse command-line arguments and build a DaemonConfig.
 *
 * Recognised options:
 *   -c / --config   Path to the INI configuration file (required)
 *   -d / --base-dir Base directory for sockets, must be absolute
 *                   (default: $RUNTIME_DIRECTORY, else /run/slash_emu)
 *
 * Socket ownership and permissions are owned by systemd (User=/Group= and
 * UMask=), not by the daemon, so there are no uid/gid/mode options.
 *
 * Validation:
 *   - base_dir must be an absolute path.
 *   - At least one accelerator must be present in the configuration file.
 *
 * On --help or --version, returns ok==true, exit_code==0, and an empty config
 * (the caller should exit immediately).
 *
 * On a bad option or validation failure, returns ok==false with a human-readable
 * error message.
 *
 * @param argc  Argument count (as passed to main).
 * @param argv  Argument vector (as passed to main).
 */
CliResult parse_cli(int argc, char* argv[]);

} // namespace slash_emu
