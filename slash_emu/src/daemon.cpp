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

#include "daemon.h"

#include "hotplug_subsystem.h"
#include "vbin_store.h"

#include <csignal>
#include <cstdio>
#include <atomic>
#include <filesystem>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace slash_emu {

namespace {

// Shared state for the shutdown rendezvous — a SELF-PIPE.
//
// A signal handler may only call async-signal-safe functions; write(2) is one of
// them, whereas std::condition_variable::notify_all() (pthread_cond_broadcast) is
// NOT formally async-signal-safe.  So the handler writes a single byte to the
// write end of a pipe; the main daemon thread blocks in read(2) on the read end
// and wakes when the byte arrives.  This is the classic self-pipe trick and the
// event-loop mechanism the architecture assigns to this step.
//
// `requested` (atomic) gates the write so a storm of signals writes at most one
// byte (the pipe can never fill), and lets request_shutdown()/the handler be
// idempotent.  The read end is O_NONBLOCK so a spurious wake never blocks the
// drain loop.  All fds are O_CLOEXEC.
struct ShutdownState {
    std::atomic<bool> requested{false};
    std::atomic<int>  read_fd{-1};
    std::atomic<int>  write_fd{-1};
};

// Single global instance.  A static local is fine because run_daemon() is not
// designed to be re-entrant (one daemon per process at a time).
ShutdownState& shutdown_state() noexcept {
    static ShutdownState state;
    return state;
}

// Wake the daemon: set the flag once and write one byte to the self-pipe.
// async-signal-safe (atomic ops + write()); safe from both a signal handler and
// ordinary thread context.
void notify_shutdown() noexcept {
    ShutdownState& s = shutdown_state();
    // Only the first caller writes, so the pipe holds at most one pending byte.
    bool expected = false;
    if (!s.requested.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel)) {
        return;
    }
    int wfd = s.write_fd.load(std::memory_order_acquire);
    if (wfd >= 0) {
        const char byte = 1;
        ssize_t n;
        do {
            n = ::write(wfd, &byte, 1);
        } while (n < 0 && errno == EINTR);
        (void)n; // a full/closed pipe is harmless: the flag is already set
    }
}

void signal_handler(int /*signum*/) noexcept { notify_shutdown(); }

// The default signal installer: delegates directly to ::sigaction.
int default_install_signal(int signum, const struct sigaction *act, struct sigaction *oldact) {
    return ::sigaction(signum, act, oldact);
}

// (Re)create the self-pipe and clear the flag so run_daemon() may be called more
// than once in one process (e.g. back-to-back unit tests).  Returns false if the
// pipe could not be created.
bool reset_shutdown_state() {
    ShutdownState& s = shutdown_state();
    // Close any stale pipe from a previous run.
    int old_r = s.read_fd.exchange(-1, std::memory_order_acq_rel);
    int old_w = s.write_fd.exchange(-1, std::memory_order_acq_rel);
    if (old_r >= 0) ::close(old_r);
    if (old_w >= 0) ::close(old_w);

    int fds[2];
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        return false;
    }
    s.read_fd.store(fds[0], std::memory_order_release);
    s.write_fd.store(fds[1], std::memory_order_release);
    // Deliberately do NOT clear `requested` here.  A shutdown request that lands
    // BEFORE this reset (via the public request_shutdown(), documented callable at
    // any time) would otherwise be silently discarded — the flag cleared and the
    // pipe recreated empty — leaving wait_for_shutdown() blocked forever.  The flag
    // means "requested and not yet consumed by a COMPLETED run"; it is cleared at
    // run end in close_shutdown_state(), so a fresh run still starts clean while a
    // genuine early request survives (wait_for_shutdown() checks the flag first).
    return true;
}

// Close the self-pipe when the daemon returns.
void close_shutdown_state() noexcept {
    ShutdownState& s = shutdown_state();
    int r = s.read_fd.exchange(-1, std::memory_order_acq_rel);
    int w = s.write_fd.exchange(-1, std::memory_order_acq_rel);
    if (r >= 0) ::close(r);
    if (w >= 0) ::close(w);
    // Consume the request at run END (not at reset/start): a completed run has
    // honored its shutdown, so the next run starts with a clear flag.  Pairing the
    // clear with close (rather than reset) is what lets an early pre-reset request
    // survive into the next run instead of being lost (see reset_shutdown_state()).
    s.requested.store(false, std::memory_order_release);
}

// Install SIGINT/SIGTERM handlers via the injectable seam.  Returns true on
// success, false if any installation failed.
bool install_shutdown_signals(const SignalInstaller& install_signal) {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    return install_signal(SIGINT, &sa, nullptr) == 0 &&
           install_signal(SIGTERM, &sa, nullptr) == 0;
}

// Block until a shutdown is requested (directly or via a signal): read from the
// self-pipe's read end, retrying on EINTR.  A prior request (flag already set,
// byte already drained) returns immediately.
void wait_for_shutdown() {
    ShutdownState& s = shutdown_state();
    int rfd = s.read_fd.load(std::memory_order_acquire);
    if (rfd < 0) {
        return; // no pipe (setup failed): do not block
    }
    for (;;) {
        if (s.requested.load(std::memory_order_acquire)) {
            return;
        }
        char buf[64];
        ssize_t n = ::read(rfd, buf, sizeof(buf));
        if (n > 0) {
            return; // woken by a self-pipe byte
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            // EINTR: retry.  EAGAIN: a spurious non-blocking wake with no data;
            // block properly via poll() before retrying so we do not busy-spin.
            if (errno == EAGAIN) {
                struct pollfd pfd{rfd, POLLIN, 0};
                (void)::poll(&pfd, 1, -1);
            }
            continue;
        }
        // n == 0 (write end closed) or a hard error: stop waiting.
        return;
    }
}

} // namespace

void request_shutdown() noexcept { notify_shutdown(); }

int run_daemon(SignalInstaller install_signal) {
    if (!install_signal) {
        install_signal = default_install_signal;
    }

    std::printf("[slash_emu] daemon starting\n");
    if (!reset_shutdown_state()) {
        std::perror("[slash_emu] pipe2");
        return 1;
    }

    if (!install_shutdown_signals(install_signal)) {
        std::perror("[slash_emu] sigaction");
        close_shutdown_state();
        return 1;
    }

    std::printf("[slash_emu] running — send SIGINT or SIGTERM to stop\n");

    wait_for_shutdown();

    std::printf("[slash_emu] daemon shutting down cleanly\n");
    close_shutdown_state();
    return 0;
}

namespace {

// Remove any leftover per-BDF VBIN directories (cold-reboot emulation).  Called
// on both startup and shutdown so VBIN files never persist across daemon runs.
void cold_reboot_cleanup_all(const DaemonConfig& config) {
    for (const auto& accel : config.accelerators) {
        VbinStore store(config.base_dir, accel.board_bdf());
        (void)store.cold_reboot_cleanup(); // best-effort
    }
}

} // namespace

int run_daemon(const DaemonConfig& config, SignalInstaller install_signal) {
    if (!install_signal) {
        install_signal = default_install_signal;
    }

    std::printf("[slash_emu] daemon starting\n");
    if (!reset_shutdown_state()) {
        std::perror("[slash_emu] pipe2");
        return 1;
    }

    // 1. Cold-reboot cleanup + ensure the base directory exists.
    cold_reboot_cleanup_all(config);
    {
        std::error_code ec;
        std::filesystem::create_directories(config.base_dir, ec);
    }

    // 2. Bring up the daemon-level hotplug subsystem (socket + lifecycle worker).
    HotplugSubsystem hotplug(config);
    if (auto r = hotplug.setup(); !r) {
        std::fprintf(stderr, "[slash_emu] hotplug setup failed: %s\n",
                     r.error().message.c_str());
        cold_reboot_cleanup_all(config);
        close_shutdown_state();
        return 1;
    }

    // 3. Install signal handlers, then trigger the startup RESCAN.
    if (!install_shutdown_signals(install_signal)) {
        std::perror("[slash_emu] sigaction");
        hotplug.remove();
        cold_reboot_cleanup_all(config);
        close_shutdown_state();
        return 1;
    }

    (void)hotplug.op_rescan(); // instantiate every configured accelerator
    std::printf("[slash_emu] running — send SIGINT or SIGTERM to stop\n");

    // 4. Block until a shutdown is requested (self-pipe read).
    wait_for_shutdown();

    // 5. Tear down: stop the socket, drain the lifecycle queue, tear down every
    //    accelerator, then cold-reboot-clean the VBIN directories.
    std::printf("[slash_emu] daemon shutting down cleanly\n");
    hotplug.remove();
    cold_reboot_cleanup_all(config);
    close_shutdown_state();
    return 0;
}

} // namespace slash_emu
