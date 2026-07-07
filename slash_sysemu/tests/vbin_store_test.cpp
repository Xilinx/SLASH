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

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

using namespace slash_sysemu;
namespace fs = std::filesystem;

namespace {

// A scratch directory removed on destruction — gives each test an isolated base.
class ScratchDir {
public:
    ScratchDir() {
        path_ = fs::temp_directory_path() /
                ("slash_sysemu_store_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++));
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path             path_;
    static inline int    counter_ = 0;
};

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream ofs(p, std::ios::binary);
    ofs << content;
}

std::string read_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

TEST(VbinStore, PathsComposedFromBaseAndBdf) {
    ScratchDir scratch;
    VbinStore store(scratch.path(), "0000:61:00");
    EXPECT_EQ(store.dir(), scratch.path() / "0000:61:00");
    EXPECT_EQ(store.main_path(), scratch.path() / "0000:61:00" / "main.vbin");
    EXPECT_EQ(store.staging_path(), scratch.path() / "0000:61:00" / "staging.vbin");
}

TEST(VbinStore, BootstrapFromDefaultSeedsMainAndEmptyStaging) {
    ScratchDir scratch;
    fs::path def = scratch.path() / "default.vbin";
    write_file(def, "DEFAULT-VBIN-CONTENT");

    VbinStore store(scratch.path(), "0000:61:00");
    ASSERT_FALSE(store.has_main());

    auto r = store.bootstrap(def);
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);

    EXPECT_TRUE(store.has_main());
    EXPECT_EQ(read_file(store.main_path()), "DEFAULT-VBIN-CONTENT");
    EXPECT_TRUE(fs::exists(store.staging_path()));
    EXPECT_FALSE(store.staging_nonempty());
}

TEST(VbinStore, BootstrapDoesNotOverwriteExistingMain) {
    ScratchDir scratch;
    fs::path def = scratch.path() / "default.vbin";
    write_file(def, "DEFAULT");

    VbinStore store(scratch.path(), "0000:61:00");
    ASSERT_TRUE(store.bootstrap(def).has_value());
    // Simulate a previously-launched main that must survive re-bootstrap.
    write_file(store.main_path(), "PREVIOUSLY-LAUNCHED");

    // Re-bootstrap (as a RESCAN would) must NOT clobber the existing main.
    ASSERT_TRUE(store.bootstrap(def).has_value());
    EXPECT_EQ(read_file(store.main_path()), "PREVIOUSLY-LAUNCHED");
}

TEST(VbinStore, BootstrapPreservesExistingStagingWhenMainExists) {
    ScratchDir scratch;
    fs::path def = scratch.path() / "default.vbin";
    write_file(def, "DEFAULT");
    VbinStore store(scratch.path(), "0000:61:00");
    ASSERT_TRUE(store.bootstrap(def).has_value());

    // User wrote a staging buffer we haven't consumed yet.
    ASSERT_TRUE(store.append_staging(bytes("STAGED")).has_value());
    ASSERT_TRUE(store.staging_nonempty());

    // Re-bootstrap must not truncate the pending staging buffer.
    ASSERT_TRUE(store.bootstrap(def).has_value());
    EXPECT_TRUE(store.staging_nonempty());
    EXPECT_EQ(read_file(store.staging_path()), "STAGED");
}

TEST(VbinStore, BootstrapMissingDefaultIsIoError) {
    ScratchDir scratch;
    VbinStore store(scratch.path(), "0000:61:00");
    auto r = store.bootstrap(scratch.path() / "does_not_exist.vbin");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, VbinErrorKind::Io);
}

TEST(VbinStore, AppendStagingAccumulatesChunks) {
    ScratchDir scratch;
    fs::path def = scratch.path() / "default.vbin";
    write_file(def, "DEFAULT");
    VbinStore store(scratch.path(), "0000:61:00");
    ASSERT_TRUE(store.bootstrap(def).has_value());

    ASSERT_TRUE(store.append_staging(bytes("AAAA")).has_value());
    ASSERT_TRUE(store.append_staging(bytes("BBBB")).has_value());
    ASSERT_TRUE(store.append_staging(bytes("")).has_value()); // empty append is a no-op

    auto r = store.read_staging();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(std::string(r.value().begin(), r.value().end()), "AAAABBBB");
}

TEST(VbinStore, ReadStagingEmptyWhenAbsent) {
    ScratchDir scratch;
    VbinStore store(scratch.path(), "0000:61:00");
    // No bootstrap: staging.vbin does not exist.
    auto r = store.read_staging();
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r.value().empty());
}

TEST(VbinStore, ClearStagingTruncatesButKeepsFile) {
    ScratchDir scratch;
    fs::path def = scratch.path() / "default.vbin";
    write_file(def, "DEFAULT");
    VbinStore store(scratch.path(), "0000:61:00");
    ASSERT_TRUE(store.bootstrap(def).has_value());
    ASSERT_TRUE(store.append_staging(bytes("SOMETHING")).has_value());
    ASSERT_TRUE(store.staging_nonempty());

    ASSERT_TRUE(store.clear_staging().has_value());
    EXPECT_TRUE(fs::exists(store.staging_path())); // file kept
    EXPECT_FALSE(store.staging_nonempty());        // but empty
}

TEST(VbinStore, ReplaceMainWithStagingIsAtomicAndRecreatesEmptyStaging) {
    ScratchDir scratch;
    fs::path def = scratch.path() / "default.vbin";
    write_file(def, "DEFAULT");
    VbinStore store(scratch.path(), "0000:61:00");
    ASSERT_TRUE(store.bootstrap(def).has_value());
    ASSERT_TRUE(store.append_staging(bytes("NEW-MAIN")).has_value());

    ASSERT_TRUE(store.replace_main_with_staging().has_value());
    EXPECT_EQ(read_file(store.main_path()), "NEW-MAIN");
    // Staging is recreated empty.
    EXPECT_TRUE(fs::exists(store.staging_path()));
    EXPECT_FALSE(store.staging_nonempty());
}

// ── ADVERSARY PROBES (Step 6) ────────────────────────────────────────────────

// PROBE A1 — replace_main_with_staging() rename FAILURE must not silently leave
// staging populated in a way that misrepresents on-disk state.  Force the rename
// to fail by making main.vbin a NON-EMPTY directory (rename-over-nonempty-dir
// fails with ENOTEMPTY/EISDIR).  Documents the observed post-failure state so the
// reconfigure-level consequence (below) is understood.
TEST(VbinStore, ReplaceMainRenameFailureLeavesStagingObservable) {
    ScratchDir scratch;
    fs::path def = scratch.path() / "default.vbin";
    write_file(def, "DEFAULT");
    VbinStore store(scratch.path(), "0000:61:00");
    ASSERT_TRUE(store.bootstrap(def).has_value());
    ASSERT_TRUE(store.append_staging(bytes("NEW-MAIN")).has_value());
    ASSERT_TRUE(store.staging_nonempty());

    // Replace main.vbin (a regular file) with a NON-EMPTY directory so the
    // std::filesystem::rename(staging -> main) fails.
    fs::remove(store.main_path());
    fs::create_directory(store.main_path());
    write_file(store.main_path() / "blocker", "x");

    auto r = store.replace_main_with_staging();
    EXPECT_FALSE(r.has_value()); // rename failed → Io error reported
    // The key observation: staging still holds the content (rename didn't move
    // it).  The NEXT reconfigure() will therefore see staging_nonempty()==true
    // and try to relaunch it again.  This is the staging-inconsistency corner.
    EXPECT_TRUE(store.staging_nonempty());
    EXPECT_EQ(read_file(store.staging_path()), "NEW-MAIN");

    // cleanup so ScratchDir::remove_all doesn't choke on the directory-as-file
    fs::remove_all(store.main_path());
}

TEST(VbinStore, ColdRebootCleanupRemovesEverything) {
    ScratchDir scratch;
    fs::path def = scratch.path() / "default.vbin";
    write_file(def, "DEFAULT");
    VbinStore store(scratch.path(), "0000:61:00");
    ASSERT_TRUE(store.bootstrap(def).has_value());
    ASSERT_TRUE(store.has_main());

    ASSERT_TRUE(store.cold_reboot_cleanup().has_value());
    EXPECT_FALSE(fs::exists(store.dir()));
    EXPECT_FALSE(store.has_main());
}

// The key lifecycle guarantee: an accelerator teardown does NOT touch the VBIN
// files (there is no teardown method on the store that removes them); only
// cold_reboot_cleanup() does.  This test documents that contract: after
// bootstrap, both files persist until an explicit cold reboot.
TEST(VbinStore, FilesPersistUntilColdReboot) {
    ScratchDir scratch;
    fs::path def = scratch.path() / "default.vbin";
    write_file(def, "DEFAULT");

    {
        VbinStore store(scratch.path(), "0000:61:00");
        ASSERT_TRUE(store.bootstrap(def).has_value());
        ASSERT_TRUE(store.append_staging(bytes("PENDING")).has_value());
        // Store goes out of scope here — models an accelerator teardown.  The
        // destructor must NOT remove the on-disk files.
    }

    // A brand-new store for the same BDF sees the persisted files.
    VbinStore store2(scratch.path(), "0000:61:00");
    EXPECT_TRUE(store2.has_main());
    EXPECT_EQ(read_file(store2.main_path()), "DEFAULT");
    EXPECT_TRUE(store2.staging_nonempty());
    EXPECT_EQ(read_file(store2.staging_path()), "PENDING");
}
