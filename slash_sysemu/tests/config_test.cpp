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

#include "config.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

namespace slash_sysemu {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// RAII temporary file that writes content on construction and deletes on destruction.
class TempFile {
public:
    explicit TempFile(const std::string& content) {
        char tmpl[] = "/tmp/slash_sysemu_config_test_XXXXXX";
        int fd = ::mkstemp(tmpl);
        EXPECT_GE(fd, 0) << "mkstemp: " << std::strerror(errno);
        path_ = tmpl;
        if (fd >= 0) {
            EXPECT_EQ(::write(fd, content.data(), content.size()),
                      static_cast<ssize_t>(content.size()))
                << "write to temp file failed: " << std::strerror(errno);
            ::close(fd);
        }
    }

    ~TempFile() {
        if (!path_.empty()) {
            ::unlink(path_.c_str());
        }
    }

    const std::string& path() const { return path_; }

    // Non-copyable.
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

private:
    std::string path_;
};

// Build a fake argv suitable for parse_cli.
// The first element is always "slash_sysemud" (argv[0]).
struct FakeArgv {
    std::vector<std::string> strings;
    std::vector<char*>       ptrs;

    explicit FakeArgv(std::vector<std::string> args) : strings(std::move(args)) {
        for (auto& s : strings) ptrs.push_back(s.data());
    }

    int    argc() { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// BoardBdf::parse
// ─────────────────────────────────────────────────────────────────────────────

TEST(BoardBdfTest, ParseValid) {
    auto b = BoardBdf::parse("0000:61:00");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ("0000:61:00", b->str());
}

TEST(BoardBdfTest, ParseValidMixedCase) {
    auto b = BoardBdf::parse("aBcD:eF:01");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ("aBcD:eF:01", b->str());
}

TEST(BoardBdfTest, ParseRejectsFunctionSuffix) {
    EXPECT_FALSE(BoardBdf::parse("0000:61:00.2").has_value());
    EXPECT_FALSE(BoardBdf::parse("0000:61:00.0").has_value());
}

TEST(BoardBdfTest, ParseRejectsTrailingGarbage) {
    EXPECT_FALSE(BoardBdf::parse("0000:61:00x").has_value());
    EXPECT_FALSE(BoardBdf::parse("0000:61:00 ").has_value());
}

TEST(BoardBdfTest, ParseRejectsTooShort) {
    EXPECT_FALSE(BoardBdf::parse("000:61:00").has_value());
    EXPECT_FALSE(BoardBdf::parse("0000:6:00").has_value());
    EXPECT_FALSE(BoardBdf::parse("0000:61:0").has_value());
    EXPECT_FALSE(BoardBdf::parse("").has_value());
}

TEST(BoardBdfTest, ParseRejectsTooLong) {
    EXPECT_FALSE(BoardBdf::parse("00000:61:00").has_value());
    EXPECT_FALSE(BoardBdf::parse("0000:611:00").has_value());
    EXPECT_FALSE(BoardBdf::parse("0000:61:000").has_value());
}

TEST(BoardBdfTest, ParseRejectsNonHex) {
    EXPECT_FALSE(BoardBdf::parse("ZZZZ:61:00").has_value());
    EXPECT_FALSE(BoardBdf::parse("0000:GG:00").has_value());
}

TEST(BoardBdfTest, ParseRejectsWrongSeparator) {
    EXPECT_FALSE(BoardBdf::parse("0000-61-00").has_value());
    EXPECT_FALSE(BoardBdf::parse("0000.61.00").has_value());
}

TEST(BoardBdfTest, Equality) {
    auto a = BoardBdf::parse("0000:61:00");
    auto b = BoardBdf::parse("0000:61:00");
    auto c = BoardBdf::parse("0000:62:00");
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(*a, *b);
    EXPECT_NE(*a, *c);
}

TEST(BoardBdfTest, LessThan) {
    auto a = BoardBdf::parse("0000:61:00");
    auto b = BoardBdf::parse("0000:62:00");
    ASSERT_TRUE(a && b);
    EXPECT_LT(*a, *b);
}

// ─────────────────────────────────────────────────────────────────────────────
// is_valid_board_bdf (free function — delegates to BoardBdf::parse)
// ─────────────────────────────────────────────────────────────────────────────

TEST(BdfTest, ValidBdf) {
    EXPECT_TRUE(is_valid_board_bdf("0000:61:00"));
    EXPECT_TRUE(is_valid_board_bdf("0000:00:00"));
    EXPECT_TRUE(is_valid_board_bdf("ffff:ff:ff"));
    EXPECT_TRUE(is_valid_board_bdf("FFFF:FF:FF"));
    EXPECT_TRUE(is_valid_board_bdf("aBcD:eF:01"));
}

TEST(BdfTest, InvalidBdfTooShort) {
    EXPECT_FALSE(is_valid_board_bdf("000:61:00")); // domain too short
    EXPECT_FALSE(is_valid_board_bdf("0000:6:00")); // bus too short
    EXPECT_FALSE(is_valid_board_bdf("0000:61:0"));  // device too short
    EXPECT_FALSE(is_valid_board_bdf(""));
}

TEST(BdfTest, InvalidBdfTooLong) {
    EXPECT_FALSE(is_valid_board_bdf("00000:61:00")); // domain too long
    EXPECT_FALSE(is_valid_board_bdf("0000:611:00")); // bus too long
    EXPECT_FALSE(is_valid_board_bdf("0000:61:000")); // device too long
}

TEST(BdfTest, InvalidBdfWithFunctionSuffix) {
    // Board BDF must NOT include a function suffix.
    EXPECT_FALSE(is_valid_board_bdf("0000:61:00.2"));
    EXPECT_FALSE(is_valid_board_bdf("0000:61:00.0"));
}

TEST(BdfTest, InvalidBdfNonHex) {
    EXPECT_FALSE(is_valid_board_bdf("ZZZZ:61:00"));
    EXPECT_FALSE(is_valid_board_bdf("0000:GG:00"));
    EXPECT_FALSE(is_valid_board_bdf("0000:61:ZZ"));
}

TEST(BdfTest, InvalidBdfWrongSeparator) {
    EXPECT_FALSE(is_valid_board_bdf("0000-61-00"));
    EXPECT_FALSE(is_valid_board_bdf("0000.61.00"));
}

// Socket ownership/permissions (uid/gid/mode) are no longer daemon config —
// systemd owns them (User=/Group= + UMask=) — so resolve_uid/resolve_gid and the
// -u/-g/-m CLI options were removed along with their tests.

// ─────────────────────────────────────────────────────────────────────────────
// parse_config_file — valid inputs
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConfigFileTest, EmptyFileIsValid) {
    TempFile f("");
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.accelerators.empty());
}

TEST(ConfigFileTest, SingleDevice) {
    TempFile f(
        "[device.0000:61:00]\n"
        "# comment\n"
    );
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(1u, res.accelerators.size());
    EXPECT_EQ("0000:61:00", res.accelerators[0].board_bdf());
}

TEST(ConfigFileTest, MultipleDevices) {
    TempFile f(
        "[device.0000:61:00]\n"
        "\n"
        "[device.0000:62:00]\n"
        "\n"
        "[device.ffff:ff:ff]\n"
    );
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(3u, res.accelerators.size());
    EXPECT_EQ("0000:61:00", res.accelerators[0].board_bdf());
    EXPECT_EQ("0000:62:00", res.accelerators[1].board_bdf());
    EXPECT_EQ("ffff:ff:ff", res.accelerators[2].board_bdf());
}

TEST(ConfigFileTest, UnknownSectionsIgnored) {
    // Non-"device." sections must be silently ignored.
    TempFile f(
        "[global]\n"
        "some_key = some_value\n"
        "\n"
        "[device.0000:61:00]\n"
        "\n"
        "[future_feature]\n"
        "foo = bar\n"
    );
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(1u, res.accelerators.size());
    EXPECT_EQ("0000:61:00", res.accelerators[0].board_bdf());
}

TEST(ConfigFileTest, ExtraKeysInDeviceSectionIgnored) {
    // Future per-accelerator keys should be tolerated.
    TempFile f(
        "[device.0000:61:00]\n"
        "some_future_key = value\n"
    );
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(1u, res.accelerators.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_config_file — error inputs
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConfigFileTest, MissingFileReturnsError) {
    auto res = parse_config_file("/nonexistent/path/to/config.ini");
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
}

TEST(ConfigFileTest, InvalidBdfInSectionReturnsError) {
    TempFile f("[device.0000:61:00.2]\n"); // function suffix not allowed
    auto res = parse_config_file(f.path());
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
}

TEST(ConfigFileTest, DuplicateBdfReturnsError) {
    TempFile f(
        "[device.0000:61:00]\n"
        "\n"
        "[device.0000:61:00]\n" // duplicate
    );
    auto res = parse_config_file(f.path());
    EXPECT_FALSE(res.ok);
    EXPECT_NE(std::string::npos, res.error.find("duplicate"))
        << "error should mention 'duplicate', got: " << res.error;
}

TEST(ConfigFileTest, MalformedIniReturnsError) {
    // An unclosed section header "[device.0000:61:00" (no closing ']') is a
    // genuine parse error that ini_parse_file detects and reports as the
    // 1-based line number (1 here).  Our scan_section_names skips it (no ']'
    // found) but ini_parse_file flags it, so parse_config_file must return
    // ok==false with a non-empty error message.
    TempFile f("[device.0000:61:00\n"
               "key=value\n");
    auto res = parse_config_file(f.path());
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty()) << "error: " << res.error;
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_cli — valid inputs
// ─────────────────────────────────────────────────────────────────────────────

TEST(CliTest, MinimalValidArgs) {
    TempFile f("[device.0000:61:00]\n");

    FakeArgv args{{"slash_sysemud", "-c", f.path()}};
    auto res = parse_cli(args.argc(), args.argv());
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(0, res.exit_code);
    EXPECT_EQ("/run/slash_sysemu", res.config.base_dir);
    ASSERT_EQ(1u, res.config.accelerators.size());
    EXPECT_EQ("0000:61:00", res.config.accelerators[0].board_bdf());
}

TEST(CliTest, CustomBaseDir) {
    TempFile f("[device.0000:61:00]\n");

    FakeArgv args{{"slash_sysemud",
                   "-c", f.path(),
                   "-d", "/tmp/my_emu"}};
    auto res = parse_cli(args.argc(), args.argv());
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ("/tmp/my_emu", res.config.base_dir);
}

TEST(CliTest, LongOptionsWork) {
    TempFile f("[device.0000:62:00]\n");

    FakeArgv args{{"slash_sysemud",
                   "--config", f.path(),
                   "--base-dir", "/tmp/long"}};
    auto res = parse_cli(args.argc(), args.argv());
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ("/tmp/long", res.config.base_dir);
    ASSERT_EQ(1u, res.config.accelerators.size());
    EXPECT_EQ("0000:62:00", res.config.accelerators[0].board_bdf());
}

TEST(CliTest, ConfigFilePathStoredInConfig) {
    TempFile f("[device.0000:63:00]\n");
    FakeArgv args{{"slash_sysemud", "-c", f.path()}};
    auto res = parse_cli(args.argc(), args.argv());
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(f.path(), res.config.config_file);
}

TEST(CliTest, MultipleAcceleratorsFromFile) {
    TempFile f(
        "[device.0000:61:00]\n"
        "[device.0000:62:00]\n"
    );
    FakeArgv args{{"slash_sysemud", "-c", f.path()}};
    auto res = parse_cli(args.argc(), args.argv());
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(2u, res.config.accelerators.size());
}

TEST(CliTest, BaseDirDefaultsToRuntimeDirectoryEnv) {
    // When -d is omitted, base_dir defaults to $RUNTIME_DIRECTORY (systemd), else
    // /run/slash_sysemu.  Verify the env-var path; take the first colon-list entry.
    TempFile f("[device.0000:61:00]\n");
    ::setenv("RUNTIME_DIRECTORY", "/run/slash_sysemu_test:/run/other", 1);
    FakeArgv args{{"slash_sysemud", "-c", f.path()}};
    auto res = parse_cli(args.argc(), args.argv());
    ::unsetenv("RUNTIME_DIRECTORY");
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ("/run/slash_sysemu_test", res.config.base_dir);
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_cli — error inputs
// ─────────────────────────────────────────────────────────────────────────────

TEST(CliTest, MissingConfigArgFails) {
    FakeArgv args{{"slash_sysemud"}};
    auto res = parse_cli(args.argc(), args.argv());
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
}

TEST(CliTest, NonexistentConfigFileFails) {
    // CLI11 with ExistingFile check rejects non-existent files at parse time.
    FakeArgv args{{"slash_sysemud", "-c", "/nonexistent/config.ini"}};
    auto res = parse_cli(args.argc(), args.argv());
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
}

TEST(CliTest, InvalidBdfInConfigFilePropagatesError) {
    TempFile f("[device.0000:61:00.2]\n"); // function suffix
    FakeArgv args{{"slash_sysemud", "-c", f.path()}};
    auto res = parse_cli(args.argc(), args.argv());
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
}

TEST(CliTest, DuplicateBdfInConfigFilePropagatesError) {
    TempFile f(
        "[device.0000:61:00]\n"
        "[device.0000:61:00]\n"
    );
    FakeArgv args{{"slash_sysemud", "-c", f.path()}};
    auto res = parse_cli(args.argc(), args.argv());
    EXPECT_FALSE(res.ok);
}

TEST(CliTest, UnknownFlagFails) {
    TempFile f("");
    FakeArgv args{{"slash_sysemud", "-c", f.path(), "--unknown-option"}};
    auto res = parse_cli(args.argc(), args.argv());
    EXPECT_FALSE(res.ok);
}

// The removed -u/-g/-m options are now genuinely unknown flags: passing one is a
// hard parse error (ownership/mode moved to systemd).
TEST(CliTest, RemovedOwnershipFlagsAreRejected) {
    TempFile f("[device.0000:61:00]\n");
    FakeArgv args{{"slash_sysemud", "-c", f.path(), "-u", "0"}};
    auto res = parse_cli(args.argc(), args.argv());
    EXPECT_FALSE(res.ok);
}

TEST(CliTest, HelpFlagReturnsOkWithExitCode) {
    // --help should return ok==true and exit_code==0 (caller exits cleanly).
    FakeArgv args{{"slash_sysemud", "--help"}};
    auto res = parse_cli(args.argc(), args.argv());
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(0, res.exit_code);
}

TEST(CliTest, VersionFlagReturnsOkWithExitCode) {
    // --version should return ok==true and exit_code==0 (caller exits cleanly).
    FakeArgv args{{"slash_sysemud", "--version"}};
    auto res = parse_cli(args.argc(), args.argv());
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(0, res.exit_code);
}

// ─────────────────────────────────────────────────────────────────────────────
// Adversary probes — Step 3 review
// ─────────────────────────────────────────────────────────────────────────────

// Finding A1 (Critical): config_test.cpp used .board_bdf as a data member but
// AcceleratorConfig exposes it as board_bdf() method.  Fixed in the tests above.
// This probe validates the correct calling convention survives future refactors.
TEST(AdversaryBdfTest, BoardBdfMethodAccessorReturnsCorrectString) {
    // Construct an AcceleratorConfig via parse_config_file and verify that
    // board_bdf() returns the expected canonical string, not a compile error.
    TempFile f("[device.ABCD:12:EF]\n");
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(1u, res.accelerators.size());
    EXPECT_EQ("ABCD:12:EF", res.accelerators[0].board_bdf());
    // Also verify bdf.str() and board_bdf() are consistent.
    EXPECT_EQ(res.accelerators[0].bdf.str(), res.accelerators[0].board_bdf());
}

// Finding A2 (Major): parse_cli now requires at least one accelerator, but many
// "happy-path" CliTest cases passed an empty config file and expected ok==true.
// Those tests were broken (fixed above).  This probe documents the requirement.
TEST(AdversaryCliTest, EmptyConfigFileViaCliFailsNoAccelerators) {
    TempFile f("");
    FakeArgv args{{"slash_sysemud", "-c", f.path()}};
    auto res = parse_cli(args.argc(), args.argv());
    EXPECT_FALSE(res.ok);
    // Error message must mention the absence of accelerators.
    EXPECT_NE(std::string::npos, res.error.find("no")) << "error: " << res.error;
}

// Finding A3: Non-absolute base_dir must be rejected.
TEST(AdversaryCliTest, RelativeBaseDirIsRejected) {
    TempFile f("[device.0000:61:00]\n");
    FakeArgv args{{"slash_sysemud",
                   "-c", f.path(),
                   "-d", "relative/path"}};
    auto res = parse_cli(args.argc(), args.argv());
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
}

// Finding A4: Empty base_dir string (edge case of non-absolute check).
TEST(AdversaryCliTest, EmptyBaseDirIsRejected) {
    TempFile f("[device.0000:61:00]\n");
    FakeArgv args{{"slash_sysemud",
                   "-c", f.path(),
                   "-d", ""}};
    auto res = parse_cli(args.argc(), args.argv());
    // CLI11 may reject empty string before we even get to the absolute-path check;
    // either way, ok must be false.
    EXPECT_FALSE(res.ok);
}

// Finding A5: socket_path_* helpers are untested.  Verify all three formats.
TEST(AdversarySocketPathTest, CtlPathFormat) {
    DaemonConfig cfg;
    cfg.base_dir = "/run/slash_sysemu";
    EXPECT_EQ("/run/slash_sysemu/slash_ctl0",  socket_path_ctl(cfg, 0));
    EXPECT_EQ("/run/slash_sysemu/slash_ctl3",  socket_path_ctl(cfg, 3));
    EXPECT_EQ("/run/slash_sysemu/slash_ctl12", socket_path_ctl(cfg, 12));
}

TEST(AdversarySocketPathTest, QdmaCtlPathFormat) {
    DaemonConfig cfg;
    cfg.base_dir = "/run/slash_sysemu";
    EXPECT_EQ("/run/slash_sysemu/slash_qdma_ctl0",  socket_path_qdma_ctl(cfg, 0));
    EXPECT_EQ("/run/slash_sysemu/slash_qdma_ctl1",  socket_path_qdma_ctl(cfg, 1));
}

TEST(AdversarySocketPathTest, HotplugPathFormat) {
    DaemonConfig cfg;
    cfg.base_dir = "/run/slash_sysemu";
    EXPECT_EQ("/run/slash_sysemu/slash_hotplug", socket_path_hotplug(cfg));
}

TEST(AdversarySocketPathTest, CustomBaseDirReflectedInPaths) {
    DaemonConfig cfg;
    cfg.base_dir = "/tmp/my_emu";
    EXPECT_EQ("/tmp/my_emu/slash_ctl0",    socket_path_ctl(cfg, 0));
    EXPECT_EQ("/tmp/my_emu/slash_qdma_ctl0", socket_path_qdma_ctl(cfg, 0));
    EXPECT_EQ("/tmp/my_emu/slash_hotplug",   socket_path_hotplug(cfg));
}

// Finding A6: BoardBdf::parse not directly tested.  Verify parse/str round-trip.
TEST(AdversaryBoardBdfParseTest, ParseValidReturnsEngaged) {
    auto result = BoardBdf::parse("0000:61:00");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ("0000:61:00", result->str());
}

TEST(AdversaryBoardBdfParseTest, ParseInvalidReturnsNullopt) {
    EXPECT_FALSE(BoardBdf::parse("0000:61:00.2").has_value()); // function suffix
    EXPECT_FALSE(BoardBdf::parse("0000:61:00.0").has_value()); // .0 suffix
    EXPECT_FALSE(BoardBdf::parse("").has_value());             // empty
    EXPECT_FALSE(BoardBdf::parse("0000:61:0").has_value());    // too short
    EXPECT_FALSE(BoardBdf::parse("0000:61:001").has_value());  // too long
    EXPECT_FALSE(BoardBdf::parse("ZZZZ:61:00").has_value());   // non-hex
}

TEST(AdversaryBoardBdfParseTest, ParseMixedCasePreservesCase) {
    // The canonical string is whatever was provided (not normalised).
    auto result = BoardBdf::parse("aBcD:eF:01");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ("aBcD:eF:01", result->str());
}

// Finding A9: vbin_path field on AcceleratorConfig is std::optional — verify
// it comes out as nullopt when not specified in the INI file.
TEST(AdversaryBdfTest, VbinPathIsNulloptWhenAbsentFromIni) {
    TempFile f("[device.0000:61:00]\n");
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(1u, res.accelerators.size());
    EXPECT_FALSE(res.accelerators[0].vbin_path.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 6: default VBIN configuration (default_vbin_path + per-device vbin_path)
// ─────────────────────────────────────────────────────────────────────────────

TEST(DefaultVbinTest, PerDeviceVbinPathParsedFromConfig) {
    TempFile f(
        "[device.0000:61:00]\n"
        "vbin_path = /opt/models/custom.vbin\n"
    );
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(1u, res.accelerators.size());
    ASSERT_TRUE(res.accelerators[0].vbin_path.has_value());
    EXPECT_EQ("/opt/models/custom.vbin", *res.accelerators[0].vbin_path);
}

TEST(DefaultVbinTest, PerDeviceVbinPathAbsentIsNullopt) {
    TempFile f("[device.0000:61:00]\n");
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(1u, res.accelerators.size());
    EXPECT_FALSE(res.accelerators[0].vbin_path.has_value());
}

TEST(DefaultVbinTest, EmptyVbinPathValueIsNullopt) {
    // An explicitly empty value is treated as "not set".
    TempFile f(
        "[device.0000:61:00]\n"
        "vbin_path =\n"
    );
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(1u, res.accelerators.size());
    EXPECT_FALSE(res.accelerators[0].vbin_path.has_value());
}

TEST(DefaultVbinTest, PerDeviceVbinPathIsScopedToItsDevice) {
    TempFile f(
        "[device.0000:61:00]\n"
        "vbin_path = /opt/a.vbin\n"
        "[device.0000:62:00]\n"
    );
    auto res = parse_config_file(f.path());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(2u, res.accelerators.size());
    ASSERT_TRUE(res.accelerators[0].vbin_path.has_value());
    EXPECT_EQ("/opt/a.vbin", *res.accelerators[0].vbin_path);
    EXPECT_FALSE(res.accelerators[1].vbin_path.has_value());
}

TEST(DefaultVbinTest, CliDefaultVbinParsed) {
    TempFile f("[device.0000:61:00]\n");
    FakeArgv args{{"slash_sysemud", "-c", f.path(),
                   "--default-vbin", "/opt/models/default.vbin"}};
    auto res = parse_cli(args.argc(), args.argv());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_TRUE(res.config.default_vbin_path.has_value());
    EXPECT_EQ("/opt/models/default.vbin", *res.config.default_vbin_path);
}

TEST(DefaultVbinTest, CliDefaultVbinAbsentUsesCompiledDefault) {
    // With neither --default-vbin nor $SLASH_SYSEMU_DEFAULT_VBIN, the daemon
    // falls back to the compiled-in installed default VBIN path.
    ::unsetenv("SLASH_SYSEMU_DEFAULT_VBIN");
    TempFile f("[device.0000:61:00]\n");
    FakeArgv args{{"slash_sysemud", "-c", f.path()}};
    auto res = parse_cli(args.argc(), args.argv());
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_TRUE(res.config.default_vbin_path.has_value());
    EXPECT_TRUE(res.config.default_vbin_path->ends_with("slash-sysemu/default.vbin"))
        << *res.config.default_vbin_path;
}

TEST(DefaultVbinTest, EnvDefaultVbinOverridesCompiledDefault) {
    ::setenv("SLASH_SYSEMU_DEFAULT_VBIN", "/opt/env/default.vbin", 1);
    TempFile f("[device.0000:61:00]\n");
    FakeArgv args{{"slash_sysemud", "-c", f.path()}};
    auto res = parse_cli(args.argc(), args.argv());
    ::unsetenv("SLASH_SYSEMU_DEFAULT_VBIN");
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_TRUE(res.config.default_vbin_path.has_value());
    EXPECT_EQ("/opt/env/default.vbin", *res.config.default_vbin_path);
}

TEST(DefaultVbinTest, CliDefaultVbinOverridesEnv) {
    ::setenv("SLASH_SYSEMU_DEFAULT_VBIN", "/opt/env/default.vbin", 1);
    TempFile f("[device.0000:61:00]\n");
    FakeArgv args{{"slash_sysemud", "-c", f.path(),
                   "--default-vbin", "/opt/cli/default.vbin"}};
    auto res = parse_cli(args.argc(), args.argv());
    ::unsetenv("SLASH_SYSEMU_DEFAULT_VBIN");
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_TRUE(res.config.default_vbin_path.has_value());
    EXPECT_EQ("/opt/cli/default.vbin", *res.config.default_vbin_path);
}

TEST(DefaultVbinTest, ResolvePrefersPerAcceleratorOverDaemonDefault) {
    DaemonConfig cfg;
    cfg.default_vbin_path = "/opt/daemon-default.vbin";

    AcceleratorConfig with_override{*BoardBdf::parse("0000:61:00"),
                                    std::string("/opt/accel-specific.vbin")};
    AcceleratorConfig no_override{*BoardBdf::parse("0000:62:00"), std::nullopt};

    // Per-accelerator vbin_path wins.
    auto a = cfg.resolve_default_vbin(with_override);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ("/opt/accel-specific.vbin", *a);

    // Falls back to the daemon-wide default when the accelerator has none.
    auto b = cfg.resolve_default_vbin(no_override);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ("/opt/daemon-default.vbin", *b);
}

TEST(DefaultVbinTest, ResolveNulloptWhenNeitherConfigured) {
    DaemonConfig cfg; // no default_vbin_path
    AcceleratorConfig accel{*BoardBdf::parse("0000:61:00"), std::nullopt};
    EXPECT_FALSE(cfg.resolve_default_vbin(accel).has_value());
}

} // namespace
} // namespace slash_sysemu
