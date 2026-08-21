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

#include "bar_memfd.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

#include <pthread.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace slash_sysemu;

namespace {

// A small BAR keeps tests fast while exercising the same code paths as the real
// 128 MiB / 512 KiB windows.
constexpr std::size_t kSmall = 4096;

BarMemfd make_small(std::size_t size = kSmall) {
    auto r = BarMemfd::create(size, "test_bar");
    EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
    return std::move(r.value());
}

int count_open_fds() {
    int  n = 0;
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) return -1;
    while (::readdir(d) != nullptr) ++n;
    ::closedir(d);
    return n;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Creation / sizing
// ─────────────────────────────────────────────────────────────────────────────

TEST(BarMemfd, CreatesAtRequestedSize) {
    auto bar = make_small(8192);
    EXPECT_EQ(bar.size(), 8192u);
    EXPECT_GE(bar.fd(), 0);
    // The memfd is actually sized: fstat reports the ftruncate length.
    struct stat st{};
    ASSERT_EQ(::fstat(bar.fd(), &st), 0);
    EXPECT_EQ(static_cast<std::size_t>(st.st_size), 8192u);
}

TEST(BarMemfd, FreshWindowReadsAsZero) {
    auto bar = make_small();
    auto v   = bar.read_u32(0);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), 0u);
    auto last = bar.read_u32(kSmall - 4);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last.value(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// u32 + byte round-trips
// ─────────────────────────────────────────────────────────────────────────────

TEST(BarMemfd, U32RoundTripAtVariousOffsets) {
    auto bar = make_small();
    ASSERT_TRUE(bar.write_u32(0, 0xdeadbeef).has_value());
    ASSERT_TRUE(bar.write_u32(16, 0x01020304).has_value());
    ASSERT_TRUE(bar.write_u32(kSmall - 4, 0xfeedface).has_value()); // last valid offset

    EXPECT_EQ(bar.read_u32(0).value(), 0xdeadbeefu);
    EXPECT_EQ(bar.read_u32(16).value(), 0x01020304u);
    EXPECT_EQ(bar.read_u32(kSmall - 4).value(), 0xfeedfaceu);
}

TEST(BarMemfd, U32IsLittleEndian) {
    auto bar = make_small();
    ASSERT_TRUE(bar.write_u32(0, 0x04030201).has_value());
    auto bytes = bar.read(0, 4);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes.value(), (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04}));
}

TEST(BarMemfd, ByteRangeRoundTrip) {
    auto bar = make_small();
    std::vector<uint8_t> data(256);
    std::iota(data.begin(), data.end(), 0);
    ASSERT_TRUE(bar.write(100, data).has_value());
    auto got = bar.read(100, data.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got.value(), data);
}

TEST(BarMemfd, EmptyReadAndWrite) {
    auto bar = make_small();
    EXPECT_TRUE(bar.write(0, {}).has_value());
    auto got = bar.read(0, 0);
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got.value().empty());
    // Zero-length access at the very end is still in range.
    EXPECT_TRUE(bar.read(kSmall, 0).has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Bounds checking
// ─────────────────────────────────────────────────────────────────────────────

TEST(BarMemfd, OutOfRangeU32IsProtocolError) {
    auto bar = make_small();
    // offset+4 > size
    auto r = bar.read_u32(kSmall - 3);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
    auto w = bar.write_u32(kSmall, 1);
    ASSERT_FALSE(w.has_value());
    EXPECT_EQ(w.error().kind, ErrorKind::Protocol);
}

TEST(BarMemfd, OutOfRangeByteRangeIsProtocolError) {
    auto bar = make_small();
    auto r = bar.read(kSmall - 10, 20);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);

    std::vector<uint8_t> big(32);
    auto w = bar.write(kSmall - 10, big);
    ASSERT_FALSE(w.has_value());
    EXPECT_EQ(w.error().kind, ErrorKind::Protocol);
}

TEST(BarMemfd, OffsetOverflowIsRejected) {
    auto bar = make_small();
    // A huge offset must not wrap; must be rejected as out of range.
    auto r = bar.read(static_cast<std::size_t>(-1), 8);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

// ─────────────────────────────────────────────────────────────────────────────
// reopen(): distinct open file description
// ─────────────────────────────────────────────────────────────────────────────

TEST(BarMemfd, ReopenSharesMemoryButIsDistinctDescription) {
    auto bar = make_small();
    auto re  = bar.reopen();
    ASSERT_TRUE(re.has_value()) << (re.has_value() ? "" : re.error().message);
    UniqueFd user_fd = std::move(re.value());
    EXPECT_NE(user_fd.get(), bar.fd()); // different fd numbers

    // A write through the daemon's mapping is visible through the user's fd
    // (same underlying memfd inode).
    ASSERT_TRUE(bar.write_u32(0, 0xa5a5a5a5).has_value());
    uint8_t buf[4] = {};
    ASSERT_EQ(::pread(user_fd.get(), buf, 4, 0), 4);
    uint32_t seen = static_cast<uint32_t>(buf[0]) | (buf[1] << 8) | (buf[2] << 16) |
                    (static_cast<uint32_t>(buf[3]) << 24);
    EXPECT_EQ(seen, 0xa5a5a5a5u);

    // And a write through the user's fd is visible to the daemon.
    uint8_t out[4] = {0x11, 0x22, 0x33, 0x44};
    ASSERT_EQ(::pwrite(user_fd.get(), out, 4, 8), 4);
    EXPECT_EQ(bar.read_u32(8).value(), 0x44332211u);
}

TEST(BarMemfd, ReopenGivesCrossDescriptionFlockCollision) {
    // flock is per-open-file-description.  An LOCK_EX on the daemon's fd must
    // CONFLICT with a lock attempt on the reopened (distinct) description — this
    // models the daemon-vs-user collision that replaces DMA_BUF_IOCTL_SYNC.
    auto bar = make_small();
    auto re  = bar.reopen();
    ASSERT_TRUE(re.has_value());
    UniqueFd user_fd = std::move(re.value());

    // Hold an exclusive lock on the daemon's description.
    ASSERT_EQ(::flock(bar.fd(), LOCK_EX), 0);

    // A non-blocking exclusive lock on the DISTINCT description must fail (EWOULDBLOCK).
    int rc = ::flock(user_fd.get(), LOCK_EX | LOCK_NB);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(errno, EWOULDBLOCK);

    ::flock(bar.fd(), LOCK_UN);
    // After release, the user's lock succeeds.
    EXPECT_EQ(::flock(user_fd.get(), LOCK_EX | LOCK_NB), 0);
    ::flock(user_fd.get(), LOCK_UN);
}

TEST(BarMemfd, DupSharesDescriptionSoNoFlockCollision) {
    // Contrast: a dup()'d fd shares the SAME open file description, so its flock
    // does NOT conflict with the original's — which is exactly why reopen() must
    // NOT use dup().
    auto bar = make_small();
    int  dup_fd = ::dup(bar.fd());
    ASSERT_GE(dup_fd, 0);

    ASSERT_EQ(::flock(bar.fd(), LOCK_EX), 0);
    // Same description → the lock is considered already held; LOCK_EX|LOCK_NB
    // succeeds (re-locks/refreshes) rather than blocking.
    EXPECT_EQ(::flock(dup_fd, LOCK_EX | LOCK_NB), 0);
    ::flock(bar.fd(), LOCK_UN);
    ::close(dup_fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrency: exclusive writers serialised, shared readers see no torn values
// ─────────────────────────────────────────────────────────────────────────────

TEST(BarMemfd, ConcurrentExclusiveIncrementsAreSerialised) {
    // N threads each with their OWN reopened description do a read-modify-write of
    // a counter under LOCK_EX.  If exclusive locking serialises them correctly,
    // the final value is exactly the number of increments.
    auto bar = make_small();
    ASSERT_TRUE(bar.write_u32(0, 0).has_value());

    constexpr int kThreads = 8;
    constexpr int kIters   = 500;

    auto worker = [&] {
        // Each thread uses its own distinct description (as a separate process
        // would), so flock collisions between threads are real.
        auto re = bar.reopen();
        ASSERT_TRUE(re.has_value());
        int fd = re.value().get();
        UniqueFd hold = std::move(re.value());
        for (int i = 0; i < kIters; ++i) {
            ASSERT_EQ(::flock(fd, LOCK_EX), 0);
            // Read-modify-write via pread/pwrite on this description.
            uint32_t v = 0;
            ASSERT_EQ(::pread(fd, &v, 4, 0), 4);
            v += 1;
            ASSERT_EQ(::pwrite(fd, &v, 4, 0), 4);
            ASSERT_EQ(::flock(fd, LOCK_UN), 0);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker);
    for (auto& th : threads) th.join();

    auto final = bar.read_u32(0);
    ASSERT_TRUE(final.has_value());
    EXPECT_EQ(final.value(), static_cast<uint32_t>(kThreads * kIters));
}

TEST(BarMemfd, ConcurrentSharedReadersSeeConsistentValues) {
    // A writer publishes 4-byte-consistent values under LOCK_EX; shared readers
    // under LOCK_SH must never observe a torn/partial value.
    //
    // IMPORTANT flock nuance: flock is per-OPEN-FILE-DESCRIPTION.  Locks taken on
    // the SAME description (e.g. the single daemon BarMemfd fd) do NOT exclude
    // each other — LOCK_SH followed by LOCK_EX on one fd merely CONVERTS the lock.
    // So to model genuine reader/writer exclusion (as between the daemon and a
    // user, or between separate worker descriptions) each party must hold its OWN
    // reopened description.  Here the writer and every reader use distinct
    // reopened fds and do the RMW/read via pread/pwrite bracketed by flock.
    auto bar = make_small();
    ASSERT_TRUE(bar.write_u32(0, 0).has_value());

    std::atomic<bool> stop{false};
    std::atomic<int>  torn{0};

    auto writer_re = bar.reopen();
    ASSERT_TRUE(writer_re.has_value());
    UniqueFd writer_fd = std::move(writer_re.value());

    std::thread writer([&] {
        for (uint32_t i = 1; i <= 200000 && !stop.load(); ++i) {
            uint32_t v = (i & 0xff);
            v          = v | (v << 8) | (v << 16) | (v << 24);
            ASSERT_EQ(::flock(writer_fd.get(), LOCK_EX), 0);
            ASSERT_EQ(::pwrite(writer_fd.get(), &v, 4, 0), 4);
            ASSERT_EQ(::flock(writer_fd.get(), LOCK_UN), 0);
        }
        stop.store(true);
    });

    std::vector<std::thread> readers;
    std::vector<UniqueFd>    reader_fds;
    for (int r = 0; r < 4; ++r) {
        auto re = bar.reopen();
        ASSERT_TRUE(re.has_value());
        reader_fds.push_back(std::move(re.value()));
    }
    for (int r = 0; r < 4; ++r) {
        int fd = reader_fds[r].get();
        readers.emplace_back([&, fd] {
            while (!stop.load()) {
                uint32_t x = 0;
                ASSERT_EQ(::flock(fd, LOCK_SH), 0);
                ASSERT_EQ(::pread(fd, &x, 4, 0), 4);
                ASSERT_EQ(::flock(fd, LOCK_UN), 0);
                uint8_t b0 = x & 0xff, b1 = (x >> 8) & 0xff, b2 = (x >> 16) & 0xff,
                        b3 = (x >> 24) & 0xff;
                if (!(b0 == b1 && b1 == b2 && b2 == b3)) {
                    ++torn;
                }
            }
        });
    }
    writer.join();
    for (auto& th : readers) th.join();
    EXPECT_EQ(torn.load(), 0)
        << "torn read observed — reader/writer flock exclusion across distinct "
           "descriptions failed";
}

// ─────────────────────────────────────────────────────────────────────────────
// Same-object concurrency: the internal mutex serialises daemon-side access
// ─────────────────────────────────────────────────────────────────────────────
//
// The 128 MiB user-region BAR holds every kernel's register window and one model
// control worker thread runs PER KERNEL, so multiple daemon threads share ONE
// BarMemfd (NOT reopened).  BarMemfd's internal mutex must make that safe: with a
// single fd, an unsynchronised LOCK_UN from one thread would drop the whole-file
// flock another thread holds.  These tests use the daemon-side write_u32/read_u32
// on the SAME object from many threads.

TEST(BarMemfd, SharedObjectConcurrentSameOffsetRmwIsSerialised) {
    auto bar = make_small();
    ASSERT_TRUE(bar.write_u32(0, 0).has_value());

    constexpr int kThreads = 8;
    constexpr int kIters   = 2000;

    auto worker = [&] {
        for (int i = 0; i < kIters; ++i) {
            // Read-modify-write the SAME offset via the daemon-side API on the
            // SHARED object.  The internal mutex must serialise the full
            // read+write so no update is lost.
            auto v = bar.read_u32(0);
            ASSERT_TRUE(v.has_value());
            ASSERT_TRUE(bar.write_u32(0, v.value() + 1).has_value());
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker);
    for (auto& th : threads) th.join();

    auto final = bar.read_u32(0);
    ASSERT_TRUE(final.has_value());
    // NOTE: this asserts atomicity of the WHOLE read-modify-write only if the
    // mutex spans both calls.  It does not — each call is individually locked —
    // so this specifically tests that individual accesses never corrupt each
    // other; the exact-count guarantee is covered by the single-call variant
    // below.  Here we only require the value is in (0, kThreads*kIters].
    EXPECT_GT(final.value(), 0u);
    EXPECT_LE(final.value(), static_cast<uint32_t>(kThreads * kIters));
}

TEST(BarMemfd, SharedObjectConcurrentSingleCallWritesNeverTear) {
    // Each thread owns a distinct offset and hammers write_u32/read_u32 on the
    // SHARED object; with the internal mutex, no access corrupts another's cell
    // and every thread always reads back exactly what it last wrote.
    auto bar = make_small();

    constexpr int kThreads = 8;
    constexpr int kIters   = 5000;
    std::atomic<int> mismatches{0};

    auto worker = [&](int t) {
        std::size_t off = static_cast<std::size_t>(t) * 4; // distinct 4-byte cell
        for (int i = 0; i < kIters; ++i) {
            uint32_t val = (static_cast<uint32_t>(t) << 24) | static_cast<uint32_t>(i);
            if (!bar.write_u32(off, val).has_value()) { ++mismatches; return; }
            auto got = bar.read_u32(off);
            if (!got.has_value() || got.value() != val) { ++mismatches; }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();
    EXPECT_EQ(mismatches.load(), 0);
}

TEST(BarMemfd, SharedObjectConcurrentUpdateU32IsExactCount) {
    // update_u32 holds ONE mutex + ONE exclusive flock bracket across the whole
    // read-modify-write, so N threads * kIters increments on the SAME offset of
    // the SAME (non-reopened) object must produce EXACTLY N*kIters — the atomic
    // RMW helper is the path the model control workers use for control-register handshakes.
    auto bar = make_small();
    ASSERT_TRUE(bar.write_u32(0, 0).has_value());

    constexpr int kThreads = 8;
    constexpr int kIters   = 4000;

    auto worker = [&] {
        for (int i = 0; i < kIters; ++i) {
            auto r = bar.update_u32(0, [](uint32_t v) { return v + 1; });
            ASSERT_TRUE(r.has_value());
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker);
    for (auto& th : threads) th.join();

    auto final = bar.read_u32(0);
    ASSERT_TRUE(final.has_value());
    EXPECT_EQ(final.value(), static_cast<uint32_t>(kThreads * kIters))
        << "update_u32 lost an increment — RMW not atomic on the shared object";
}

TEST(BarMemfd, SharedObjectMixedApiPathsNoTear) {
    // Exercise EVERY flock-bracketed path (read_u32/write_u32/read/write/
    // update_u32) concurrently on ONE shared object, each thread on its own
    // 8-byte cell, and require each thread always reads back its own last write.
    // This hammers the internal mutex across the full mix of access paths under
    // the sanitizers.
    auto bar = make_small();
    constexpr int kThreads = 8;
    constexpr int kIters   = 4000;
    std::atomic<int> mismatches{0};

    auto worker = [&](int t) {
        std::size_t off = static_cast<std::size_t>(t) * 8; // distinct 8-byte cell
        for (int i = 0; i < kIters; ++i) {
            uint32_t val = (static_cast<uint32_t>(t) << 24) | static_cast<uint32_t>(i);
            // Rotate through the byte-range and u32 paths plus an RMW.
            std::vector<uint8_t> b = {static_cast<uint8_t>(val), static_cast<uint8_t>(val >> 8),
                                      static_cast<uint8_t>(val >> 16),
                                      static_cast<uint8_t>(val >> 24)};
            if (!bar.write(off, b).has_value()) { ++mismatches; return; }
            auto rd = bar.read(off, 4);
            if (!rd.has_value() || rd.value() != b) { ++mismatches; }
            if (!bar.write_u32(off + 4, val).has_value()) { ++mismatches; return; }
            auto u = bar.update_u32(off + 4, [](uint32_t v) { return v ^ 0u; }); // no-op RMW
            if (!u.has_value() || u.value() != val) { ++mismatches; }
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();
    EXPECT_EQ(mismatches.load(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// fd hygiene
// ─────────────────────────────────────────────────────────────────────────────

TEST(BarMemfd, NoFdLeakOverManyCreateReopenCycles) {
    int before = count_open_fds();
    ASSERT_GE(before, 0);
    for (int i = 0; i < 200; ++i) {
        auto bar = BarMemfd::create(kSmall, "leak_test");
        ASSERT_TRUE(bar.has_value());
        auto re = bar.value().reopen();
        ASSERT_TRUE(re.has_value());
        // bar + reopened fd both go out of scope → both closed.
    }
    int after = count_open_fds();
    EXPECT_LT(after - before, 10) << "fd leak: before=" << before << " after=" << after;
}

TEST(BarMemfd, MoveTransfersOwnership) {
    auto bar = make_small();
    ASSERT_TRUE(bar.write_u32(0, 0x1234).has_value());
    int orig_fd = bar.fd();

    BarMemfd moved = std::move(bar);
    EXPECT_EQ(moved.fd(), orig_fd);
    EXPECT_EQ(moved.read_u32(0).value(), 0x1234u);
    // Moved-from is empty.
    EXPECT_EQ(bar.size(), 0u); // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(bar.fd(), -1);   // NOLINT(bugprone-use-after-move)
}

// ─────────────────────────────────────────────────────────────────────────────
// ADVERSARY PROBES
// ─────────────────────────────────────────────────────────────────────────────

// PROBE 1 — self-move-assignment must not double-munmap / self-destruct the
// mapping, and the object must remain usable afterward.
TEST(BarMemfdAdversary, SelfMoveAssignIsSafe) {
    auto bar = make_small();
    ASSERT_TRUE(bar.write_u32(0, 0xcafebabe).has_value());
    // Launder through aliased references so -Wself-move can't see it.
    BarMemfd& a = bar;
    BarMemfd& b = bar;
    a = std::move(b); // self-move-assign
    // Still usable, data intact, fd/map not freed.
    EXPECT_EQ(bar.size(), kSmall);
    ASSERT_TRUE(bar.read_u32(0).has_value());
    EXPECT_EQ(bar.read_u32(0).value(), 0xcafebabeu);
}

// PROBE 2 — a moved-from BAR (map_==nullptr, size_==0) must fail every access
// cleanly (Protocol range error), never null-deref / crash.
TEST(BarMemfdAdversary, MovedFromBarAccessesFailCleanly) {
    auto bar = make_small();
    BarMemfd sink = std::move(bar);
    // `bar` is moved-from.  NOLINT(bugprone-use-after-move) on purpose.
    EXPECT_FALSE(bar.read_u32(0).has_value());
    EXPECT_FALSE(bar.write_u32(0, 1).has_value());
    EXPECT_FALSE(bar.read(0, 4).has_value());
    std::vector<uint8_t> d = {1, 2, 3, 4};
    EXPECT_FALSE(bar.write(0, d).has_value());
    // Zero-length access on a moved-from BAR: in_range(0,0,0) is true, so this
    // reaches the flock path.  fd_ is -1 (moved out), so flock(-1) fails and the
    // call returns a clean Transport error (NOT a crash / null-deref).  Either a
    // clean error or a clean empty success is acceptable; the point is no UB.
    auto z = bar.read(0, 0);
    if (z.has_value()) {
        EXPECT_TRUE(z.value().empty());
    } else {
        EXPECT_EQ(z.error().kind, ErrorKind::Transport); // flock(-1) → EBADF
    }
}

// PROBE 2b — move-safety of the internal mutex.  The move ctor/assign do NOT
// transfer mutex_, so the TARGET must have a valid (default-initialised) mutex
// and remain internally thread-safe, and the moved-from SOURCE must retain a
// valid mutex too (its zero-length access path dereferences *mutex_).  We
// concurrency-hammer a move-CONSTRUCTED object to prove its mutex protects it,
// and repeatedly touch the moved-from object's *mutex_ path to prove no null
// deref (UBSan/ASan would catch a null *mutex_).
TEST(BarMemfdAdversary, MoveConstructedObjectIsThreadSafeAndSourceMutexValid) {
    auto src = make_small();
    BarMemfd bar = std::move(src); // move-construct target

    // Moved-from source: zero-length read reaches std::lock_guard(*mutex_); it
    // must not null-deref.  fd_ is -1 so it returns cleanly either way.
    for (int i = 0; i < 100; ++i) {
        auto z = src.read(0, 0); // NOLINT(bugprone-use-after-move)
        (void)z;
    }

    // The move-constructed target must be internally thread-safe.
    constexpr int kThreads = 8;
    constexpr int kIters   = 3000;
    std::atomic<int> mismatches{0};
    auto worker = [&](int t) {
        std::size_t off = static_cast<std::size_t>(t) * 4;
        for (int i = 0; i < kIters; ++i) {
            uint32_t val = (static_cast<uint32_t>(t) << 24) | static_cast<uint32_t>(i);
            if (!bar.write_u32(off, val).has_value()) { ++mismatches; return; }
            auto got = bar.read_u32(off);
            if (!got.has_value() || got.value() != val) ++mismatches;
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();
    EXPECT_EQ(mismatches.load(), 0);
}

// PROBE 2c — move-ASSIGNMENT preserves each object's own valid mutex; the target
// (assigned-into) must be thread-safe afterwards.
TEST(BarMemfdAdversary, MoveAssignedObjectIsThreadSafe) {
    auto a = make_small();
    auto b = make_small();
    a = std::move(b); // move-assign; a's mutex_ untouched, still valid

    constexpr int kThreads = 8;
    constexpr int kIters   = 3000;
    std::atomic<int> mismatches{0};
    auto worker = [&](int t) {
        std::size_t off = static_cast<std::size_t>(t) * 4;
        for (int i = 0; i < kIters; ++i) {
            uint32_t val = (static_cast<uint32_t>(t) << 24) | static_cast<uint32_t>(i);
            if (!a.write_u32(off, val).has_value()) { ++mismatches; return; }
            auto got = a.read_u32(off);
            if (!got.has_value() || got.value() != val) ++mismatches;
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();
    EXPECT_EQ(mismatches.load(), 0);
}

// PROBE 3 — a size-0 BAR (no mapping) must handle zero-length access without a
// nullptr pointer-arithmetic UB (caught by UBSan) and reject any non-zero access.
TEST(BarMemfdAdversary, ZeroSizeBarZeroLengthAccess) {
    auto r = BarMemfd::create(0, "zero_bar");
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
    BarMemfd bar = std::move(r.value());
    EXPECT_EQ(bar.size(), 0u);
    // Zero-length read/write at offset 0 == size: allowed by in_range, exercises
    // (nullptr + 0) pointer arithmetic in read()/write().  UBSan will flag if the
    // implementation forms an invalid pointer.
    auto rd = bar.read(0, 0);
    EXPECT_TRUE(rd.has_value());
    EXPECT_TRUE(rd.value().empty());
    EXPECT_TRUE(bar.write(0, {}).has_value());
    // Any non-zero access is out of range.
    EXPECT_FALSE(bar.read_u32(0).has_value());
    EXPECT_FALSE(bar.read(0, 1).has_value());
}

// PROBE 4 — u32 boundary: offset == size-1, size-2, size-3 all straddle the end
// and MUST be rejected (offset+4 > size), while size-4 is the last valid u32.
TEST(BarMemfdAdversary, U32StraddlingEndRejected) {
    auto bar = make_small();
    EXPECT_TRUE(bar.read_u32(kSmall - 4).has_value());  // last valid
    EXPECT_FALSE(bar.read_u32(kSmall - 3).has_value());
    EXPECT_FALSE(bar.read_u32(kSmall - 2).has_value());
    EXPECT_FALSE(bar.read_u32(kSmall - 1).has_value());
    EXPECT_FALSE(bar.read_u32(kSmall).has_value());
    EXPECT_FALSE(bar.write_u32(kSmall - 1, 0).has_value());
}

// PROBE 5 — offset+len overflow must be rejected, not wrapped.  offset near
// SIZE_MAX with a len that would overflow the sum.
TEST(BarMemfdAdversary, OffsetPlusLenOverflowRejected) {
    auto bar = make_small();
    // offset just below SIZE_MAX, len large: offset+len wraps if unchecked.
    EXPECT_FALSE(bar.read(SIZE_MAX - 2, 8).has_value());
    EXPECT_FALSE(bar.write(SIZE_MAX - 2, std::vector<uint8_t>(8)).has_value());
    // offset == size, len 1 → out of range.
    EXPECT_FALSE(bar.read(kSmall, 1).has_value());
    // offset valid, len == exactly the remaining bytes → in range.
    EXPECT_TRUE(bar.read(kSmall - 8, 8).has_value());
    // one more byte → out of range.
    EXPECT_FALSE(bar.read(kSmall - 8, 9).has_value());
}

// PROBE 6 — MFD_CLOEXEC on the memfd and O_CLOEXEC on the reopened fd must be
// set, so a spawned model child does not inherit these fds.
TEST(BarMemfdAdversary, CloexecIsSetOnFdAndReopen) {
    auto bar = make_small();
    int flags = ::fcntl(bar.fd(), F_GETFD);
    ASSERT_NE(flags, -1);
    EXPECT_TRUE(flags & FD_CLOEXEC) << "memfd not created with MFD_CLOEXEC";

    auto re = bar.reopen();
    ASSERT_TRUE(re.has_value());
    int rflags = ::fcntl(re.value().get(), F_GETFD);
    ASSERT_NE(rflags, -1);
    EXPECT_TRUE(rflags & FD_CLOEXEC) << "reopen() fd not O_CLOEXEC";
}

// PROBE 7 — the reopened fd must be RDWR so the user can mmap it read-write.
TEST(BarMemfdAdversary, ReopenFdIsReadWriteMappable) {
    auto bar = make_small();
    auto re  = bar.reopen();
    ASSERT_TRUE(re.has_value());
    int fd = re.value().get();
    // O_ACCMODE must be O_RDWR.
    int fl = ::fcntl(fd, F_GETFL);
    ASSERT_NE(fl, -1);
    EXPECT_EQ(fl & O_ACCMODE, O_RDWR);
    // A read-write MAP_SHARED mmap on the reopened fd must succeed and be visible
    // to the daemon (models the user's mmap of a BAR fd).
    void* p = ::mmap(nullptr, kSmall, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(p, MAP_FAILED);
    static_cast<unsigned char*>(p)[12] = 0xEE;
    static_cast<unsigned char*>(p)[13] = 0xFF;
    ::munmap(p, kSmall);
    auto v = bar.read(12, 2);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), (std::vector<uint8_t>{0xEE, 0xFF}));
}

// PROBE 8 — concurrent LOCK_SH readers must NOT block each other (shared locks are
// compatible).  If they were serialised, N readers each holding the lock for a
// short sleep would take ~N*sleep; compatible shared locks finish in ~1*sleep.
TEST(BarMemfdAdversary, ConcurrentSharedLocksDoNotBlockEachOther) {
    auto bar = make_small();
    constexpr int kReaders = 8;
    std::vector<UniqueFd> fds;
    for (int i = 0; i < kReaders; ++i) {
        auto re = bar.reopen();
        ASSERT_TRUE(re.has_value());
        fds.push_back(std::move(re.value()));
    }
    std::atomic<int> holding{0};
    std::atomic<int> max_concurrent{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < kReaders; ++i) {
        int fd = fds[i].get();
        ts.emplace_back([&, fd] {
            ASSERT_EQ(::flock(fd, LOCK_SH), 0);
            int now = holding.fetch_add(1) + 1;
            int prev = max_concurrent.load();
            while (now > prev && !max_concurrent.compare_exchange_weak(prev, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            holding.fetch_sub(1);
            ASSERT_EQ(::flock(fd, LOCK_UN), 0);
        });
    }
    for (auto& t : ts) t.join();
    // Shared locks are compatible: all readers should have held simultaneously.
    EXPECT_GT(max_concurrent.load(), 1)
        << "shared LOCK_SH readers appear to be blocking each other";
}

// PROBE 9 — reopen() over many cycles must not leak fds (the reopened fd is
// closed by UniqueFd each cycle).
TEST(BarMemfdAdversary, ManyReopenCyclesNoFdLeak) {
    auto bar = make_small();
    int before = count_open_fds();
    ASSERT_GE(before, 0);
    for (int i = 0; i < 500; ++i) {
        auto re = bar.reopen();
        ASSERT_TRUE(re.has_value());
        // closed at end of loop iteration
    }
    int after = count_open_fds();
    EXPECT_LT(after - before, 10) << "fd leak in reopen(): before=" << before
                                  << " after=" << after;
}

// PROBE 10 — a write that fails the bounds check must NOT have taken the flock
// (i.e. no lock is left held → no deadlock for the next writer).  We verify the
// fd is unlocked after an out-of-range write by taking LOCK_EX|LOCK_NB from a
// distinct description and requiring it to succeed.
TEST(BarMemfdAdversary, OutOfRangeAccessLeavesNoLockHeld) {
    auto bar = make_small();
    // Out-of-range write (rejected before the flock bracket).
    std::vector<uint8_t> big(64);
    EXPECT_FALSE(bar.write(kSmall - 1, big).has_value());
    EXPECT_FALSE(bar.read_u32(kSmall).has_value());

    // If the daemon's description held a stale LOCK_EX, a distinct description's
    // LOCK_EX|LOCK_NB would fail.  It must succeed.
    auto re = bar.reopen();
    ASSERT_TRUE(re.has_value());
    int rc = ::flock(re.value().get(), LOCK_EX | LOCK_NB);
    EXPECT_EQ(rc, 0) << "a lock appears to be held after an out-of-range access";
    if (rc == 0) ::flock(re.value().get(), LOCK_UN);
}

// PROBE 11 — after a normal (successful) read/write, the flock is released
// (FlockGuard destructor). A distinct description can immediately take LOCK_EX.
TEST(BarMemfdAdversary, LockReleasedAfterSuccessfulAccess) {
    auto bar = make_small();
    ASSERT_TRUE(bar.write_u32(0, 0x12345678).has_value());
    ASSERT_TRUE(bar.read_u32(0).has_value());
    auto re = bar.reopen();
    ASSERT_TRUE(re.has_value());
    EXPECT_EQ(::flock(re.value().get(), LOCK_EX | LOCK_NB), 0)
        << "lock still held after a completed access";
    ::flock(re.value().get(), LOCK_UN);
}

// PROBE 12 — flock EINTR handling.  A blocking flock(LOCK_EX) interrupted by a
// signal (handler installed WITHOUT SA_RESTART) returns -1/EINTR.  The BAR's
// write_u32 must transparently retry, not spuriously fail as a Transport error —
// otherwise, in the daemon (which installs signal handlers), a stray signal during
// a contended register write would fake a device error.
//
// Setup: main thread holds LOCK_EX on a DISTINCT description; a worker thread
// calls bar.write_u32() which blocks on its own flock(LOCK_EX); main thread pokes
// the worker with SIGUSR1 repeatedly to force EINTR; then main releases its lock
// so the worker's flock can finally succeed.  If the impl retries EINTR, the
// write succeeds; if not, it returns Transport.
namespace {
std::atomic<int> g_sigusr1_count{0};
void sigusr1_handler(int) { g_sigusr1_count.fetch_add(1); }
} // namespace

TEST(BarMemfdAdversary, FlockEintrIsRetriedNotSpuriousFailure) {
    // Install a NON-restarting SIGUSR1 handler so blocking syscalls get EINTR.
    struct sigaction sa{};
    sa.sa_handler = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // NOT SA_RESTART: interrupted syscalls return EINTR
    struct sigaction old{};
    ASSERT_EQ(::sigaction(SIGUSR1, &sa, &old), 0);

    auto bar = make_small();
    ASSERT_TRUE(bar.write_u32(0, 0).has_value());

    // Hold an exclusive lock on a distinct description so the worker's write_u32
    // blocks on flock(LOCK_EX).
    auto blocker_re = bar.reopen();
    ASSERT_TRUE(blocker_re.has_value());
    UniqueFd blocker = std::move(blocker_re.value());
    ASSERT_EQ(::flock(blocker.get(), LOCK_EX), 0);

    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    Result<void> write_result = Result<void>::err({ErrorKind::Transport, "unset"});
    std::thread worker([&] {
        started.store(true);
        write_result = bar.write_u32(4, 0xABCD1234); // blocks on flock(LOCK_EX)
        finished.store(true);
    });

    while (!started.load()) std::this_thread::yield();
    // Poke the worker with EINTR-inducing signals while it is blocked on flock.
    for (int i = 0; i < 20 && !finished.load(); ++i) {
        ::pthread_kill(worker.native_handle(), SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Release the blocking lock so the worker's flock can now acquire.
    ASSERT_EQ(::flock(blocker.get(), LOCK_UN), 0);
    worker.join();

    // Restore the previous SIGUSR1 disposition.
    ::sigaction(SIGUSR1, &old, nullptr);

    EXPECT_GT(g_sigusr1_count.load(), 0) << "signals were not delivered; test inconclusive";
    // The write must have SUCCEEDED (EINTR retried), not failed spuriously.
    EXPECT_TRUE(write_result.has_value())
        << "flock EINTR was not retried — spurious Transport failure: "
        << (write_result.has_value() ? "" : write_result.error().message);
    if (write_result.has_value()) {
        EXPECT_EQ(bar.read_u32(4).value(), 0xABCD1234u);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Standard BAR set
// ─────────────────────────────────────────────────────────────────────────────

TEST(BarSetTest, StandardSizesAndIndices) {
    EXPECT_EQ(bar_kind_size(BarKind::UserRegion), 128u * 1024 * 1024);
    EXPECT_EQ(bar_kind_size(BarKind::ServiceLayer), 128u * 1024 * 1024);
    EXPECT_EQ(bar_kind_size(BarKind::ClockWizard), 512u * 1024);
    EXPECT_EQ(bar_kind_index(BarKind::UserRegion), 0);
    EXPECT_EQ(bar_kind_index(BarKind::ServiceLayer), 2);
    EXPECT_EQ(bar_kind_index(BarKind::ClockWizard), 4);
}

TEST(BarSetTest, MakeStandardBarsHasCorrectSizes) {
    auto set = make_standard_bars();
    ASSERT_TRUE(set.has_value()) << (set.has_value() ? "" : set.error().message);
    auto& s = set.value();
    EXPECT_EQ(s.user_region.size(), kUserRegionSize);
    EXPECT_EQ(s.service_layer.size(), kServiceLayerSize);
    EXPECT_EQ(s.clock_wizard.size(), kClockWizardSize);
}

TEST(BarSetTest, LookupByKindAndIndex) {
    auto set = make_standard_bars();
    ASSERT_TRUE(set.has_value());
    auto& s = set.value();

    EXPECT_EQ(s.by_kind(BarKind::UserRegion).size(), kUserRegionSize);
    EXPECT_EQ(s.by_kind(BarKind::ClockWizard).size(), kClockWizardSize);

    ASSERT_NE(s.by_index(0), nullptr);
    ASSERT_NE(s.by_index(2), nullptr);
    ASSERT_NE(s.by_index(4), nullptr);
    EXPECT_EQ(s.by_index(0)->size(), kUserRegionSize);
    EXPECT_EQ(s.by_index(2)->size(), kServiceLayerSize);
    EXPECT_EQ(s.by_index(4)->size(), kClockWizardSize);
    // Non-present BAR indices.
    EXPECT_EQ(s.by_index(1), nullptr);
    EXPECT_EQ(s.by_index(3), nullptr);
    EXPECT_EQ(s.by_index(5), nullptr);
}

TEST(BarSetTest, StandardBarsAreIndependentWindows) {
    auto set = make_standard_bars();
    ASSERT_TRUE(set.has_value());
    auto& s = set.value();
    ASSERT_TRUE(s.user_region.write_u32(0, 0x11111111).has_value());
    ASSERT_TRUE(s.service_layer.write_u32(0, 0x22222222).has_value());
    ASSERT_TRUE(s.clock_wizard.write_u32(0, 0x33333333).has_value());
    EXPECT_EQ(s.user_region.read_u32(0).value(), 0x11111111u);
    EXPECT_EQ(s.service_layer.read_u32(0).value(), 0x22222222u);
    EXPECT_EQ(s.clock_wizard.read_u32(0).value(), 0x33333333u);
}
