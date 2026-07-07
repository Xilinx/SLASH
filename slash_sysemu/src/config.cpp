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

#include "config.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <map>
#include <unordered_set>

#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

// ini.h uses INI_API which may be an empty macro outside Windows; include it
// before any namespace so that ini_parse_file is in the global namespace.
extern "C" {
#include <ini.h>
}

#include <CLI/CLI.hpp>

namespace slash_sysemu {

// ─────────────────────────────────────────────────────────────────────────────
// BoardBdf
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Regex pattern for a board BDF: exactly 4+2+2 hex digits, colon-separated,
// no trailing characters (the $ is implicit via regex_match).
const std::regex& bdf_regex() {
    static const std::regex kRe{R"([0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2})",
                                std::regex::optimize};
    return kRe;
}

} // namespace

std::optional<BoardBdf> BoardBdf::parse(const std::string& raw) {
    // The canonical board BDF is exactly 10 characters: "DDDD:BB:DD".
    // A function suffix (e.g. ".2") makes the string longer and is rejected by
    // the length check before the regex is even evaluated.
    if (raw.size() != 10) return std::nullopt;
    if (!std::regex_match(raw, bdf_regex())) return std::nullopt;
    return BoardBdf{raw};
}

// ─────────────────────────────────────────────────────────────────────────────
// BDF validation (free function)
// ─────────────────────────────────────────────────────────────────────────────

bool is_valid_board_bdf(const std::string& bdf) {
    return BoardBdf::parse(bdf).has_value();
}

// ─────────────────────────────────────────────────────────────────────────────
// Socket path helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string socket_path_ctl(const DaemonConfig& cfg, std::size_t n) {
    return cfg.base_dir + "/slash_ctl" + std::to_string(n);
}

std::string socket_path_qdma_ctl(const DaemonConfig& cfg, std::size_t n) {
    return cfg.base_dir + "/slash_qdma_ctl" + std::to_string(n);
}

std::string socket_path_hotplug(const DaemonConfig& cfg) {
    return cfg.base_dir + "/slash_hotplug";
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration file parsing
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr std::string_view kDevicePrefix = "device.";

// Accumulator for key=value pairs, keyed by section name, populated during the
// inih parse pass.  Used to read per-device keys such as `vbin_path` without a
// second file scan.  (Section discovery still relies on the line scan, since
// inih does not report empty/keyless sections.)
struct KeyValueCollector {
    // section -> (key -> value); last write wins for duplicate keys.
    std::map<std::string, std::map<std::string, std::string>> values;
};

// ini_parse_file callback: records every name=value pair under its section.
// Still returns 1 (continue) so ini_parse_file's return code keeps reporting
// syntax errors exactly as before.
static int collect_handler(void* user, const char* section, const char* name,
                           const char* value) {
    auto* collector = static_cast<KeyValueCollector*>(user);
    if (collector != nullptr && section != nullptr && name != nullptr) {
        collector->values[section][name] = value != nullptr ? value : "";
    }
    return 1;
}

} // namespace

ConfigFileResult parse_config_file(const std::string& path) {
    ConfigFileResult result;

    // Open the file once; we use it for both our line-scanning pass and the
    // inih syntax-validation pass, avoiding a TOCTOU window between two opens.
    std::FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) {
        result.error = "cannot open configuration file '" + path + "': " + std::strerror(errno);
        return result;
    }

    // Pass 1 (line scan): collect section names.
    //
    // inih's callback is invoked only for name=value pairs; sections that
    // contain only comments (or no keys) are invisible when
    // INI_CALL_HANDLER_ON_NEW_SECTION=0 (the default).  We therefore scan
    // lines directly: any line starting with '[' and containing ']' is a
    // section header.  Duplicates are preserved so the processing loop can
    // detect duplicate device BDFs.
    std::vector<std::string> sections;
    {
        char buf[1024];
        while (std::fgets(buf, sizeof(buf), fp)) {
            std::string_view sv(buf);

            // Trim leading whitespace.
            std::size_t start = 0;
            while (start < sv.size() && (sv[start] == ' ' || sv[start] == '\t')) ++start;

            if (start >= sv.size() || sv[start] != '[') continue;

            std::size_t end = sv.find(']', start + 1);
            if (end == std::string_view::npos) continue;

            sections.emplace_back(sv.substr(start + 1, end - start - 1));
        }
    }

    // Pass 2 (ini_parse_file): syntax validation.
    // ini_parse_file returns >0 with the 1-based line number of a parse error,
    // or 0 on success.  We rewind the FILE* to reuse the same open file handle.
    std::rewind(fp);
    KeyValueCollector collector;
    int rc = ::ini_parse_file(fp, collect_handler, &collector);
    std::fclose(fp);

    if (rc > 0) {
        result.error = "parse error in configuration file '" + path +
                       "' at line " + std::to_string(rc);
        return result;
    }

    // Process collected sections.
    std::unordered_set<std::string> seen_bdfs;

    for (const auto& section : sections) {
        // Only "device.<BDF>" sections define accelerators.
        if (section.size() <= kDevicePrefix.size() ||
            section.substr(0, kDevicePrefix.size()) != kDevicePrefix) {
            // Unknown sections are silently ignored to allow forward compatibility.
            continue;
        }

        std::string bdf_str = section.substr(kDevicePrefix.size());
        auto bdf = BoardBdf::parse(bdf_str);

        if (!bdf) {
            result.error = "invalid board BDF '" + bdf_str + "' in section [" + section + "]";
            return result;
        }

        if (!seen_bdfs.insert(bdf_str).second) {
            result.error = "duplicate board BDF '" + bdf_str + "' in configuration file";
            return result;
        }

        // Per-device optional `vbin_path` override (used by Step 6 as this
        // accelerator's default VBIN source, taking precedence over the daemon
        // wide default_vbin_path).  Absent or empty → nullopt.
        std::optional<std::string> vbin_path;
        if (auto sec_it = collector.values.find(section); sec_it != collector.values.end()) {
            if (auto kv = sec_it->second.find("vbin_path");
                kv != sec_it->second.end() && !kv->second.empty()) {
                vbin_path = kv->second;
            }
        }

        result.accelerators.push_back(AcceleratorConfig{std::move(*bdf), std::move(vbin_path)});
    }

    result.ok = true;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// CLI parsing
// ─────────────────────────────────────────────────────────────────────────────

CliResult parse_cli(int argc, char* argv[]) {
    CliResult result;

    CLI::App app{"slash_sysemu — SLASH system emulation daemon"};
    app.set_version_flag("--version", "slash_sysemu 0.1.0");

    // ---- Options ----

    std::string config_path;
    app.add_option("-c,--config", config_path,
                   "Path to the INI configuration file")
       ->required()
       ->check(CLI::ExistingFile);

    // Default the socket base directory to the systemd-provided RuntimeDirectory
    // ($RUNTIME_DIRECTORY, e.g. /run/slash_sysemu) when present, else the well-known
    // path.  systemd creates and tears down this directory; -d overrides it
    // (used by tests, which run without a RuntimeDirectory).
    std::string base_dir = "/run/slash_sysemu";
    if (const char* rd = std::getenv("RUNTIME_DIRECTORY"); rd != nullptr && rd[0] != '\0') {
        // RUNTIME_DIRECTORY may be a colon-separated list; take the first entry.
        std::string_view rv{rd};
        base_dir.assign(rv.substr(0, rv.find(':')));
    }
    app.add_option("-d,--base-dir", base_dir,
                   "Base directory for emulation sockets "
                   "(default: $RUNTIME_DIRECTORY, else /run/slash_sysemu)");

    // Daemon-wide default VBIN used to bootstrap a fresh accelerator (Step 6).
    // Precedence (low → high): compiled-in installed default, $SLASH_SYSEMU_DEFAULT_VBIN,
    // then the --default-vbin flag.  Left unset only if none of these apply.
    std::string default_vbin;
    bool        default_vbin_set = false;
#ifdef SLASH_SYSEMU_DEFAULT_VBIN_PATH
    default_vbin     = SLASH_SYSEMU_DEFAULT_VBIN_PATH;
    default_vbin_set = true;
#endif
    if (const char* dv = std::getenv("SLASH_SYSEMU_DEFAULT_VBIN"); dv != nullptr && dv[0] != '\0') {
        default_vbin     = dv;
        default_vbin_set = true;
    }
    app.add_option("--default-vbin", default_vbin,
                   "Daemon-wide default VBIN used to bootstrap a fresh accelerator "
                   "(default: the installed VBIN, or $SLASH_SYSEMU_DEFAULT_VBIN)")
       ->each([&default_vbin_set](const std::string&) { default_vbin_set = true; });

    // ---- Parse ----

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        // --help was passed; CLI11 already printed the help.
        result.ok        = true;
        result.exit_code = 0;
        return result;
    } catch (const CLI::CallForVersion&) {
        result.ok        = true;
        result.exit_code = 0;
        return result;
    } catch (const CLI::ParseError& e) {
        result.error = e.what();
        return result;
    }

    // ---- Validate base_dir (must be absolute) ----
    if (base_dir.empty() || base_dir[0] != '/') {
        result.error = "base-dir '" + base_dir + "' is not an absolute path";
        return result;
    }

    // ---- Parse the config file ----
    auto file_result = parse_config_file(config_path);
    if (!file_result.ok) {
        result.error = file_result.error;
        return result;
    }

    // ---- Require at least one accelerator ----
    if (file_result.accelerators.empty()) {
        result.error = "configuration file '" + config_path +
                       "' contains no [device.*] sections; at least one accelerator required";
        return result;
    }

    // ---- Assemble the config ----
    result.config.base_dir    = std::move(base_dir);
    result.config.config_file = config_path;
    if (default_vbin_set) {
        result.config.default_vbin_path = std::move(default_vbin);
    }
    result.config.accelerators = std::move(file_result.accelerators);

    result.ok = true;
    return result;
}

} // namespace slash_sysemu
