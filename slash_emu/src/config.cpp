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
#include <charconv>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>
#include <stdexcept>
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

namespace slash_emu {

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
// uid/gid resolution
// ─────────────────────────────────────────────────────────────────────────────

bool resolve_uid(const std::string& name_or_id, uid_t& out) {
    if (name_or_id.empty()) return false;

    // Try numeric parse first.
    if (std::isdigit(static_cast<unsigned char>(name_or_id[0]))) {
        unsigned long v{};
        auto [ptr, ec] = std::from_chars(name_or_id.data(),
                                         name_or_id.data() + name_or_id.size(), v);
        if (ec == std::errc{} && ptr == name_or_id.data() + name_or_id.size()) {
            out = static_cast<uid_t>(v);
            return true;
        }
    }

    // Name lookup.
    errno = 0;
    struct passwd* pw = ::getpwnam(name_or_id.c_str());
    if (pw) {
        out = pw->pw_uid;
        return true;
    }
    return false;
}

bool resolve_gid(const std::string& name_or_id, gid_t& out) {
    if (name_or_id.empty()) return false;

    // Try numeric parse first.
    if (std::isdigit(static_cast<unsigned char>(name_or_id[0]))) {
        unsigned long v{};
        auto [ptr, ec] = std::from_chars(name_or_id.data(),
                                         name_or_id.data() + name_or_id.size(), v);
        if (ec == std::errc{} && ptr == name_or_id.data() + name_or_id.size()) {
            out = static_cast<gid_t>(v);
            return true;
        }
    }

    // Group name lookup.
    errno = 0;
    struct group* gr = ::getgrnam(name_or_id.c_str());
    if (gr) {
        out = gr->gr_gid;
        return true;
    }
    return false;
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

    CLI::App app{"slash_emu — SLASH system emulation daemon"};
    app.set_version_flag("--version", "slash_emu 0.1.0");

    // ---- Options ----

    std::string config_path;
    app.add_option("-c,--config", config_path,
                   "Path to the INI configuration file")
       ->required()
       ->check(CLI::ExistingFile);

    std::string base_dir = "/run/slash_emu";
    app.add_option("-d,--base-dir", base_dir,
                   "Base directory for emulation sockets (default: /run/slash_emu)");

    // Daemon-wide default VBIN used to bootstrap a fresh accelerator (Step 6).
    // Optional: left unset when not provided.
    std::string default_vbin;
    bool        default_vbin_set = false;
    app.add_option("--default-vbin", default_vbin,
                   "Daemon-wide default VBIN used to bootstrap a fresh accelerator")
       ->each([&default_vbin_set](const std::string&) { default_vbin_set = true; });

    std::string uid_str = "vrtd";
    bool uid_is_default = true;
    app.add_option("-u,--uid", uid_str,
                   "Owner UID of created sockets; name or integer (default: vrtd)")
       ->each([&uid_is_default](const std::string&) { uid_is_default = false; });

    std::string gid_str = "vrt";
    bool gid_is_default = true;
    app.add_option("-g,--gid", gid_str,
                   "Owner GID of created sockets; name or integer (default: vrt)")
       ->each([&gid_is_default](const std::string&) { gid_is_default = false; });

    // Accept mode as a string so we can parse it as octal.
    std::string mode_str = "600";
    app.add_option("-m,--mode", mode_str,
                   "Permission mode for sockets in octal (default: 600)");

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

    // ---- Resolve uid ----
    uid_t uid{};
    if (!resolve_uid(uid_str, uid)) {
        if (uid_is_default) {
            // The default "vrtd" doesn't exist — fall back to current uid with warning.
            uid = ::getuid();
            std::fprintf(stderr,
                         "slash_emu: warning: user 'vrtd' not found; "
                         "falling back to current uid %u\n",
                         static_cast<unsigned>(uid));
        } else {
            result.error = "unknown user or invalid UID '" + uid_str + "'";
            return result;
        }
    }

    // ---- Resolve gid ----
    gid_t gid{};
    if (!resolve_gid(gid_str, gid)) {
        if (gid_is_default) {
            // The default "vrt" doesn't exist — fall back to current gid with warning.
            gid = ::getgid();
            std::fprintf(stderr,
                         "slash_emu: warning: group 'vrt' not found; "
                         "falling back to current gid %u\n",
                         static_cast<unsigned>(gid));
        } else {
            result.error = "unknown group or invalid GID '" + gid_str + "'";
            return result;
        }
    }

    // ---- Parse mode (octal) ----
    mode_t mode{};
    {
        unsigned long v{};
        // strtoul with base 8 to parse octal strings like "600" or "0600".
        char* endptr = nullptr;
        errno = 0;
        v = std::strtoul(mode_str.c_str(), &endptr, 8);
        if (errno != 0 || endptr == mode_str.c_str() || *endptr != '\0' || v > 07777) {
            result.error = "invalid permission mode '" + mode_str +
                           "' (expected octal digits, e.g. 600 or 0600)";
            return result;
        }
        mode = static_cast<mode_t>(v);
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
    result.config.uid         = uid;
    result.config.gid         = gid;
    result.config.mode        = mode;
    result.config.config_file = config_path;
    if (default_vbin_set) {
        result.config.default_vbin_path = std::move(default_vbin);
    }
    result.config.accelerators = std::move(file_result.accelerators);

    result.ok = true;
    return result;
}

} // namespace slash_emu
