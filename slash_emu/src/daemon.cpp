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
#include "log.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>

#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>

namespace slash_emu {

namespace {

// ── Programmatic shutdown rendezvous (eventfd) ───────────────────────────────
//
// request_shutdown() is thread-safe and may be called from any thread (tests, or
// a future control path).  It writes to an eventfd that the sd-event loop watches;
// the io handler then calls sd_event_exit().  `pending` latches a request so one
// that lands before run_daemon() has created the eventfd is not lost: run_daemon()
// checks it immediately after registering the fd and self-triggers.
//
// Real SIGTERM/SIGINT delivery drives the SAME exit via sd_event_add_signal(); the
// eventfd path is only the programmatic equivalent.
struct ShutdownState {
    std::atomic<int>  event_fd{-1};
    std::atomic<bool> pending{false};
};

ShutdownState& shutdown_state() noexcept {
    static ShutdownState state;
    return state;
}

void notify_shutdown() noexcept {
    ShutdownState& s = shutdown_state();
    s.pending.store(true, std::memory_order_release);
    int fd = s.event_fd.load(std::memory_order_acquire);
    if (fd >= 0) {
        uint64_t one = 1;
        ssize_t  n;
        do {
            n = ::write(fd, &one, sizeof(one));
        } while (n < 0 && errno == EINTR);
        (void)n; // EAGAIN on a saturated counter is harmless: a wake is pending
    }
}

// ── sd-event handlers ────────────────────────────────────────────────────────

// The eventfd (request_shutdown) fired: exit the loop.
int on_stop_event(sd_event_source* s, int /*fd*/, uint32_t /*revents*/, void* /*ud*/) {
    return sd_event_exit(sd_event_source_get_event(s), 0);
}

// SIGTERM/SIGINT: exit the loop.
int on_signal(sd_event_source* s, const struct signalfd_siginfo* /*si*/, void* /*ud*/) {
    return sd_event_exit(sd_event_source_get_event(s), 0);
}

// Health-gated watchdog keepalive.  Fired every WATCHDOG_USEC/2; pings only while
// the lifecycle path is making progress, then re-arms itself.
struct WatchdogCtx {
    HotplugSubsystem* hotplug{nullptr};
    uint64_t          interval_usec{0};
};

int on_watchdog(sd_event_source* s, uint64_t /*usec*/, void* ud) {
    auto* ctx = static_cast<WatchdogCtx*>(ud);
    if (ctx->hotplug->healthy()) {
        sd_notify(0, "WATCHDOG=1");
    }
    // Re-arm relative to the current monotonic time.
    uint64_t now = 0;
    if (sd_event_now(sd_event_source_get_event(s), CLOCK_MONOTONIC, &now) >= 0) {
        (void)sd_event_source_set_time(s, now + ctx->interval_usec);
        (void)sd_event_source_set_enabled(s, SD_EVENT_ONESHOT);
    }
    return 0;
}

} // namespace

void request_shutdown() noexcept { notify_shutdown(); }

int run_daemon(const DaemonConfig& config) {
    log_info("daemon starting");

    // 1a. Block SIGTERM/SIGINT process-wide BEFORE any subsystem thread spawns, so
    //     sd-event's signalfd is the sole consumer in every thread.  Save the old
    //     mask to restore on return (keeps run_daemon() re-invocable in tests).
    sigset_t block{};
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigset_t oldmask{};
    ::pthread_sigmask(SIG_BLOCK, &block, &oldmask);

    // Restore the signal mask on every return path.
    struct MaskGuard {
        const sigset_t& old;
        ~MaskGuard() { ::pthread_sigmask(SIG_SETMASK, &old, nullptr); }
    } mask_guard{oldmask};

    // 1b. Create the sd-event loop + its sources.
    sd_event* event = nullptr;
    if (int r = sd_event_default(&event); r < 0) {
        log_err("sd_event_default failed: %s", std::strerror(-r));
        return 1;
    }
    struct EventGuard {
        sd_event* e;
        ~EventGuard() { sd_event_unref(e); }
    } event_guard{event};

    // Signal sources (floating: owned by the loop).
    if (sd_event_add_signal(event, nullptr, SIGTERM, on_signal, nullptr) < 0 ||
        sd_event_add_signal(event, nullptr, SIGINT,  on_signal, nullptr) < 0) {
        log_err("sd_event_add_signal failed");
        return 1;
    }

    // eventfd for request_shutdown().  O_NONBLOCK so notify_shutdown() never blocks.
    int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0) {
        log_err("eventfd failed: %s", std::strerror(errno));
        return 1;
    }
    ShutdownState& sd_state = shutdown_state();
    sd_state.event_fd.store(efd, std::memory_order_release);
    struct EfdGuard {
        ShutdownState& s;
        int            fd;
        ~EfdGuard() {
            s.event_fd.store(-1, std::memory_order_release);
            ::close(fd);
            // A completed run has honored any request; clear the latch so the next
            // run starts clean.  Pairing the clear with teardown (not startup) is
            // what lets a request that lands just before startup survive.
            s.pending.store(false, std::memory_order_release);
        }
    } efd_guard{sd_state, efd};

    if (sd_event_add_io(event, nullptr, efd, EPOLLIN, on_stop_event, nullptr) < 0) {
        log_err("sd_event_add_io failed");
        return 1;
    }
    // Honor a shutdown request that arrived before the eventfd existed.
    if (sd_state.pending.load(std::memory_order_acquire)) {
        notify_shutdown();
    }

    // 2. Bring up the daemon-level hotplug subsystem (socket + lifecycle worker).
    HotplugSubsystem hotplug(config);
    if (auto r = hotplug.setup(); !r) {
        log_err("hotplug setup failed: %s", r.error().message.c_str());
        return 1;
    }

    // 3. Startup RESCAN, then announce readiness.
    (void)hotplug.op_rescan();
    log_info("running (%zu accelerator(s))", hotplug.accelerator_count());
    sd_notify(0, "READY=1\nSTATUS=running");

    // 4. Arm the health-gated watchdog keepalive if systemd enabled it.
    WatchdogCtx wd_ctx{};
    uint64_t    wd_usec = 0;
    if (sd_watchdog_enabled(0, &wd_usec) > 0 && wd_usec > 0) {
        wd_ctx.hotplug       = &hotplug;
        wd_ctx.interval_usec = wd_usec / 2; // ping at half the deadline
        uint64_t now = 0;
        if (sd_event_now(event, CLOCK_MONOTONIC, &now) >= 0) {
            (void)sd_event_add_time(event, nullptr, CLOCK_MONOTONIC,
                                    now + wd_ctx.interval_usec,
                                    /*accuracy=*/wd_ctx.interval_usec / 10,
                                    on_watchdog, &wd_ctx);
        }
    }

    // 5. Run until SIGTERM/SIGINT or request_shutdown().
    if (int r = sd_event_loop(event); r < 0) {
        log_err("sd_event_loop failed: %s", std::strerror(-r));
        // Fall through to an orderly teardown regardless.
    }

    // 6. Tear down: notify STOPPING, stop the socket, drain the lifecycle queue,
    //    and tear down every accelerator.  systemd wipes RuntimeDirectory (sockets
    //    + VBIN staging dirs) on stop, so there is no cleanup to do here.
    sd_notify(0, "STOPPING=1");
    log_info("daemon shutting down cleanly");
    hotplug.remove();
    return 0;
}

} // namespace slash_emu
