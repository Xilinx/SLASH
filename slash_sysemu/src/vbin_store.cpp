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

#include "vbin_store.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace slash_sysemu {

namespace {

VbinError io_error(const std::string& what) {
    return VbinError{VbinErrorKind::Io, what};
}

std::string with_errno(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

// Create an empty regular file at @p p, truncating any existing content.
VbinResult<void> make_empty_file(const std::filesystem::path& p) {
    // std::ofstream with trunc creates or truncates to zero length.
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return VbinResult<void>::err(io_error(with_errno("cannot create '" + p.string() + "'")));
    }
    return VbinResult<void>::ok();
}

} // namespace

VbinStore::VbinStore(std::filesystem::path base_dir, std::string bdf)
    : dir_(base_dir / bdf), main_(dir_ / "main.vbin"), staging_(dir_ / "staging.vbin") {}

bool VbinStore::has_main() const {
    std::error_code ec;
    return std::filesystem::is_regular_file(main_, ec);
}

bool VbinStore::staging_nonempty() const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(staging_, ec)) {
        return false;
    }
    auto size = std::filesystem::file_size(staging_, ec);
    if (ec) {
        return false;
    }
    return size > 0;
}

VbinResult<void> VbinStore::bootstrap(const std::filesystem::path& default_vbin) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        return VbinResult<void>::err(
            io_error("cannot create directory '" + dir_.string() + "': " + ec.message()));
    }

    if (!has_main()) {
        // Fresh accelerator: seed main.vbin from the default VBIN.
        std::filesystem::copy_file(default_vbin, main_,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return VbinResult<void>::err(
                io_error("cannot copy default VBIN '" + default_vbin.string() + "' to '" +
                         main_.string() + "': " + ec.message()));
        }
        // A fresh accelerator starts with an empty staging buffer.
        return make_empty_file(staging_);
    }

    // main.vbin already exists (persisted across a teardown): just make sure a
    // staging file exists.  Do NOT truncate an existing staging file here — a
    // user may have written a reconfiguration buffer we have not consumed yet.
    if (!std::filesystem::exists(staging_, ec)) {
        return make_empty_file(staging_);
    }
    return VbinResult<void>::ok();
}

VbinResult<void> VbinStore::append_staging(std::span<const uint8_t> bytes) {
    std::ofstream ofs(staging_, std::ios::binary | std::ios::app);
    if (!ofs) {
        return VbinResult<void>::err(
            io_error(with_errno("cannot open staging '" + staging_.string() + "' for append")));
    }
    if (!bytes.empty()) {
        ofs.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!ofs) {
            return VbinResult<void>::err(
                io_error("cannot append to staging '" + staging_.string() + "'"));
        }
    }
    return VbinResult<void>::ok();
}

VbinResult<std::vector<uint8_t>> VbinStore::read_staging() const {
    std::error_code ec;
    if (!std::filesystem::exists(staging_, ec)) {
        return VbinResult<std::vector<uint8_t>>::ok({});
    }
    std::ifstream ifs(staging_, std::ios::binary);
    if (!ifs) {
        return VbinResult<std::vector<uint8_t>>::err(
            io_error(with_errno("cannot open staging '" + staging_.string() + "'")));
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
    if (ifs.bad()) {
        return VbinResult<std::vector<uint8_t>>::err(
            io_error("read error on staging '" + staging_.string() + "'"));
    }
    return VbinResult<std::vector<uint8_t>>::ok(std::move(data));
}

VbinResult<void> VbinStore::clear_staging() {
    return make_empty_file(staging_);
}

VbinResult<void> VbinStore::replace_main_with_staging() {
    std::error_code ec;
    // rename() within one directory is atomic and overwrites the destination.
    std::filesystem::rename(staging_, main_, ec);
    if (ec) {
        return VbinResult<void>::err(io_error("cannot replace main with staging: " + ec.message()));
    }
    // Recreate an empty staging buffer for the next reconfiguration.
    return make_empty_file(staging_);
}

VbinResult<void> VbinStore::cold_reboot_cleanup() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    if (ec) {
        return VbinResult<void>::err(
            io_error("cannot remove accelerator dir '" + dir_.string() + "': " + ec.message()));
    }
    return VbinResult<void>::ok();
}

} // namespace slash_sysemu
