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

#include <gtest/gtest.h>

#include "vbin.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "fixtures_paths.h"

namespace slash_sysemu {
namespace {

using namespace slash_sysemu::test_fixtures;
namespace fs = std::filesystem;

// RAII temp file that writes raw bytes and unlinks on destruction.
class RawTempFile {
public:
    explicit RawTempFile(const std::vector<uint8_t>& bytes) {
        char tmpl[] = "/tmp/slash_sysemu_vbin_test_XXXXXX";
        int fd = ::mkstemp(tmpl);
        EXPECT_GE(fd, 0);
        path_ = tmpl;
        if (fd >= 0) {
            if (!bytes.empty()) {
                [[maybe_unused]] auto n = ::write(fd, bytes.data(), bytes.size());
            }
            ::close(fd);
        }
    }
    ~RawTempFile() {
        if (!path_.empty()) ::unlink(path_.c_str());
    }
    RawTempFile(const RawTempFile&) = delete;
    RawTempFile& operator=(const RawTempFile&) = delete;
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

bool file_exists(const char* p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// ── In-memory USTAR builder (adversary probes) ───────────────────────────────
//
// Builds raw (uncompressed) tar images byte-for-byte so we can craft headers the
// `tar` CLI will not emit: signed checksums, PAX extended headers, empty GNU
// long-name payloads, backslash names, octal-trap contents, etc.

constexpr std::size_t kBlock = 512;

// A single POSIX/ustar header + its (padded) payload appended to `out`.
// typeflag '0' regular, '5' dir, 'L' GNU longname, 'x'/'g' PAX. If
// signed_chksum is true, the checksum field is written using a signed byte sum
// (as historical `star` did) to test signed-checksum tolerance.
void append_member(std::vector<uint8_t>& out, const std::string& name,
                   const std::string& payload, char typeflag = '0',
                   bool signed_chksum = false, const std::string& prefix = "") {
    std::array<uint8_t, kBlock> hdr{};
    auto put = [&](std::size_t off, const std::string& s, std::size_t cap) {
        for (std::size_t i = 0; i < s.size() && i < cap; ++i)
            hdr[off + i] = static_cast<uint8_t>(s[i]);
    };
    auto put_octal = [&](std::size_t off, uint64_t v, std::size_t width) {
        // width includes trailing NUL; emit (width-1) octal digits, space/NUL term.
        std::string s(width - 1, '0');
        for (std::size_t i = width - 1; i-- > 0;) {
            s[i] = static_cast<char>('0' + (v & 7));
            v >>= 3;
        }
        put(off, s, width - 1);
        hdr[off + width - 1] = '\0';
    };

    put(0, name, 100);
    put_octal(100, 0644, 8);   // mode
    put_octal(108, 0, 8);      // uid
    put_octal(116, 0, 8);      // gid
    put_octal(124, payload.size(), 12);  // size
    put_octal(136, 0, 12);     // mtime
    hdr[156] = static_cast<uint8_t>(typeflag);
    put(257, "ustar", 6);      // magic "ustar\0"
    hdr[263] = '0';            // version "00"
    hdr[264] = '0';
    put(345, prefix, 155);

    // checksum: fill field with spaces, sum, then write octal.
    for (std::size_t i = 148; i < 156; ++i) hdr[i] = ' ';
    long sum = 0;
    for (std::size_t i = 0; i < kBlock; ++i) {
        sum += signed_chksum ? static_cast<long>(static_cast<int8_t>(hdr[i]))
                             : static_cast<long>(hdr[i]);
    }
    // chksum field: 6 octal digits, NUL, space (classic layout).
    {
        std::string s(6, '0');
        long v = sum;
        for (std::size_t i = 6; i-- > 0;) {
            s[i] = static_cast<char>('0' + (v & 7));
            v >>= 3;
        }
        put(148, s, 6);
        hdr[154] = '\0';
        hdr[155] = ' ';
    }

    out.insert(out.end(), hdr.begin(), hdr.end());
    out.insert(out.end(), payload.begin(), payload.end());
    if (const std::size_t rem = payload.size() % kBlock; rem != 0)
        out.insert(out.end(), kBlock - rem, 0);
}

void append_end(std::vector<uint8_t>& out) { out.insert(out.end(), 2 * kBlock, 0); }

// The minimal good-emu system_map.xml (mirrors the checked-in fixture).
const char* kEmuMapXml =
    "<?xml version=\"1.0\"?>\n<SystemMap>\n<Platform>Emulation</Platform>\n"
    "<ClockFrequency>250000000</ClockFrequency>\n<Kernel><Name>demo</Name>\n"
    "<BaseAddress>0x30000</BaseAddress><Range>0x1000</Range>\n"
    "<register offset=\"0x0\" name=\"CTRL\" access=\"RW\" range=\"32\"/>\n"
    "</Kernel>\n</SystemMap>\n";

// ── Real sample VBINs ────────────────────────────────────────────────────────

TEST(VbinTest, UnpacksRealEmuVbin) {
    if (!file_exists(kRealEmuVbin)) GTEST_SKIP() << "real emu vbin not present";
    auto res = unpack_vbin(kRealEmuVbin);
    ASSERT_TRUE(res) << (res ? "" : res.error().message);
    const Vbin& v = res.value();
    EXPECT_EQ(v.map.platform, Platform::Emulation);
    EXPECT_EQ(v.executable.filename(), "vpp_emu");
    EXPECT_TRUE(fs::exists(v.executable));
    EXPECT_TRUE(fs::exists(v.system_map));
    EXPECT_EQ(v.map.clock_frequency_hz, 200000000u);

    ASSERT_EQ(v.map.kernels.size(), 2u);
    const Kernel* acc = v.map.find_kernel("accumulate_0");
    ASSERT_NE(acc, nullptr);
    EXPECT_EQ(acc->base_address, 0x20200000000ull);
    EXPECT_EQ(acc->range, 0x10000u);
    EXPECT_NE(acc->register_at(0), nullptr);
    EXPECT_EQ(acc->find_register("size")->offset, 0x10u);
    EXPECT_EQ(acc->find_register("out_r")->offset, 0x18u);
    ASSERT_EQ(acc->args.size(), 2u);
    EXPECT_EQ(acc->args[0].name, "size");
    EXPECT_TRUE(acc->args[0].writable);
    EXPECT_EQ(acc->args[1].name, "out_r");
    EXPECT_TRUE(acc->args[1].readable);

    const Kernel* inc = v.map.find_kernel("increment_0");
    ASSERT_NE(inc, nullptr);
    EXPECT_EQ(inc->base_address, 0x20200010000ull);
    ASSERT_EQ(inc->connections.size(), 1u);
    EXPECT_EQ(inc->connections[0].port, "m_axi_gmem0");
    EXPECT_EQ(inc->connections[0].target, "HBM1");
    const FunctionalArg* in_r = inc->find_arg("in_r");
    ASSERT_NE(in_r, nullptr);
    EXPECT_EQ(in_r->type, "buffer");
    EXPECT_EQ(in_r->bit_width, 64u);
    EXPECT_EQ(in_r->port, "m_axi_gmem0");
}

TEST(VbinTest, UnpacksRealSimVbin) {
    if (!file_exists(kRealSimVbin)) GTEST_SKIP() << "real sim vbin not present";
    auto res = unpack_vbin(kRealSimVbin);
    ASSERT_TRUE(res) << (res ? "" : res.error().message);
    const Vbin& v = res.value();
    EXPECT_EQ(v.map.platform, Platform::Simulation);
    EXPECT_EQ(v.executable.filename(), "vpp_sim");
    EXPECT_TRUE(fs::exists(v.executable));
    EXPECT_EQ(v.map.kernels.size(), 2u);
}

TEST(VbinTest, TempDirRemovedOnDestruction) {
    if (!file_exists(kGoodEmuVbin)) GTEST_SKIP() << "fixture not present";
    fs::path dir;
    {
        auto res = unpack_vbin(kGoodEmuVbin);
        ASSERT_TRUE(res) << res.error().message;
        dir = res.value().temp_dir.path();
        EXPECT_TRUE(fs::exists(dir));
    }
    EXPECT_FALSE(fs::exists(dir)) << "temp dir should be removed by RAII";
}

// ── Raw tar vs gzip ──────────────────────────────────────────────────────────

TEST(VbinTest, UnpacksRawTarFixture) {
    ASSERT_TRUE(file_exists(kGoodEmuVbin));
    auto res = unpack_vbin(kGoodEmuVbin);
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().map.platform, Platform::Emulation);
    EXPECT_EQ(res.value().executable.filename(), "vpp_emu");
    ASSERT_EQ(res.value().map.kernels.size(), 1u);
    EXPECT_EQ(res.value().map.kernels[0].name, "demo");
}

TEST(VbinTest, UnpacksGzipFixture) {
    ASSERT_TRUE(file_exists(kGoodEmuVbinGz));
    auto res = unpack_vbin(kGoodEmuVbinGz);
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().map.platform, Platform::Emulation);
    EXPECT_EQ(res.value().map.kernels[0].name, "demo");
}

// ── Broken inputs (one distinct test each) ───────────────────────────────────

TEST(VbinTest, RejectsNotATar) {
    std::vector<uint8_t> garbage(2048, 0x5A);  // large enough, but no valid header
    RawTempFile f(garbage);
    auto res = unpack_vbin(f.path());
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Archive);
}

TEST(VbinTest, RejectsEmptyFile) {
    RawTempFile f(std::vector<uint8_t>{});
    auto res = unpack_vbin(f.path());
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Archive);
}

TEST(VbinTest, RejectsTruncatedTar) {
    // A valid good tar with its tail chopped off mid-payload.
    std::ifstream in(kGoodEmuVbin, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(in);
    const auto size = static_cast<std::size_t>(in.tellg());
    ASSERT_GT(size, 1024u);
    std::vector<uint8_t> bytes(size);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    // Keep the first header block + a bit, dropping the rest so payload runs past end.
    bytes.resize(600);
    RawTempFile f(bytes);
    auto res = unpack_vbin(f.path());
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Archive);
}

TEST(VbinTest, RejectsPathTraversal) {
    ASSERT_TRUE(file_exists(kTraversalVbin));
    auto res = unpack_vbin(kTraversalVbin);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Archive);
    EXPECT_NE(res.error().message.find("traversal"), std::string::npos);
}

TEST(VbinTest, RejectsHardwarePlatform) {
    ASSERT_TRUE(file_exists(kHardwareVbin));
    auto res = unpack_vbin(kHardwareVbin);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Contents);
    EXPECT_NE(res.error().message.find("Hardware"), std::string::npos);
}

TEST(VbinTest, HandlesGnuLongNames) {
    ASSERT_TRUE(file_exists(kLongNameVbin));
    auto res = unpack_vbin(kLongNameVbin);
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().map.platform, Platform::Emulation);
    // The deeply-nested long-name member must have been extracted.
    bool found = false;
    for (auto it = fs::recursive_directory_iterator(res.value().temp_dir.path());
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->path().filename() == "payload.bin") found = true;
    }
    EXPECT_TRUE(found);
}

TEST(VbinTest, RejectsAbsolutePath) {
    ASSERT_TRUE(file_exists(kAbsoluteVbin));
    auto res = unpack_vbin(kAbsoluteVbin);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Archive);
    EXPECT_NE(res.error().message.find("absolute"), std::string::npos);
}

TEST(VbinTest, RejectsGzipMagicButCorruptStream) {
    // gzip magic bytes followed by junk — inflate must fail.
    std::vector<uint8_t> bytes = {0x1F, 0x8B, 0x08, 0x00, 0xDE, 0xAD, 0xBE, 0xEF,
                                  0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    RawTempFile f(bytes);
    auto res = unpack_vbin(f.path());
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Archive);
}

TEST(VbinTest, RejectsMissingSystemMap) {
    ASSERT_TRUE(file_exists(kNoMapVbin));
    auto res = unpack_vbin(kNoMapVbin);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Contents);
    EXPECT_NE(res.error().message.find("system_map.xml"), std::string::npos);
}

TEST(VbinTest, RejectsMissingExecutable) {
    ASSERT_TRUE(file_exists(kNoExecVbin));
    auto res = unpack_vbin(kNoExecVbin);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Contents);
    EXPECT_NE(res.error().message.find("vpp_emu"), std::string::npos);
}

TEST(VbinTest, RejectsMissingSimExecutable) {
    ASSERT_TRUE(file_exists(kNoExecSimVbin));
    auto res = unpack_vbin(kNoExecSimVbin);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Contents);
    EXPECT_NE(res.error().message.find("vpp_sim"), std::string::npos);
}

TEST(VbinTest, RejectsNonexistentFile) {
    auto res = unpack_vbin("/nonexistent/path/to.vbin");
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Io);
}

TEST(VbinTest, RejectsDirectoryPath) {
    auto res = unpack_vbin("/tmp");
    ASSERT_FALSE(res);
    // Opening a directory for read either fails at open or yields no tar.
    EXPECT_TRUE(res.error().kind == VbinErrorKind::Io ||
                res.error().kind == VbinErrorKind::Archive);
}

// ── Adversary probes: crafted tar images ─────────────────────────────────────

// Sanity: a hand-built ustar with unsigned checksum unpacks fine (validates the
// in-memory builder before using it for negative probes).
TEST(VbinTest, HandBuiltUstarUnpacks) {
    std::vector<uint8_t> tar;
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "#!/bin/true\n");
    append_end(tar);
    RawTempFile f(tar);
    auto res = unpack_vbin(f.path());
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().map.kernels[0].name, "demo");
}

// PROBE: signed tar checksum. The standard permits interpreting header bytes as
// signed when verifying; some historical writers (star) emit a signed sum. Our
// verifier only computes the unsigned sum. This documents whether such archives
// are accepted. (For a purely-ASCII header the two sums are identical, so this
// forces a high-bit byte into the name to make them differ.)
TEST(VbinTest, SignedChecksumTarBehavior) {
    std::vector<uint8_t> tar;
    // NOTE: all header bytes here are <128, so the signed and unsigned sums are
    // identical and this MUST still unpack. A true divergence needs a high-bit
    // byte (e.g. UTF-8 name); real toolchains emit unsigned checksums, so this
    // is a regression guard rather than a bug demonstration. See report.
    append_member(tar, "system_map.xml", kEmuMapXml, '0', /*signed_chksum=*/true);
    append_member(tar, "vpp_emu", "#!/bin/true\n");
    append_end(tar);
    RawTempFile f(tar);
    auto res = unpack_vbin(f.path());
    EXPECT_TRUE(res) << (res ? "" : res.error().message);
}

// PROBE: a PAX extended header ('x') precedes the real entry. Real GNU/bsdtar
// emit these for long names / large files / xattrs. They must be skipped
// WITHOUT corrupting the following member.
TEST(VbinTest, PaxExtendedHeaderIsSkippedCleanly) {
    std::vector<uint8_t> tar;
    // A plausible PAX record: "<len> path=system_map.xml\n".
    std::string rec = " path=system_map.xml\n";
    std::string paxbody = std::to_string(rec.size() + 2) + rec;  // len prefix
    append_member(tar, "PaxHeaders/system_map.xml", paxbody, 'x');
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "#!/bin/true\n");
    append_end(tar);
    RawTempFile f(tar);
    auto res = unpack_vbin(f.path());
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().map.kernels[0].name, "demo");
}

// PROBE: a global PAX header ('g') at the front of the archive.
TEST(VbinTest, GlobalPaxHeaderIsSkippedCleanly) {
    std::vector<uint8_t> tar;
    std::string body = "30 comment=hello world here yes\n";
    append_member(tar, "pax_global_header", body, 'g');
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "#!/bin/true\n");
    append_end(tar);
    RawTempFile f(tar);
    auto res = unpack_vbin(f.path());
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().map.kernels[0].name, "demo");
}

// PROBE: GNU long-name ('L') header with an EMPTY payload, immediately followed
// by a real member. The empty pending name must not clobber the real header's
// own name. Current code checks `!pending_long_name.empty()`, so an empty long
// name silently falls back to the header name — document that behavior.
TEST(VbinTest, EmptyGnuLongNameFallsBackToHeaderName) {
    std::vector<uint8_t> tar;
    append_member(tar, "@LongLink", "", 'L');           // empty long name
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "#!/bin/true\n");
    append_end(tar);
    RawTempFile f(tar);
    auto res = unpack_vbin(f.path());
    // Falls back to "system_map.xml" from the header, so it succeeds.
    EXPECT_TRUE(res) << (res ? "" : res.error().message);
}

// PROBE: a member literally named ".." (not "a/../b"). lexically_normal keeps
// ".." as a single component; the loop must reject it as traversal.
TEST(VbinTest, RejectsBareDotDotMember) {
    std::vector<uint8_t> tar;
    append_member(tar, "..", "gotcha");
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "x");
    append_end(tar);
    RawTempFile f(tar);
    auto res = unpack_vbin(f.path());
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Archive);
}

// PROBE: "foo/../system_map.xml" normalizes to "system_map.xml" — a member that
// LOOKS like traversal but resolves inside the dir. Documents that we accept it.
TEST(VbinTest, NormalizesInteriorDotDot) {
    std::vector<uint8_t> tar;
    append_member(tar, "foo/../system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "x");
    append_end(tar);
    RawTempFile f(tar);
    auto res = unpack_vbin(f.path());
    EXPECT_TRUE(res) << (res ? "" : res.error().message);
}

// PROBE: backslash "path" — on POSIX, "..\\escape" is a single filename with no
// separator, so it CANNOT traverse. Documents that Windows-style separators are
// treated as literal characters (safe on Linux, the only supported platform).
TEST(VbinTest, BackslashNameIsLiteralComponent) {
    std::vector<uint8_t> tar;
    append_member(tar, "..\\escape.xml", "data");
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "x");
    append_end(tar);
    RawTempFile f(tar);
    auto res = unpack_vbin(f.path());
    ASSERT_TRUE(res) << res.error().message;
    // The backslash file is written literally inside the temp dir; nothing escaped.
    bool escaped = fs::exists(fs::path(res.value().temp_dir.path()).parent_path() / "escape.xml");
    EXPECT_FALSE(escaped);
}

// PROBE: ustar prefix+name joining. A name split across the 155-byte prefix and
// 100-byte name fields must be rejoined with '/'.
TEST(VbinTest, JoinsUstarPrefixAndName) {
    std::vector<uint8_t> tar;
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "x");
    // A payload member whose path is prefix + "/" + name.
    append_member(tar, "leaf.bin", "hello", '0', false, /*prefix=*/"deep/nested/dir");
    append_end(tar);
    RawTempFile f(tar);
    auto res = unpack_vbin(f.path());
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_TRUE(fs::exists(res.value().temp_dir.path() / "deep/nested/dir/leaf.bin"));
}

// PROBE: multi-member gzip stream. gzip -c concatenation produces a single file
// with two gzip members; zlib should decode both. Documents whether our gunzip
// stops after the first member (losing data) or decodes all.
TEST(VbinTest, MultiMemberGzipStream) {
    // Build a good tar, then gzip it twice and concatenate the two .gz streams.
    std::vector<uint8_t> tar;
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "x");
    append_end(tar);
    RawTempFile raw(tar);
    // Split the tar so each half becomes its own gzip member; concatenation must
    // reassemble to the full tar.
    const std::string gz = std::string(raw.path()) + ".mm.gz";
    // Compress halves separately then cat -> one file with two gzip members.
    std::string cmd = "head -c " + std::to_string(tar.size() / 2) + " '" + raw.path() +
                      "' | gzip -c > '" + gz + "'; tail -c +" +
                      std::to_string(tar.size() / 2 + 1) + " '" + raw.path() +
                      "' | gzip -c >> '" + gz + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
    auto res = unpack_vbin(gz);
    ::unlink(gz.c_str());
    // If gunzip only decodes the first member, the tar is truncated -> failure.
    EXPECT_TRUE(res) << (res ? "" : res.error().message)
                     << " (multi-member gzip may not be fully decoded)";
}

// PROBE: trailing garbage after a complete gzip stream. Real toolchains never do
// this, but robustness matters — zlib returns Z_STREAM_END and we should not
// choke on the leftover bytes.
TEST(VbinTest, TrailingGarbageAfterGzipStream) {
    std::vector<uint8_t> tar;
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "x");
    append_end(tar);
    RawTempFile raw(tar);
    const std::string gz = std::string(raw.path()) + ".tg.gz";
    std::string cmd = "gzip -c '" + std::string(raw.path()) + "' > '" + gz +
                      "'; printf 'GARBAGE' >> '" + gz + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
    auto res = unpack_vbin(gz);
    ::unlink(gz.c_str());
    // Document current behavior: does trailing garbage abort or is it ignored?
    EXPECT_TRUE(res) << (res ? "" : res.error().message)
                     << " (trailing garbage after gzip stream)";
}

// PROBE (iter-2): a valid gzip member followed by gzip-MAGIC bytes that are NOT a
// valid deflate stream. The reset path decodes it and inflate returns
// Z_DATA_ERROR -> the archive is rejected as corrupt (a deliberate, defensible
// choice: bytes claiming to be a gzip member but malformed are corruption, not
// tolerable trailing junk).
TEST(VbinTest, SecondMemberGzipMagicButCorruptIsRejected) {
    std::vector<uint8_t> tar;
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "x");
    append_end(tar);
    RawTempFile raw(tar);
    const std::string gz = std::string(raw.path()) + ".c2.gz";
    // First a valid gzip member, then gzip magic + junk (a fake second member).
    std::string cmd = "gzip -c '" + std::string(raw.path()) + "' > '" + gz +
                      "'; printf '\\037\\213\\010\\000RUBBISH!' >> '" + gz + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
    auto res = unpack_vbin(gz);
    ::unlink(gz.c_str());
    ASSERT_FALSE(res) << "corrupt second gzip member must be rejected";
    EXPECT_EQ(res.error().kind, VbinErrorKind::Archive);
}

// PROBE (iter-2): a valid gzip member followed by a TRUNCATED second member (gzip
// magic + valid-looking header but cut short). inflate cannot reach
// Z_STREAM_END and must terminate (not hang) and be rejected as corrupt.
TEST(VbinTest, TruncatedSecondGzipMemberIsRejected) {
    std::vector<uint8_t> tar;
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "x");
    append_end(tar);
    RawTempFile raw(tar);
    const std::string gz = std::string(raw.path()) + ".t2.gz";
    // Two full members, then chop the tail so the last member is truncated.
    std::string cmd = "gzip -c '" + std::string(raw.path()) + "' > '" + gz +
                      "'; gzip -c '" + std::string(raw.path()) + "' | head -c 20 >> '" + gz + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
    auto res = unpack_vbin(gz);
    ::unlink(gz.c_str());
    ASSERT_FALSE(res) << "truncated second gzip member must be rejected, not hang";
    EXPECT_EQ(res.error().kind, VbinErrorKind::Archive);
}

// PROBE (iter-2): a lone gzip magic pair (0x1F 0x8B) as trailing bytes after a
// complete member — exactly matching the magic but with no member body. The
// reset+inflate must not read past the buffer (ASan would catch an overrun) and
// must terminate. Two bytes cannot complete a member -> rejected as corrupt.
TEST(VbinTest, TrailingLoneGzipMagicHandledSafely) {
    std::vector<uint8_t> tar;
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "x");
    append_end(tar);
    RawTempFile raw(tar);
    const std::string gz = std::string(raw.path()) + ".m2.gz";
    std::string cmd = "gzip -c '" + std::string(raw.path()) + "' > '" + gz +
                      "'; printf '\\037\\213' >> '" + gz + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
    auto res = unpack_vbin(gz);
    ::unlink(gz.c_str());
    EXPECT_FALSE(res);  // must not crash/hang
}

// ── Adversary probes: Vbin / TempDir move-safety ─────────────────────────────

TEST(VbinTest, VbinMoveKeepsPathsAndOwnership) {
    ASSERT_TRUE(file_exists(kGoodEmuVbin));
    auto res = unpack_vbin(kGoodEmuVbin);
    ASSERT_TRUE(res) << res.error().message;
    Vbin v = std::move(res).value();
    const fs::path dir = v.temp_dir.path();
    const fs::path exe = v.executable;
    EXPECT_TRUE(fs::exists(dir));

    Vbin moved = std::move(v);
    EXPECT_FALSE(v.temp_dir.valid());           // moved-from owns nothing
    EXPECT_EQ(moved.temp_dir.path(), dir);
    EXPECT_EQ(moved.executable, exe);
    EXPECT_TRUE(fs::exists(dir));
    // Destroying the moved-from Vbin must NOT delete the dir owned by `moved`.
    {
        Vbin sink = std::move(v);
        (void)sink;
    }
    EXPECT_TRUE(fs::exists(dir)) << "moved-from destruction wrongly removed the dir";
}

TEST(VbinTest, TempDirSelfMoveIsSafe) {
    auto res = unpack_vbin(kGoodEmuVbin);
    ASSERT_TRUE(res) << res.error().message;
    TempDir& td = res.value().temp_dir;
    const fs::path dir = td.path();
    // Self-move via an aliased pointer (defeats -Wself-move) must not delete the
    // directory — TempDir::operator= guards with `if (this != &o)`.
    TempDir* alias = &td;
    td = std::move(*alias);
    EXPECT_TRUE(fs::exists(dir)) << "self-move removed the directory";
    EXPECT_TRUE(td.valid());
}

TEST(VbinTest, TempDirReleaseSuppressesRemoval) {
    fs::path dir;
    {
        auto res = unpack_vbin(kGoodEmuVbin);
        ASSERT_TRUE(res) << res.error().message;
        dir = res.value().temp_dir.release();
    }
    EXPECT_TRUE(fs::exists(dir)) << "release() should suppress auto-removal";
    fs::remove_all(dir);
}

// ── ADVERSARY PROBE: umask=0117 (systemd UMask=0117 live regression) ─────────
//
// Verifies that both the implicit-directory path (write_member creates parent
// dirs via create_directories) and the explicit-directory-entry path (extract_tar
// handles typeflag '5' dir entries) both add owner-execute AFTER creation so the
// extraction succeeds regardless of the process umask.
//
// MUST FAIL before the fix (create_directories yields 0660; the subsequent
// ofstream fails EACCES) and PASS after.

// Helper: recursively check that every directory under @p root has owner-execute.
void expect_dirs_traversable(const fs::path& root) {
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
            auto perms = it->status().permissions();
            EXPECT_NE(perms & fs::perms::owner_exec, fs::perms::none)
                << "directory missing owner-execute: " << it->path()
                << " mode=" << std::oct << static_cast<int>(perms);
        }
    }
}

// Covers write_member's create_directories(parent) path: the tar has a member
// nested under a path whose parent dirs must be implicitly created.
TEST(VbinTest, NestedMemberExtractionSucceedsUnderRestrictiveUmask) {
    std::vector<uint8_t> tar;
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "#!/bin/true\n");
    // A member nested under a directory that must be created implicitly.
    append_member(tar, "data/default/payload.bin", "nested-content");
    append_end(tar);
    RawTempFile f(tar);

    const mode_t old_umask = ::umask(0117);
    struct UmaskGuard {
        mode_t saved;
        ~UmaskGuard() { ::umask(saved); }
    } umask_guard{old_umask};

    auto res = unpack_vbin(f.path());

    ::umask(old_umask); // restore before ASSERT so teardown is clean

    ASSERT_TRUE(res) << (res ? "" : res.error().message);

    // Every extracted directory must be traversable (owner-exec present).
    expect_dirs_traversable(res.value().temp_dir.path());

    // The nested file was actually extracted.
    EXPECT_TRUE(fs::exists(res.value().temp_dir.path() / "data/default/payload.bin"));
}

// Covers extract_tar's explicit dir-entry path (typeflag '5'): the tar contains
// an explicit directory entry before the file nested inside it.
TEST(VbinTest, ExplicitDirEntryExtractionSucceedsUnderRestrictiveUmask) {
    std::vector<uint8_t> tar;
    append_member(tar, "system_map.xml", kEmuMapXml);
    append_member(tar, "vpp_emu", "#!/bin/true\n");
    // Explicit directory entry (typeflag '5'), then a file inside it.
    append_member(tar, "data/default/", "", '5');
    append_member(tar, "data/default/payload.bin", "content-in-explicit-dir");
    append_end(tar);
    RawTempFile f(tar);

    const mode_t old_umask = ::umask(0117);
    struct UmaskGuard {
        mode_t saved;
        ~UmaskGuard() { ::umask(saved); }
    } umask_guard{old_umask};

    auto res = unpack_vbin(f.path());

    ::umask(old_umask);

    ASSERT_TRUE(res) << (res ? "" : res.error().message);

    expect_dirs_traversable(res.value().temp_dir.path());

    EXPECT_TRUE(fs::exists(res.value().temp_dir.path() / "data/default/payload.bin"));
}

} // namespace
} // namespace slash_sysemu
