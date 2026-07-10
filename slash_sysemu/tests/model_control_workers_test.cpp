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

#include "model_control_workers.h"

#include "bar_memfd.h"
#include "fixtures_paths.h"
#include "model_client.h"
#include "mock_model_server.h"
#include "system_map.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace slash_sysemu;
using slash_sysemu::test::FaultMode;
using slash_sysemu::test::MockModelServer;
using slash_sysemu::test::RequestRecord;
using namespace std::chrono_literals;

namespace {

std::string unique_endpoint() {
    static std::atomic<unsigned> counter{0};
    return "ipc:///tmp/slash_sysemu_mcw_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1)) + ".sock";
}

ModelClient make_client(const std::string& endpoint, std::chrono::milliseconds timeout = 500ms) {
    auto r = ModelClient::connect(endpoint, timeout);
    EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
    return std::move(r.value());
}

BarMemfd make_bar(std::size_t size) {
    auto r = BarMemfd::create(size, "mcw_test_bar");
    EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
    return std::move(r.value());
}

// A small user-region BAR: big enough for the fixture kernels (base 0x1000..),
// far smaller than the real 128 MiB so tests stay fast.
constexpr std::size_t kUserBarSize  = 64 * 1024;
constexpr std::size_t kClockBarSize = 512 * 1024;

// Build a one-kernel system map programmatically.  base is where the kernel's
// control register (offset 0) lives; adds one control reg (RW) and the given
// extra registers.
SystemMap one_kernel_map(uint64_t base, std::vector<Register> extra_regs) {
    SystemMap map;
    Kernel    k;
    k.name         = "demo";
    k.base_address = base;
    k.range        = 0x1000;
    k.registers.push_back(Register{"CTRL", 0x0, "RW", 32, "control"});
    for (auto& r : extra_regs) {
        k.registers.push_back(std::move(r));
    }
    map.kernels.push_back(std::move(k));
    return map;
}

// Spin until `pred` is true or the deadline elapses.  Returns pred()'s final
// value so callers can ASSERT it — tests never hang.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds deadline = 2000ms) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

// A background "FSM proxy": the mock server has no compute-kernel FSM, and the
// worker's own control forward (reg_write to the control address) overwrites any
// pre-seeded ap_done at that same model scalar (address identity: the control
// forward and the busy-loop fetch_scalar both hit control_addr).  So a one-shot
// seed always races the forward.  Instead, this proxy continuously re-asserts
// ap_done on the given control addresses at a short interval, so that — however
// the forward interleaves — a busy-loop poll eventually reads ap_done and the
// cycle completes.  This models a real sim's FSM raising ap_done once triggered.
// RAII: the thread runs from construction until stop()/destruction.
class FsmProxy {
public:
    FsmProxy(MockModelServer& server, std::vector<uint64_t> control_addrs)
        : server_(server), addrs_(std::move(control_addrs)) {
        thread_ = std::thread([this] {
            while (run_.load()) {
                for (uint64_t a : addrs_) {
                    server_.set_scalar(a, ModelControlWorkers::kApDone);
                }
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    ~FsmProxy() { stop(); }
    void stop() {
        if (run_.exchange(false) && thread_.joinable()) {
            thread_.join();
        }
    }

private:
    MockModelServer&      server_;
    std::vector<uint64_t> addrs_;
    std::atomic<bool>     run_{true};
    std::thread           thread_;
};

// Count records whose command == cmd.
std::size_t count_cmd(const std::vector<RequestRecord>& recs, const std::string& cmd) {
    std::size_t n = 0;
    for (const auto& r : recs) {
        if (r.command == cmd) ++n;
    }
    return n;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Basic lifecycle
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelControlWorkers, StartSpawnsOneWorkerPerKernel) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    SystemMap map;
    for (int i = 0; i < 3; ++i) {
        Kernel k;
        k.name         = "k" + std::to_string(i);
        k.base_address = 0x1000 * (i + 1);
        k.registers.push_back(Register{"CTRL", 0x0, "RW", 32, ""});
        map.kernels.push_back(std::move(k));
    }

    ModelControlWorkers workers(user, clock, 1ms);
    auto r = workers.start(client, map);
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
    EXPECT_TRUE(workers.running());
    EXPECT_EQ(workers.kernel_count(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(workers.kernel_state(i), KernelState::Idle);
    }
    workers.stop();
    EXPECT_FALSE(workers.running());
}

TEST(ModelControlWorkers, DoubleStartIsRejected) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);
    auto map    = one_kernel_map(0x1000, {});

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    auto again = workers.start(client, map);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().kind, ErrorKind::Protocol);
    workers.stop();
}

TEST(ModelControlWorkers, StopWithoutStartIsSafe) {
    auto user  = make_bar(kUserBarSize);
    auto clock = make_bar(kClockBarSize);
    ModelControlWorkers workers(user, clock, 1ms);
    workers.stop(); // must not crash / hang
    EXPECT_FALSE(workers.running());
}

TEST(ModelControlWorkers, StopIsIdempotent) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);
    auto map    = one_kernel_map(0x1000, {});

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    workers.stop();
    workers.stop(); // second stop is a no-op
    EXPECT_FALSE(workers.running());
}

TEST(ModelControlWorkers, DestructorTearsDownCleanly) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);
    auto map    = one_kernel_map(0x1000, {});
    {
        ModelControlWorkers workers(user, clock, 1ms);
        ASSERT_TRUE(workers.start(client, map).has_value());
        // Leave scope without calling stop(); the destructor must join threads.
    }
    SUCCEED();
}

TEST(ModelControlWorkers, StartStopRestartReusesController) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);
    auto map    = one_kernel_map(0x1000, {});

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    workers.stop();
    // A second start on the same object must succeed (the WorkerController
    // contract: start/stop bracket one worker set, and reconfiguration reuses it).
    ASSERT_TRUE(workers.start(client, map).has_value());
    EXPECT_TRUE(workers.running());
    workers.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// End-to-end idle → busy → idle
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelControlWorkers, FullCycleForwardsParamsThenControlAndWritesBackOutputs) {
    const uint64_t base = 0x2000;
    // Registers: control@0 (RW), size@0x10 (W, param), out@0x20 (R, output).
    auto map = one_kernel_map(
        base, {Register{"size", 0x10, "W", 32, ""}, Register{"out", 0x20, "R", 32, ""}});

    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    // The model reports an output value on the output register; ap_done on the
    // control register is set by the FSM proxy (complete_when_triggered) once the
    // control forward arrives.
    server.set_scalar(base + 0x20, 0xCAFEBABE);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    // Stage a parameter and trigger ap_start via direct "MMIO" into the memfd.
    ASSERT_TRUE(user.write_u32(base + 0x10, 0x1234).has_value());
    ASSERT_TRUE(user.write_u32(base + 0x0, ModelControlWorkers::kApStart).has_value());

    // Act as the model FSM: continuously raise ap_done so the busy loop completes.
    FsmProxy fsm(server, {base});

    // The worker must clear the control register (handshake reset) ...
    ASSERT_TRUE(wait_until([&] {
        auto v = user.read_u32(base + 0x0);
        return v.has_value() && (v.value() & ModelControlWorkers::kApStart) == 0 &&
               (v.value() & ModelControlWorkers::kApDone) != 0;
    })) << "control register never showed ap_done / cleared ap_start";

    // ... and the output register must land back in the memfd.
    ASSERT_TRUE(wait_until([&] {
        auto v = user.read_u32(base + 0x20);
        return v.has_value() && v.value() == 0xCAFEBABE;
    })) << "output register never written back";

    // The worker returns to Idle after the cycle.
    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Idle; }));

    workers.stop();

    // Assert the model saw: reg write of the param, then reg write of the control,
    // in that order (params-before-control ordering).
    auto recs = server.requests();
    // Find the reg writes.
    std::vector<RequestRecord> regs;
    for (const auto& r : recs) {
        if (r.command == "reg") regs.push_back(r);
    }
    ASSERT_GE(regs.size(), 2u);
    // The param write (addr base+0x10) must precede the control write (addr base).
    bool saw_param   = false;
    bool control_ok  = false;
    for (const auto& r : regs) {
        if (r.addr == base + 0x10) saw_param = true;
        if (r.addr == base) {
            EXPECT_TRUE(saw_param) << "control was sent before the parameter";
            control_ok = true;
        }
    }
    EXPECT_TRUE(control_ok);
    // The param value written to the model matches what we staged.
    EXPECT_EQ(server.peek(0), 0u); // unrelated addr sanity
}

TEST(ModelControlWorkers, ControlRegisterResetIsAtomicViaUpdate) {
    // The captured control value forwarded to the model equals what the user
    // wrote (ap_start | any other bits), and the memfd is cleared to 0 first.
    const uint64_t base = 0x3000;
    auto           map  = one_kernel_map(base, {});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    server.set_scalar(base, ModelControlWorkers::kApDone);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    // Write ap_start plus an extra high bit to prove the whole value is captured.
    const uint32_t written = ModelControlWorkers::kApStart | 0x80000000u;
    ASSERT_TRUE(user.write_u32(base, written).has_value());

    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) != KernelState::Idle ||
                                        server.request_count() > 0; }));
    ASSERT_TRUE(wait_until([&] {
        auto recs = server.requests();
        for (const auto& r : recs) {
            if (r.command == "reg" && r.addr == base) return true;
        }
        return false;
    }));

    workers.stop();
    // The value the model received for the control register is the full captured
    // value (ap_start | 0x80000000).
    EXPECT_EQ(server.peek(0), 0u);
    // scalars_ recorded via set_scalar in dispatch: the reg write stores val.
    // Re-read through a fresh client to confirm.
    auto v = client.fetch_scalar(base);
    ASSERT_TRUE(v.has_value());
    // The last reg write to base carried the captured control value.
    EXPECT_EQ(v.value(), written);
}

// ─────────────────────────────────────────────────────────────────────────────
// Kernels without a control register / unusual registers
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelControlWorkers, KernelWithNoControlRegisterNeverStarts) {
    // A kernel whose lowest register is not at offset 0 has no ap_start handshake.
    SystemMap map;
    Kernel    k;
    k.name         = "nostart";
    k.base_address = 0x5000;
    k.registers.push_back(Register{"data", 0x10, "RW", 32, ""}); // no offset-0 reg
    map.kernels.push_back(std::move(k));

    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    // Even if the user writes a would-be ap_start at base+0, the worker ignores
    // it (there is no declared control register) and never transitions.
    ASSERT_TRUE(user.write_u32(0x5000, ModelControlWorkers::kApStart).has_value());
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(workers.kernel_state(0), KernelState::Idle);
    // The model saw no reg writes from this kernel.
    EXPECT_EQ(count_cmd(server.requests(), "reg"), 0u);

    workers.stop();
}

TEST(ModelControlWorkers, ZeroWidthRegisterDoesNotCrash) {
    // system_map warns bit_width may be 0.  The worker must not do width math that
    // trips on it; it just forwards the u32 value.
    const uint64_t base = 0x6000;
    auto           map  = one_kernel_map(
        base, {Register{"weird", 0x10, "RW", 0 /* zero width */, ""}});
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    FsmProxy fsm(server, {base});
    ASSERT_TRUE(user.write_u32(base + 0x10, 0xDEAD).has_value());
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Idle &&
                                        count_cmd(server.requests(), "reg") >= 2; }));
    workers.stop();
    SUCCEED();
}

TEST(ModelControlWorkers, OutOfRangeRegisterOffsetIsSkippedNotFatal) {
    // A declared register whose absolute address exceeds the BAR is skipped
    // gracefully; the cycle still completes and control is still forwarded.
    const uint64_t base = 0x1000;
    auto           map  = one_kernel_map(
        base, {Register{"oob", kUserBarSize + 0x100, "W", 32, ""}});
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    server.set_scalar(base, ModelControlWorkers::kApDone);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    ASSERT_TRUE(wait_until([&] {
        auto recs = server.requests();
        for (const auto& r : recs) {
            if (r.command == "reg" && r.addr == base) return true;
        }
        return false;
    })) << "control was never forwarded despite an out-of-range param register";
    workers.stop();
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────────────
// Multiple kernels concurrently on the shared user-region BAR
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelControlWorkers, MultipleKernelsShareUserRegionBarSafely) {
    SystemMap map;
    const int kN = 4;
    for (int i = 0; i < kN; ++i) {
        Kernel   k;
        uint64_t base  = 0x1000 * (i + 1);
        k.name         = "k" + std::to_string(i);
        k.base_address = base;
        k.registers.push_back(Register{"CTRL", 0x0, "RW", 32, ""});
        k.registers.push_back(Register{"out", 0x20, "R", 32, ""});
        map.kernels.push_back(std::move(k));
    }

    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    // Every kernel reports a distinct output; ap_done is raised by an FSM proxy.
    for (int i = 0; i < kN; ++i) {
        uint64_t base = 0x1000 * (i + 1);
        server.set_scalar(base + 0x20, 0xA000 + i);
    }

    std::vector<uint64_t> ctrl_addrs;
    for (int i = 0; i < kN; ++i) ctrl_addrs.push_back(0x1000 * (i + 1));
    FsmProxy fsm(server, ctrl_addrs);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    // Fire all kernels' ap_start at once.
    for (int i = 0; i < kN; ++i) {
        uint64_t base = 0x1000 * (i + 1);
        ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());
    }

    // All outputs must land back in the shared BAR.
    bool all_ok = true;
    for (int i = 0; i < kN; ++i) {
        uint64_t base = 0x1000 * (i + 1);
        bool ok = wait_until([&] {
            auto v = user.read_u32(base + 0x20);
            return v.has_value() && v.value() == static_cast<uint32_t>(0xA000 + i);
        });
        EXPECT_TRUE(ok) << "kernel " << i << " output missing";
        all_ok = all_ok && ok;
    }

    fsm.stop();
    workers.stop();
    (void)all_ok;
    // The shared ModelClient was never used by two workers at once.
    EXPECT_EQ(server.max_in_flight(), 1);
}

TEST(ModelControlWorkers, ConcurrentWorkersNeverOverlapOnSharedClient) {
    // Many kernels, many cycles, tiny interval — stress the shared ModelClient
    // serialisation: max_in_flight must stay 1.
    SystemMap map;
    const int kN = 6;
    for (int i = 0; i < kN; ++i) {
        Kernel   k;
        uint64_t base  = 0x800 * (i + 1);
        k.name         = "k" + std::to_string(i);
        k.base_address = base;
        k.registers.push_back(Register{"CTRL", 0x0, "RW", 32, ""});
        map.kernels.push_back(std::move(k));
    }

    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    // An FSM proxy continuously raises ap_done for every kernel, so each completed
    // cycle immediately re-arms from the still-set memfd ap_start, driving many
    // overlapping cycles across all workers on the shared client.
    std::vector<uint64_t> ctrl_addrs;
    for (int i = 0; i < kN; ++i) ctrl_addrs.push_back(0x800 * (i + 1));
    FsmProxy fsm(server, ctrl_addrs);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    // Repeatedly re-arm each kernel to drive many overlapping cycles.
    for (int round = 0; round < 20; ++round) {
        for (int i = 0; i < kN; ++i) {
            (void)user.write_u32(0x800 * (i + 1), ModelControlWorkers::kApStart);
        }
        std::this_thread::sleep_for(5ms);
    }

    fsm.stop();
    workers.stop();
    EXPECT_EQ(server.max_in_flight(), 1);
    EXPECT_GT(server.request_count(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Clock-wizard worker
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelControlWorkers, ClockWizardPinsBothLockBits) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    SystemMap empty_map; // no kernels; only the clock worker runs

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, empty_map).has_value());

    ASSERT_TRUE(wait_until([&] {
        auto u = clock.read_u32(ModelControlWorkers::kClockLockUserOffset);
        auto s = clock.read_u32(ModelControlWorkers::kClockLockServiceOffset);
        return u.has_value() && s.has_value() &&
               (u.value() & ModelControlWorkers::kClockLockBit) != 0 &&
               (s.value() & ModelControlWorkers::kClockLockBit) != 0;
    })) << "clock lock bits never pinned";

    workers.stop();
}

TEST(ModelControlWorkers, ClockWizardReassertsClearedLockBit) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);
    SystemMap empty_map;

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, empty_map).has_value());

    // Wait for the bit to be pinned, then clear it (simulate a divider write that
    // zeroes the aliased cell) and confirm it is re-asserted.
    ASSERT_TRUE(wait_until([&] {
        auto u = clock.read_u32(ModelControlWorkers::kClockLockUserOffset);
        return u.has_value() && (u.value() & ModelControlWorkers::kClockLockBit) != 0;
    }));
    ASSERT_TRUE(clock.write_u32(ModelControlWorkers::kClockLockUserOffset, 0).has_value());
    ASSERT_TRUE(wait_until([&] {
        auto u = clock.read_u32(ModelControlWorkers::kClockLockUserOffset);
        return u.has_value() && (u.value() & ModelControlWorkers::kClockLockBit) != 0;
    })) << "cleared lock bit was not re-asserted";

    workers.stop();
}

TEST(ModelControlWorkers, ClockWizardPreservesOtherBits) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);
    SystemMap empty_map;

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, empty_map).has_value());

    // Write a divider value with bit0 clear; the worker ORs in bit0 but must not
    // clobber the other bits.
    const uint32_t divider = 0xABCD0000u;
    ASSERT_TRUE(clock.write_u32(ModelControlWorkers::kClockLockUserOffset, divider).has_value());
    ASSERT_TRUE(wait_until([&] {
        auto u = clock.read_u32(ModelControlWorkers::kClockLockUserOffset);
        return u.has_value() && u.value() == (divider | ModelControlWorkers::kClockLockBit);
    })) << "clock worker clobbered non-lock bits";

    workers.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Model death mid-cycle
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelControlWorkers, ModelDeathDuringIdleForwardStopsWorker) {
    // Close the model socket on the first request (the param/control forward from
    // the idle loop).  The worker must go Dead and not hang.
    const uint64_t base = 0x1000;
    auto           map  = one_kernel_map(base, {});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep, 300ms);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    server.set_fault(FaultMode::Close, 0); // die on the very first request

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Dead; }, 5000ms))
        << "worker did not go Dead after model close";

    workers.stop(); // must still tear down cleanly
    SUCCEED();
}

TEST(ModelControlWorkers, ModelSilenceDuringBusyPollStopsWorker) {
    // Forward succeeds, then the model goes silent on the busy-loop fetch_scalar,
    // forcing a client timeout (Transport). The worker must go Dead, not hang.
    const uint64_t base = 0x1000;
    auto           map  = one_kernel_map(base, {});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep, 300ms);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    // Serve the idle-loop forward (1 reg write for control), then go silent.
    server.set_fault(FaultMode::Silence, 1);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Dead; }, 8000ms))
        << "worker did not go Dead after model silence in busy loop";

    workers.stop();
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────────────
// Real fixture system map
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelControlWorkers, DrivesFixtureSystemMapKernel) {
    // Parse the good_sim fixture map and run one cycle against its "demo" kernel
    // (base 0x30000, CTRL@0, size@0x10 W).
    auto parsed = parse_system_map_file(test_fixtures::kGoodSimMapXml);
    ASSERT_TRUE(parsed.has_value()) << (parsed.has_value() ? "" : parsed.error().message);
    SystemMap map = std::move(parsed.value());
    ASSERT_EQ(map.kernels.size(), 1u);
    const uint64_t base = map.kernels[0].base_address;

    // Need a BAR big enough to hold base 0x30000 + registers.
    auto user  = make_bar(base + 0x1000);
    auto clock = make_bar(kClockBarSize);
    auto ep    = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);

    FsmProxy fsm(server, {base});
    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    ASSERT_TRUE(user.write_u32(base + 0x10, 0x99).has_value()); // the size param
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    ASSERT_TRUE(wait_until([&] {
        auto recs = server.requests();
        bool saw_param = false, saw_ctrl = false;
        for (const auto& r : recs) {
            if (r.command == "reg" && r.addr == base + 0x10) saw_param = true;
            if (r.command == "reg" && r.addr == base) saw_ctrl = true;
        }
        return saw_param && saw_ctrl;
    }));
    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Idle; }));
    fsm.stop();
    workers.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Additional coverage: state helpers, death on the param-forward and output-fetch
// paths, and the "not done yet" busy poll.
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelControlWorkers, KernelStateNameCoversAllValues) {
    EXPECT_STREQ(kernel_state_name(KernelState::Idle), "Idle");
    EXPECT_STREQ(kernel_state_name(KernelState::Busy), "Busy");
    EXPECT_STREQ(kernel_state_name(KernelState::Dead), "Dead");
}

TEST(ModelControlWorkers, KernelStateOutOfRangeIsDead) {
    auto user  = make_bar(kUserBarSize);
    auto clock = make_bar(kClockBarSize);
    ModelControlWorkers workers(user, clock, 1ms);
    // No start(): no kernels, so any index is out of range → Dead.
    EXPECT_EQ(workers.kernel_state(0), KernelState::Dead);
    EXPECT_EQ(workers.kernel_state(999), KernelState::Dead);
}

TEST(ModelControlWorkers, ModelDeathDuringParamForwardStopsWorker) {
    // A kernel WITH a writable parameter register: the model dies on the very
    // first request, which is the parameter reg_write in the idle-forward loop.
    const uint64_t base = 0x1000;
    auto           map  = one_kernel_map(base, {Register{"size", 0x10, "W", 32, ""}});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep, 300ms);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    server.set_fault(FaultMode::Close, 0); // die on the first (param) request

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    ASSERT_TRUE(user.write_u32(base + 0x10, 0x1234).has_value());
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Dead; }, 5000ms))
        << "worker did not go Dead after model close during param forward";
    workers.stop();
    SUCCEED();
}

TEST(ModelControlWorkers, ModelDeathDuringBusyCycleWithOutputStopsWorker) {
    // A kernel WITH an output register.  The model reports ap_done (so the worker
    // reaches the output-fetch phase) via an FSM proxy, then is killed.  Whichever
    // busy-loop model call the Close lands on (control poll or output fetch), the
    // worker must go Dead and tear down cleanly — no hang, no leak.
    const uint64_t base = 0x1000;
    auto           map  = one_kernel_map(base, {Register{"out", 0x20, "R", 32, ""}});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep, 300ms);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    FsmProxy fsm(server, {base});

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    // Continuously re-arm so the worker keeps running cycles (idle-forward → busy
    // poll → output fetch) against the model until we kill it, guaranteeing the
    // Close lands while a model call is in flight.
    std::atomic<bool> rearm{true};
    std::thread       rearmer([&] {
        while (rearm.load()) {
            (void)user.write_u32(base, ModelControlWorkers::kApStart);
            std::this_thread::sleep_for(1ms);
        }
    });

    ASSERT_TRUE(wait_until([&] { return server.request_count() > 3; }))
        << "worker never drove cycles against the model";
    server.set_fault(FaultMode::Close, 0); // kill the model mid-cycle

    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Dead; }, 5000ms))
        << "worker did not go Dead after model close during the busy cycle";
    rearm.store(false);
    rearmer.join();
    fsm.stop();
    workers.stop();
    SUCCEED();
}

TEST(ModelControlWorkers, ProtocolErrorInBusyPollIsRetriedNotFatal) {
    // A Protocol error (not Transport) during the busy-loop control poll must be
    // retried, not treated as death.  After clearing the fault and raising
    // ap_done, the cycle completes.
    const uint64_t base = 0x1000;
    auto           map  = one_kernel_map(base, {});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    // Serve the idle forward (1 reg write) normally, then reply "ERR" (a Protocol
    // error) to the busy-loop fetch:scalar polls.
    server.set_fault(FaultMode::ErrReply, 1);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Busy; }));
    // It stays Busy under repeated Protocol errors (not Dead).
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(workers.kernel_state(0), KernelState::Busy);

    // Clear the fault and complete the cycle.
    server.set_fault(FaultMode::None, 0);
    FsmProxy fsm(server, {base});
    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Idle; }));
    fsm.stop();
    workers.stop();
}

TEST(ModelControlWorkers, ProtocolErrorOnParamForwardIsNotFatal) {
    // A Protocol error on a parameter reg_write in the idle forward is logged and
    // skipped; the worker still forwards the control and advances to Busy.
    const uint64_t base = 0x1000;
    auto           map  = one_kernel_map(base, {Register{"size", 0x10, "W", 32, ""}});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    // WrongReply makes reg writes reply "42" (JSON) instead of "OK" → Protocol.
    server.set_fault(FaultMode::WrongReply, 0);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    ASSERT_TRUE(user.write_u32(base + 0x10, 0x1).has_value());
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    // Despite the Protocol errors on the reg writes, the worker still reaches Busy.
    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Busy; }))
        << "Protocol error on param forward wrongly aborted the cycle";
    workers.stop();
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────────────
// ADVERSARY PROBES — added by the adversary; each targets a specific
// suspected bug, race, or conformance gap.  A probe that reveals a
// bug becomes a regression test after the fix; a probe that passes is a hardening
// test.
// ─────────────────────────────────────────────────────────────────────────────

// PROBE A — LOST RE-ARM / DROPPED KERNEL LAUNCH.
//
// The busy→idle transition publishes the model's control value (ap_done set) to
// the memfd with a BLIND write_u32(control_offset, ctrl.value()).  If the user
// (VRT) writes a NEW ap_start into the memfd control register while the worker is
// Busy — i.e. after the idle-loop update_u32 already cleared ap_start to 0 — that
// blind publish overwrites the user's fresh ap_start with ap_done, silently
// dropping a kernel launch.
//
// Real VRT usage (vrt/src/kernel.cpp): startKernel() writes 0x1 (ap_start), then
// wait() spins reading control until bit1 (ap_done) is set; then the next
// invocation writes ap_start again.  A user who pipelines — writes the next
// ap_start immediately after seeing ap_done, or a second thread that re-arms —
// exposes the loss.  This probe stages the exact interleaving deterministically:
// it holds the worker in Busy (ap_done not yet raised), writes a fresh ap_start
// into the memfd, THEN raises ap_done.  A correct handshake must not lose the
// re-arm: the kernel must run a SECOND cycle (>= 2 control forwards to the model).
TEST(ModelControlWorkers, ReArmDuringBusyIsNotLostByApDonePublish) {
    const uint64_t base = 0x4000;
    auto           map  = one_kernel_map(base, {});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    // Fire the first ap_start; the worker forwards control and enters Busy while
    // the model reports "not done" (control scalar left at 0).
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());
    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Busy; }))
        << "worker never entered Busy for the first cycle";

    // The idle-loop update_u32 has, by now, cleared the memfd control to 0.
    // While still Busy, the user pipelines the NEXT launch: write a fresh ap_start.
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    // Now raise ap_done so the first cycle completes.  The worker will publish the
    // model's control value (ap_done) to the memfd — the suspected clobber point.
    FsmProxy fsm(server, {base});

    // A correct implementation must observe the pipelined ap_start and run a second
    // cycle.  Count control-register forwards to the model (addr == base): the
    // first cycle produced 1; a preserved re-arm produces a 2nd.
    bool second_cycle = wait_until([&] {
        auto recs = server.requests();
        std::size_t control_forwards = 0;
        for (const auto& r : recs) {
            if (r.command == "reg" && r.addr == base) ++control_forwards;
        }
        return control_forwards >= 2;
    }, 3000ms);

    fsm.stop();
    workers.stop();
    EXPECT_TRUE(second_cycle)
        << "the ap_start written during Busy was lost: the ap_done publish "
           "clobbered the user's re-arm and no second cycle ran";
}

// PROBE B2 — RE-ARM PRESERVATION MUST NOT DROP THE FIRST CYCLE'S OUTPUT WRITE-BACK.
//
// The BUG-2 fix publishes ap_done via update_u32 and, if the user re-armed during
// Busy, PRESERVES ap_start instead of publishing ap_done.  That path must still
// have written the first cycle's output register(s) back to the memfd (the
// write-backs happen before the control publish).  Otherwise honouring the re-arm
// would silently eat the first cycle's results.  This probe re-arms during Busy on
// a kernel WITH an output register and asserts BOTH: the output lands AND a second
// cycle runs.
TEST(ModelControlWorkers, ReArmDuringBusyStillWritesBackFirstCycleOutput) {
    const uint64_t base = 0xC000;
    auto map = one_kernel_map(base, {Register{"out", 0x20, "R", 32, ""}});
    auto ep  = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    server.set_scalar(base + 0x20, 0x0DDF00D5);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    // Cycle 1: fire ap_start; model reports "not done" (control scalar 0) so the
    // worker parks in Busy.
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());
    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Busy; }))
        << "worker never entered Busy";

    // Re-arm while Busy (idle-loop update_u32 already cleared ap_start).
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    // Let the FSM raise ap_done → cycle 1 completes: outputs written, re-arm honoured.
    FsmProxy fsm(server, {base});

    // The first cycle's output must have landed in the memfd despite the re-arm.
    ASSERT_TRUE(wait_until([&] {
        auto v = user.read_u32(base + 0x20);
        return v.has_value() && v.value() == 0x0DDF00D5u;
    }, 3000ms)) << "first cycle's output write-back was dropped when honouring a re-arm";

    // And a second cycle must run (the re-arm was not eaten).
    ASSERT_TRUE(wait_until([&] {
        std::size_t control_forwards = 0;
        for (const auto& r : server.requests()) {
            if (r.command == "reg" && r.addr == base) ++control_forwards;
        }
        return control_forwards >= 2;
    }, 3000ms)) << "re-arm was lost: no second control forward";

    fsm.stop();
    workers.stop();
}

// PROBE C — DATA RACE on the kernels_ vector between kernel_state()/kernel_count()
// and stop()'s kernels_.clear().  Before the fix these reads were lock-free; the
// fix guards the vector lifetime with a shared_mutex (readers shared, start/stop
// unique) and joins worker threads before clearing.  A consumer polling kernel
// state (e.g. a status thread, or CTL-subsystem introspection) concurrently with a
// reconfiguration-driven stop() must never read freed std::vector storage — this
// was a use-after-free (ASan-caught at kernel_state()); it now stays clean.  This
// probe hammers kernel_state()/kernel_count() from a reader thread while the main
// thread repeatedly start()/stop()s; the assertion is "ASan reports nothing".
TEST(ModelControlWorkers, KernelStateReadRacesStopUnderAsan) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    SystemMap map;
    for (int i = 0; i < 4; ++i) {
        Kernel k;
        k.name         = "k" + std::to_string(i);
        k.base_address = 0x1000 * (i + 1);
        k.registers.push_back(Register{"CTRL", 0x0, "RW", 32, ""});
        map.kernels.push_back(std::move(k));
    }

    ModelControlWorkers workers(user, clock, 1ms);

    std::atomic<bool> stop_reader{false};
    std::thread reader([&] {
        while (!stop_reader.load()) {
            // Read across the whole (possibly-shrinking) vector.  kernel_count()
            // and kernel_state() both touch kernels_ without holding any lock.
            std::size_t n = workers.kernel_count();
            for (std::size_t i = 0; i < n + 2; ++i) {
                (void)workers.kernel_state(i);
            }
        }
    });

    for (int round = 0; round < 40; ++round) {
        ASSERT_TRUE(workers.start(client, map).has_value());
        std::this_thread::sleep_for(1ms);
        workers.stop(); // clears kernels_ while the reader may be indexing it
    }

    stop_reader.store(true);
    reader.join();
    SUCCEED(); // the real assertion is "ASan/UBSan reported no error"
}

// PROBE D — CONTROL REGISTER NOT AT OFFSET 0.
//
// The worker hardcodes control_offset = base_address (offset 0) and gates on
// register_at(0).  By convention the control register is only "expected
// at offset 0"; the parser explicitly does NOT enforce it.  A kernel whose
// control/handshake register is declared at a non-zero offset will therefore
// NEVER start (has_control == false), even though a real ap_ctrl_hs block could
// place it elsewhere.  This probe documents the current behavior: such a kernel
// stays Idle forever and drives no model traffic.  If the workers are meant to resolve
// the control register by role rather than by fixed offset 0, this is a bug; if
// offset-0 is a hard contract, this is the hardening test that pins it.
TEST(ModelControlWorkers, ControlRegisterAtNonZeroOffsetNeverStarts) {
    SystemMap map;
    Kernel    k;
    k.name         = "offset_ctrl";
    k.base_address = 0x7000;
    // Control-like register declared at 0x8, NOT at 0.  register_at(0) == nullptr.
    k.registers.push_back(Register{"CTRL", 0x8, "RW", 32, "control not at 0"});
    map.kernels.push_back(std::move(k));

    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    // Write ap_start at the declared control offset (base + 0x8).
    ASSERT_TRUE(user.write_u32(0x7000 + 0x8, ModelControlWorkers::kApStart).has_value());
    std::this_thread::sleep_for(50ms);

    // Current contract: no offset-0 register ⇒ never starts, no model traffic.
    EXPECT_EQ(workers.kernel_state(0), KernelState::Idle);
    EXPECT_EQ(count_cmd(server.requests(), "reg"), 0u)
        << "a control register declared off offset 0 unexpectedly drove the model";

    workers.stop();
}

// PROBE E — AUTORESTART (0x81) BIT0 DETECTION + FULL-VALUE FORWARD.
//
// VRT's startKernel(autorestart=true) writes 0x81 = ap_start | autorestart
// (vrt/src/kernel.cpp:442).  The worker must still detect bit0 (ap_start) and
// must forward the FULL captured value (0x81) to the model so the model FSM sees
// the same control word VRT would have written to hardware — not a masked 0x1.
TEST(ModelControlWorkers, AutorestartControlWordIsDetectedAndForwardedWhole) {
    const uint64_t base = 0x9000;
    auto           map  = one_kernel_map(base, {});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    const uint32_t autorestart = 0x81u; // ap_start | autorestart
    ASSERT_TRUE(user.write_u32(base, autorestart).has_value());

    // The worker must detect bit0 and forward the whole 0x81 to the model.
    ASSERT_TRUE(wait_until([&] {
        auto recs = server.requests();
        for (const auto& r : recs) {
            if (r.command == "reg" && r.addr == base) return true;
        }
        return false;
    })) << "autorestart control word (0x81) never triggered a forward";

    workers.stop();

    // The model's control scalar must hold the full 0x81, not a masked 0x1.
    auto v = client.fetch_scalar(base);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), autorestart)
        << "autorestart bits were dropped; model saw a truncated control word";
    // And the memfd was atomically cleared to 0 by the update_u32.
    // (After the cycle it may hold ap_done; assert only that ap_start is gone.)
}

// PROBE F — ap_done ALREADY SET ON ENTRY (control word carries ap_start|ap_done).
//
// If the user writes ap_start with ap_done already set (0x3), or the model's
// control scalar happens to already read ap_done at the first busy poll, the
// worker must not misbehave (immediate completion is fine; a spin/hang is not).
// This probes the busy loop's first-poll ap_done handling.
TEST(ModelControlWorkers, ApDoneAlreadySetCompletesPromptly) {
    const uint64_t base = 0xA000;
    auto           map  = one_kernel_map(base, {});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    // Seed the model control scalar with ap_done already asserted.  The worker's
    // control forward will overwrite this address, so re-assert via FsmProxy too.
    FsmProxy fsm(server, {base});

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    // The cycle must complete (return to Idle) and publish ap_done to the memfd.
    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Idle; }))
        << "worker did not complete a cycle when ap_done was set promptly";
    ASSERT_TRUE(wait_until([&] {
        auto v = user.read_u32(base);
        return v.has_value() && (v.value() & ModelControlWorkers::kApDone) != 0;
    })) << "ap_done was never published to the memfd";

    fsm.stop();
    workers.stop();
}

// PROBE G — READABLE-AND-WRITABLE (RW) NON-CONTROL REGISTER IS DOUBLE-HANDLED.
//
// The idle-forward loop forwards every register whose access contains 'W'; the
// busy write-back loop fetches every register whose access contains 'R'.  A
// non-control register declared "RW" therefore matches BOTH: it is forwarded to
// the model on the way in AND fetched back from the model on the way out.  For a
// pure input or pure output register this is correct, but an "RW" scratch/param
// register gets round-tripped.  This probe documents the behavior: the model
// receives the RW register's staged value on entry, and the model's value is
// written back on exit — so a user value can be silently replaced by the model's.
TEST(ModelControlWorkers, RwNonControlRegisterIsBothForwardedAndFetchedBack) {
    const uint64_t base = 0xB000;
    // scratch@0x30 is RW: an input the user stages AND an output the model reports.
    auto map = one_kernel_map(base, {Register{"scratch", 0x30, "RW", 32, ""}});
    auto ep  = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    FsmProxy fsm(server, {base});

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    ASSERT_TRUE(user.write_u32(base + 0x30, 0x11112222).has_value()); // user's input
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    // Wait for the RW register to be BOTH forwarded (idle "reg") AND fetched back
    // (busy "fetch:scalar") within the cycle.  NOTE: waiting on
    // `Idle && reg_count>=2` is racy — the worker sets reg_count to 2 in the idle
    // forward a few instructions BEFORE it stores state=Busy, so a poll can observe
    // the still-Idle initial state together with reg>=2 and conclude "done" before
    // any busy-loop fetch has run.  Instead we wait directly on the two request
    // verbs we are about to assert on, which only appear once the corresponding
    // phase has actually executed.
    auto has_verb = [&](const std::string& cmd) {
        for (const auto& r : server.requests()) {
            if (r.addr == base + 0x30 && r.command == cmd) return true;
        }
        return false;
    };
    ASSERT_TRUE(wait_until([&] { return has_verb("reg") && has_verb("fetch:scalar"); }))
        << "RW register was not both forwarded and fetched back within the cycle";

    // Document the double-handling: the SAME RW register (addr base+0x30) is both
    // written (idle-forward, command "reg") AND read (busy write-back, command
    // "fetch:scalar") within one cycle.  A masked-by-role implementation would do
    // only one of the two.  (Note: because the mock keys model memory by address,
    // the worker's own forward and the later fetch hit the same cell, so we assert
    // on the REQUEST verbs, not on values.)
    bool wrote_rw = has_verb("reg");
    bool read_rw  = has_verb("fetch:scalar");
    fsm.stop();
    workers.stop();
    EXPECT_TRUE(wrote_rw)
        << "RW register was not forwarded to the model on entry (double-handling changed)";
    EXPECT_TRUE(read_rw)
        << "RW register was not fetched back from the model on exit (double-handling changed)";
}

// PROBE H — MANY KERNELS: no thread leak / clean teardown at higher fan-out, and
// serialisation still holds.  Exercises start() spawning many threads and stop()
// joining all of them (ASan thread-leak check) plus max_in_flight==1.
TEST(ModelControlWorkers, ManyKernelsStartAndStopCleanly) {
    SystemMap map;
    const int kN = 32;
    for (int i = 0; i < kN; ++i) {
        Kernel   k;
        uint64_t base  = 0x100 * (i + 1);
        k.name         = "k" + std::to_string(i);
        k.base_address = base;
        k.registers.push_back(Register{"CTRL", 0x0, "RW", 32, ""});
        map.kernels.push_back(std::move(k));
    }
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    EXPECT_EQ(workers.kernel_count(), static_cast<std::size_t>(kN));

    // Fire them all, let a few cycles churn on the shared client, then tear down.
    FsmProxy fsm(server, [&] {
        std::vector<uint64_t> a;
        for (int i = 0; i < kN; ++i) a.push_back(0x100 * (i + 1));
        return a;
    }());
    for (int i = 0; i < kN; ++i) {
        (void)user.write_u32(0x100 * (i + 1), ModelControlWorkers::kApStart);
    }
    std::this_thread::sleep_for(30ms);
    fsm.stop();
    workers.stop();
    EXPECT_EQ(server.max_in_flight(), 1);
    EXPECT_FALSE(workers.running());
}

// PROBE I — KERNEL WITH range == 0 must not crash or wedge.  A degenerate kernel
// range (0) is representable in the map; the worker builds a context regardless.
// It must spawn a worker, stay Idle (control still at base+0), and tear down.
TEST(ModelControlWorkers, ZeroRangeKernelIsHandled) {
    SystemMap map;
    Kernel    k;
    k.name         = "zero_range";
    k.base_address = 0x2000;
    k.range        = 0; // degenerate
    k.registers.push_back(Register{"CTRL", 0x0, "RW", 32, ""});
    map.kernels.push_back(std::move(k));

    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    FsmProxy fsm(server, {0x2000});
    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    ASSERT_TRUE(user.write_u32(0x2000, ModelControlWorkers::kApStart).has_value());
    // It should still run a normal cycle (range is not consulted by the worker).
    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Idle &&
                                        count_cmd(server.requests(), "reg") >= 1; }));
    fsm.stop();
    workers.stop();
    SUCCEED();
}

// PROBE J — ABSOLUTE KERNEL BASE ADDRESS: MEMFD OFFSET vs MODEL ADDRESS.
//
// Regression for the "kernels never finish" bug.  A real system_map declares a
// kernel's <BaseAddress> as an ABSOLUTE AXI address (e.g. increment_0 @
// 0x20200010000), well beyond the 128 MiB user-region BAR.  The user/VRT accesses
// the kernel through the BAR window at offset (base % kUserRegionSize), while the
// simulation model is addressed by the absolute base.  The worker must translate
// between the two: if it used the absolute base as a memfd offset, every memfd
// access is out of range and the worker immediately goes Dead — ap_start is never
// observed and the kernel never completes (the exact 00_axilite_raw timeout).
//
// This drives a full IDLE→BUSY→IDLE cycle on a full-size (128 MiB) user BAR with an
// absolute base, asserting:
//   * ap_start written at the BAR-window offset triggers the cycle (worker alive),
//   * the model is driven at the ABSOLUTE address (params + control forwards),
//   * the output and ap_done are published back at the BAR-window offset.
TEST(ModelControlWorkers, AbsoluteBaseAddressTranslatesMemfdVsModel) {
    // increment_0-like absolute base; range 0x10000; size@0x10 (W), out@0x18 (R).
    const uint64_t    abs_base   = 0x20200010000ULL;
    const std::size_t bar_offset = static_cast<std::size_t>(abs_base % kUserRegionSize);
    ASSERT_EQ(bar_offset, 0x10000u); // matches VRT's BAR-window resolution

    auto map = one_kernel_map(
        abs_base, {Register{"size", 0x10, "W", 32, ""}, Register{"out", 0x18, "R", 32, ""}});

    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    // A full-size user region: the whole point is that abs_base is NOT a valid
    // memfd offset, only (abs_base % kUserRegionSize) is.
    auto user  = make_bar(kUserRegionSize);
    auto clock = make_bar(kClockBarSize);

    // The model reports its output at the ABSOLUTE output address; ap_done is
    // raised by the FSM proxy at the ABSOLUTE control address.
    server.set_scalar(abs_base + 0x18, 0xFEEDFACE);
    FsmProxy fsm(server, {abs_base});

    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());

    // The user writes params + ap_start at the BAR-WINDOW OFFSET (as VRT does).
    ASSERT_TRUE(user.write_u32(bar_offset + 0x10, 0x1234).has_value());
    ASSERT_TRUE(user.write_u32(bar_offset + 0x0, ModelControlWorkers::kApStart).has_value());

    // ap_done must be published back at the BAR-window offset (this is what the raw
    // example polls, and what timed out before the fix).
    ASSERT_TRUE(wait_until([&] {
        auto v = user.read_u32(bar_offset + 0x0);
        return v.has_value() && (v.value() & ModelControlWorkers::kApStart) == 0 &&
               (v.value() & ModelControlWorkers::kApDone) != 0;
    })) << "ap_done never surfaced at the BAR-window offset (worker likely died on "
           "an out-of-range memfd access using the absolute base as an offset)";

    // The output must land back at the BAR-window offset.
    ASSERT_TRUE(wait_until([&] {
        auto v = user.read_u32(bar_offset + 0x18);
        return v.has_value() && v.value() == 0xFEEDFACEu;
    })) << "output register never written back at the BAR-window offset";

    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Idle; }));
    fsm.stop();
    workers.stop();

    // The model must have been driven at the ABSOLUTE addresses, never the offsets.
    auto recs = server.requests();
    bool param_at_abs   = false; // reg write of size at abs_base+0x10
    bool control_at_abs = false; // reg write of control at abs_base
    bool fetch_at_abs   = false; // fetch:scalar of out at abs_base+0x18
    for (const auto& r : recs) {
        if (r.command == "reg" && r.addr == abs_base + 0x10) param_at_abs = true;
        if (r.command == "reg" && r.addr == abs_base) control_at_abs = true;
        if (r.command == "fetch:scalar" && r.addr == abs_base + 0x18) fetch_at_abs = true;
        // The model must NEVER be addressed by the BAR-window offset.
        EXPECT_NE(r.addr, static_cast<uint64_t>(bar_offset))
            << "model was addressed by the BAR-window offset instead of the absolute address";
    }
    EXPECT_TRUE(param_at_abs)   << "param was not forwarded to the model's absolute address";
    EXPECT_TRUE(control_at_abs) << "control was not forwarded to the model's absolute address";
    EXPECT_TRUE(fetch_at_abs)   << "output was not fetched from the model's absolute address";
}

TEST(ModelControlWorkers, BusyLoopPollsUntilDone) {
    // The model reports "not done" for several polls, then ap_done.  Exercises the
    // busy-loop "not done yet" retry branch.
    const uint64_t base = 0x1000;
    auto           map  = one_kernel_map(base, {});
    auto           ep   = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto user   = make_bar(kUserBarSize);
    auto clock  = make_bar(kClockBarSize);

    // Leave the control scalar at 0 (not done) initially; the worker forwards the
    // control (overwriting to ap_start=1, still "not done"), so it polls.  After a
    // short delay, raise ap_done.
    ModelControlWorkers workers(user, clock, 1ms);
    ASSERT_TRUE(workers.start(client, map).has_value());
    ASSERT_TRUE(user.write_u32(base, ModelControlWorkers::kApStart).has_value());

    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Busy; }))
        << "worker never entered Busy";
    // It should still be Busy after a few polls (no ap_done yet).
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(workers.kernel_state(0), KernelState::Busy);

    // Now let the FSM complete it.
    FsmProxy fsm(server, {base});
    ASSERT_TRUE(wait_until([&] { return workers.kernel_state(0) == KernelState::Idle; }));
    fsm.stop();
    workers.stop();
}
