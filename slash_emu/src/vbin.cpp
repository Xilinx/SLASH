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

#include "vbin.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <zlib.h>

namespace slash_emu {

// ─────────────────────────────────────────────────────────────────────────────
// TempDir
// ─────────────────────────────────────────────────────────────────────────────

void TempDir::reset() noexcept {
    if (owned_ && !path_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);  // best-effort; nothing to do on failure
    }
    owned_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internals
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr std::size_t kTarBlockSize = 512;
constexpr char        kTarLongNameType = 'L';

// A sane upper bound on any single VBIN member and on the whole decompressed
// archive, guarding against decompression bombs and absurd tar size fields.
constexpr uint64_t kMaxMemberBytes  = 8ULL * 1024 * 1024 * 1024;   // 8 GiB
constexpr uint64_t kMaxArchiveBytes = 16ULL * 1024 * 1024 * 1024;  // 16 GiB

struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};
static_assert(sizeof(TarHeader) == kTarBlockSize, "tar header must be 512 bytes");

VbinError io_err(std::string m) { return VbinError{VbinErrorKind::Io, std::move(m)}; }
VbinError archive_err(std::string m) { return VbinError{VbinErrorKind::Archive, std::move(m)}; }
VbinError contents_err(std::string m) { return VbinError{VbinErrorKind::Contents, std::move(m)}; }

bool is_zero_block(const uint8_t* p) {
    for (std::size_t i = 0; i < kTarBlockSize; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

// Parse an octal tar numeric field; returns nullopt on a non-octal digit.
std::optional<uint64_t> parse_octal(const char* field, std::size_t len) {
    uint64_t value = 0;
    bool seen_digit = false;
    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(field[i]);
        if (c == '\0' || c == ' ') {
            if (seen_digit) break;
            continue;
        }
        if (c < '0' || c > '7') return std::nullopt;
        seen_digit = true;
        value = (value << 3) | static_cast<uint64_t>(c - '0');
    }
    return value;
}

std::string read_field(const char* field, std::size_t len) {
    std::size_t n = 0;
    while (n < len && field[n] != '\0') ++n;
    return std::string(field, field + n);
}

// Verify the tar header checksum (chksum field summed as spaces).
bool valid_checksum(const uint8_t* raw) {
    TarHeader header{};
    std::memcpy(&header, raw, sizeof(header));
    auto expected = parse_octal(header.chksum, sizeof(header.chksum));
    if (!expected) return false;
    uint64_t actual = 0;
    for (std::size_t i = 0; i < kTarBlockSize; ++i) {
        actual += (i >= 148 && i < 156) ? static_cast<unsigned char>(' ') : raw[i];
    }
    return *expected == actual;
}

// Normalise an entry name and reject absolute paths / ".." traversal.
// Returns nullopt on rejection (with *err populated); an empty (but engaged)
// path means "skip this entry" (e.g. "." or a pure directory root).
std::optional<std::filesystem::path> sanitize_path(const std::string& name, VbinError& err) {
    std::filesystem::path path = std::filesystem::path(name).lexically_normal();
    if (path.empty() || path == ".") return std::filesystem::path{};
    if (path.is_absolute()) {
        err = archive_err("VBIN tar contains absolute path entry: '" + name + "'");
        return std::nullopt;
    }
    for (const auto& part : path) {
        if (part == "..") {
            err = archive_err("VBIN tar contains parent-directory traversal: '" + name + "'");
            return std::nullopt;
        }
    }
    return path;
}

bool is_regular_type(char typeflag) {
    return typeflag == '0' || typeflag == '\0';
}

bool is_dir_type(char typeflag) { return typeflag == '5'; }

// Read an entire file into memory.
std::optional<std::vector<uint8_t>> read_whole_file(const std::string& path, VbinError& err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        err = io_err("cannot open VBIN file '" + path + "'");
        return std::nullopt;
    }
    const std::streamoff size = f.tellg();
    if (size < 0) {
        err = io_err("cannot determine size of VBIN file '" + path + "'");
        return std::nullopt;
    }
    if (static_cast<uint64_t>(size) > kMaxArchiveBytes) {
        err = archive_err("VBIN file '" + path + "' exceeds the maximum supported size");
        return std::nullopt;
    }
    std::vector<uint8_t> data(static_cast<std::size_t>(size));
    f.seekg(0);
    if (size > 0 && !f.read(reinterpret_cast<char*>(data.data()), size)) {
        err = io_err("failed reading VBIN file '" + path + "'");
        return std::nullopt;
    }
    return data;
}

// gzip-decompress @p in into a byte buffer, bounded by kMaxArchiveBytes.
std::optional<std::vector<uint8_t>> gunzip(const std::vector<uint8_t>& in, VbinError& err) {
    z_stream zs{};
    // 15 + 16 selects gzip decoding with the default window size.
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {
        err = archive_err("failed to initialise gzip decompressor");
        return std::nullopt;
    }
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());

    std::vector<uint8_t> out;
    std::array<uint8_t, 1 << 16> chunk{};
    for (;;) {
        zs.next_out = chunk.data();
        zs.avail_out = static_cast<uInt>(chunk.size());
        const int rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&zs);
            err = archive_err("VBIN gzip stream is corrupt");
            return std::nullopt;
        }
        const std::size_t produced = chunk.size() - zs.avail_out;
        if (out.size() + produced > kMaxArchiveBytes) {
            inflateEnd(&zs);
            err = archive_err("VBIN gzip stream expands beyond the maximum supported size");
            return std::nullopt;
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + produced);

        if (rc == Z_STREAM_END) {
            // End of one gzip member. `gzip -c a; gzip -c b >> f` concatenates
            // independent members that all belong to the same logical stream, so
            // if input remains we must reset and decode the next member.
            if (zs.avail_in == 0) break;  // whole input consumed — done.
            // A gzip member always starts with the magic 0x1F 0x8B. Bytes that
            // are not another member are trailing garbage to be TOLERATED (a
            // valid tar/gzip followed by junk is accepted), not decoded.
            if (zs.avail_in < 2 || zs.next_in[0] != 0x1F || zs.next_in[1] != 0x8B) break;
            if (inflateReset(&zs) != Z_OK) {
                inflateEnd(&zs);
                err = archive_err("VBIN gzip stream is corrupt");
                return std::nullopt;
            }
        }
    }

    inflateEnd(&zs);
    return out;
}

// Write @p data (len bytes) to @p out_path, creating parent directories.
std::optional<VbinError> write_member(const std::filesystem::path& out_path, const uint8_t* data,
                                      std::size_t len) {
    std::error_code ec;
    const auto parent = out_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return io_err("failed to create directory '" + parent.string() + "': " + ec.message());
        }
    }
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return io_err("failed to create extracted file '" + out_path.string() + "'");
    }
    if (len > 0) {
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    }
    out.flush();
    if (!out) {
        return io_err("failed writing extracted file '" + out_path.string() + "'");
    }
    return std::nullopt;
}

// Extract a raw (uncompressed) tar image held in @p tar into @p dest_dir.
std::optional<VbinError> extract_tar(const std::vector<uint8_t>& tar,
                                     const std::filesystem::path& dest_dir) {
    std::size_t pos = 0;
    std::string pending_long_name;

    while (pos + kTarBlockSize <= tar.size()) {
        const uint8_t* raw = tar.data() + pos;
        if (is_zero_block(raw)) break;  // end-of-archive marker
        if (!valid_checksum(raw)) {
            return archive_err("VBIN tar header checksum mismatch (corrupt archive)");
        }

        TarHeader header{};
        std::memcpy(&header, raw, sizeof(header));
        pos += kTarBlockSize;

        auto payload_size = parse_octal(header.size, sizeof(header.size));
        if (!payload_size) {
            return archive_err("VBIN tar header has a malformed size field");
        }
        if (*payload_size > kMaxMemberBytes) {
            return archive_err("VBIN tar member exceeds the maximum supported size");
        }
        const uint64_t size = *payload_size;
        const uint64_t padded = ((size + kTarBlockSize - 1) / kTarBlockSize) * kTarBlockSize;

        if (padded > tar.size() - pos) {
            return archive_err("VBIN tar is truncated (member payload runs past end of archive)");
        }
        const uint8_t* payload = tar.data() + pos;

        const char typeflag = header.typeflag;
        if (typeflag == kTarLongNameType) {
            // GNU long name: the payload is the name of the *next* entry.
            std::string long_name(reinterpret_cast<const char*>(payload),
                                  static_cast<std::size_t>(size));
            if (auto nul = long_name.find('\0'); nul != std::string::npos) {
                long_name.resize(nul);
            }
            pending_long_name = std::move(long_name);
            pos += static_cast<std::size_t>(padded);
            continue;
        }

        std::string entry_name;
        if (!pending_long_name.empty()) {
            entry_name = std::move(pending_long_name);
            pending_long_name.clear();
        } else {
            const std::string name = read_field(header.name, sizeof(header.name));
            const std::string prefix = read_field(header.prefix, sizeof(header.prefix));
            entry_name = prefix.empty() ? name : (prefix + "/" + name);
        }

        VbinError san_err{};
        auto rel = sanitize_path(entry_name, san_err);
        if (!rel) return san_err;

        if (!rel->empty()) {
            const std::filesystem::path out_path = dest_dir / *rel;
            if (is_dir_type(typeflag)) {
                std::error_code ec;
                std::filesystem::create_directories(out_path, ec);
                if (ec) {
                    return io_err("failed to create directory '" + out_path.string() +
                                  "': " + ec.message());
                }
            } else if (is_regular_type(typeflag)) {
                if (auto e = write_member(out_path, payload, static_cast<std::size_t>(size))) {
                    return e;
                }
            }
            // Other typeflags (symlinks, devices, ...) are intentionally skipped.
        }

        pos += static_cast<std::size_t>(padded);
    }
    return std::nullopt;
}

// Create a unique extraction directory under @p temp_root.
std::optional<std::filesystem::path> make_temp_dir(const std::filesystem::path& temp_root,
                                                   VbinError& err) {
    std::string tmpl = (temp_root / "slash_emu_vbin_XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
        err = io_err("failed to create temporary extraction directory under '" +
                     temp_root.string() + "'");
        return std::nullopt;
    }
    return std::filesystem::path(buf.data());
}

// Locate a file by name at the extraction root (VBIN members are flat).
std::filesystem::path find_root_file(const std::filesystem::path& dir, const std::string& name) {
    std::filesystem::path candidate = dir / name;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) &&
        std::filesystem::is_regular_file(candidate, ec)) {
        return candidate;
    }
    return {};
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

VbinResult<Vbin> unpack_vbin(const std::string& path, const std::filesystem::path& temp_root) {
    VbinError err{};

    // 1. Read the whole file.
    auto file = read_whole_file(path, err);
    if (!file) return VbinResult<Vbin>::err(std::move(err));

    // 2. Detect gzip and decompress if needed.
    std::vector<uint8_t> tar;
    if (file->size() >= 2 && (*file)[0] == 0x1F && (*file)[1] == 0x8B) {
        auto inflated = gunzip(*file, err);
        if (!inflated) return VbinResult<Vbin>::err(std::move(err));
        tar = std::move(*inflated);
    } else {
        tar = std::move(*file);
    }

    if (tar.size() < kTarBlockSize) {
        return VbinResult<Vbin>::err(
            archive_err("VBIN '" + path + "' is not a valid tar archive (too small)"));
    }

    // 3. Create the temp dir (RAII from here on) and extract.
    auto dir = make_temp_dir(temp_root, err);
    if (!dir) return VbinResult<Vbin>::err(std::move(err));

    Vbin vbin;
    vbin.temp_dir = TempDir(*dir);

    if (auto e = extract_tar(tar, *dir)) {
        return VbinResult<Vbin>::err(std::move(*e));
    }

    // 4. Locate and parse system_map.xml.
    vbin.system_map = find_root_file(*dir, "system_map.xml");
    if (vbin.system_map.empty()) {
        return VbinResult<Vbin>::err(
            contents_err("VBIN '" + path + "' does not contain system_map.xml"));
    }
    auto map = parse_system_map_file(vbin.system_map.string());
    if (!map) return VbinResult<Vbin>::err(std::move(map).error());
    vbin.map = std::move(map).value();

    // 5. Locate the platform-appropriate executable.
    switch (vbin.map.platform) {
        case Platform::Emulation:
            vbin.executable = find_root_file(*dir, "vpp_emu");
            if (vbin.executable.empty()) {
                return VbinResult<Vbin>::err(contents_err(
                    "VBIN '" + path + "' is an Emulation VBIN but is missing 'vpp_emu'"));
            }
            break;
        case Platform::Simulation:
            vbin.executable = find_root_file(*dir, "vpp_sim");
            if (vbin.executable.empty()) {
                return VbinResult<Vbin>::err(contents_err(
                    "VBIN '" + path + "' is a Simulation VBIN but is missing 'vpp_sim'"));
            }
            break;
        case Platform::Hardware:
            return VbinResult<Vbin>::err(contents_err(
                "VBIN '" + path +
                "' targets Hardware, which the emulation daemon cannot run"));
        case Platform::Unknown:
            // parse_system_map_file never yields Unknown, but be explicit.
            return VbinResult<Vbin>::err(contents_err(
                "VBIN '" + path + "' has an unknown platform"));
    }

    return VbinResult<Vbin>::ok(std::move(vbin));
}

} // namespace slash_emu
