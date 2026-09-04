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

#include "model_process.h"

#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <utility>
#include <vector>

#include <spawn.h>
#include <signal.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace slash_sysemu {

namespace {

VbinError io_error(const std::string& what) {
    return VbinError{VbinErrorKind::Io, what};
}

using LaunchErr = VbinResult<std::unique_ptr<ModelProcess>>;

LaunchErr launch_err(const std::string& what) {
    return LaunchErr::err(io_error(what));
}

// Reaping coordination shared between teardown() and the monitor thread.  The
// monitor is the SOLE caller of waitpid; teardown waits on `reaped` rather than
// racing the monitor for the child's exit status.
struct ReapState {
    std::mutex              mu;
    std::condition_variable cv;
    bool                    reaped = false;
};

} // namespace

// A small out-of-class state bag so the header need not include <condition_variable>.
// One instance per ModelProcess, owned via unique_ptr.
struct ModelProcessReap {
    ReapState state;
};

// ─────────────────────────────────────────────────────────────────────────────
// Launch
// ─────────────────────────────────────────────────────────────────────────────

LaunchErr ModelProcess::launch(const std::string&          vbin_path,
                               DeathCallback               on_death,
                               const ModelProcessTimeouts& timeouts) {
    // 1. Unpack the VBIN (Archive/Contents/Parse errors propagate unchanged).
    VbinResult<Vbin> unpacked = unpack_vbin(vbin_path);
    if (!unpacked) {
        return LaunchErr::err(unpacked.error());
    }

    auto self = std::unique_ptr<ModelProcess>(new ModelProcess());
    self->vbin_     = std::move(unpacked.value());
    self->on_death_ = std::move(on_death);
    self->timeouts_ = timeouts;

    const std::filesystem::path& extract_dir = self->vbin_.temp_dir.path();
    const std::filesystem::path& exec_path   = self->vbin_.executable;

    // The VBIN extractor writes members with default (non-executable) mode; the
    // launcher is responsible for making the model binary runnable.  Add the
    // execute bits (respecting the existing read bits) before spawning.
    {
        std::error_code ec;
        auto perms = std::filesystem::status(exec_path, ec).permissions();
        if (!ec) {
            std::filesystem::permissions(
                exec_path,
                perms | std::filesystem::perms::owner_exec |
                    std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                std::filesystem::perm_options::replace, ec);
        }
        if (ec) {
            return launch_err("cannot make model executable '" + exec_path.string() +
                              "' runnable: " + ec.message());
        }
    }

    // The model binds ipc://<extraction_dir>/zmq.socket; we connect to it.
    const std::string socket_path = (extract_dir / "zmq.socket").string();
    self->endpoint_ = "ipc://" + socket_path;

    // Guard against the AF_UNIX sun_path limit.  An ipc:// endpoint becomes an
    // AF_UNIX socket path bound into a fixed-size sockaddr_un::sun_path (108 on
    // Linux, incl. the NUL terminator).  Under a very long TMPDIR the extraction
    // path could meet or exceed that; libzmq would then fail (or silently
    // truncate) at bind/connect time with an opaque error, or the model's bind
    // would fail and we would only find out via the probe timeout.  Fail early
    // with a clear, specific Io diagnostic instead.
    if (socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        return launch_err("endpoint path too long for AF_UNIX (" +
                          std::to_string(socket_path.size()) + " >= " +
                          std::to_string(sizeof(sockaddr_un{}.sun_path)) + "): '" +
                          socket_path + "'");
    }

    // argv: run the executable by its basename with cwd = extraction dir, so a
    // relative "./vpp_sim" resolves inside the extracted tree.
    std::string exec_name = "./" + exec_path.filename().string();
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exec_name.c_str()));
    argv.push_back(const_cast<char*>(self->endpoint_.c_str()));
    argv.push_back(nullptr);

    // 2. posix_spawn with cwd = extraction dir and default signal dispositions.
    posix_spawn_file_actions_t file_actions;
    if (posix_spawn_file_actions_init(&file_actions) != 0) {
        return launch_err("posix_spawn_file_actions_init failed");
    }
    // chdir into the extraction dir before exec (glibc extension).
    if (posix_spawn_file_actions_addchdir_np(&file_actions, extract_dir.c_str()) != 0) {
        posix_spawn_file_actions_destroy(&file_actions);
        return launch_err("posix_spawn_file_actions_addchdir_np failed");
    }

    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&file_actions);
        return launch_err("posix_spawnattr_init failed");
    }
    // Reset all signal dispositions to default in the child so it does not
    // inherit the daemon's handlers/masks.
    sigset_t all;
    sigfillset(&all);
    posix_spawnattr_setsigdefault(&attr, &all);
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

    pid_t child = -1;
    int spawn_rc = posix_spawn(&child, exec_path.c_str(), &file_actions, &attr, argv.data(),
                               environ);
    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attr);

    if (spawn_rc != 0) {
        return launch_err("posix_spawn('" + exec_path.string() +
                          "') failed: " + std::strerror(spawn_rc));
    }

    self->pid_ = child;
    self->running_.store(true);
    self->reap_ = std::make_unique<ModelProcessReap>();

    // 3. Start the monitor thread (sole reaper) before we probe, so that a
    //    process that dies during the probe is still reaped.
    //    The raw `self` capture is safe: teardown() always joins (or, in the
    //    on-monitor-thread re-entrant case, detaches) the monitor before *self is
    //    destroyed — ~ModelProcess calls teardown() first — so the monitor never
    //    dereferences a destroyed object.
    self->monitor_ = std::thread([p = self.get()] { p->monitor_loop(); });

    // 4. Connect the client and probe with the one-time global `start`.
    Result<ModelClient> connected = ModelClient::connect(self->endpoint_, timeouts.request);
    if (!connected) {
        std::string msg = connected.error().message;
        self->teardown();
        return launch_err("model client connect failed: " + msg);
    }
    self->client_ = std::move(connected.value());

    Result<void> probe = self->client_.start();
    if (!probe) {
        std::string msg = probe.error().message;
        self->teardown();
        return launch_err("model launch probe (start) failed: " + msg);
    }

    return LaunchErr::ok(std::move(self));
}

// ─────────────────────────────────────────────────────────────────────────────
// Monitor / death detection
// ─────────────────────────────────────────────────────────────────────────────

void ModelProcess::monitor_loop() {
    int   status = 0;
    pid_t r;
    do {
        r = ::waitpid(pid_, &status, 0);
    } while (r < 0 && errno == EINTR);

    // Child reaped: publish the reaped state and wake any teardown() waiter.
    running_.store(false);
    {
        std::lock_guard<std::mutex> g(reap_->state.mu);
        reap_->state.reaped = true;
    }
    reap_->state.cv.notify_all();

    // Fire the death callback only for an UNEXPECTED death.
    if (!intentional_teardown_.load() && on_death_) {
        on_death_();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Teardown
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Wait up to @p timeout for the child to be reaped (by the monitor thread).
// Returns true if reaped within the deadline.
bool wait_reaped(ReapState& st, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(st.mu);
    return st.cv.wait_for(lk, timeout, [&] { return st.reaped; });
}

} // namespace

void ModelProcess::teardown() noexcept {
    std::call_once(teardown_once_, [this] {
        // Mark BEFORE any signal so the monitor suppresses the death callback.
        intentional_teardown_.store(true);

        if (pid_ > 0) {
            // Best-effort graceful `exit` verb (ignore its result).
            (void)client_.exit();

            if (!wait_reaped(reap_->state, timeouts_.exit_wait)) {
                // Escalate: SIGTERM, then SIGKILL.
                ::kill(pid_, SIGTERM);
                if (!wait_reaped(reap_->state, timeouts_.term_wait)) {
                    ::kill(pid_, SIGKILL);
                    // Block until the monitor reaps the (now killed) child.
                    std::unique_lock<std::mutex> lk(reap_->state.mu);
                    reap_->state.cv.wait(lk, [this] { return reap_->state.reaped; });
                }
            }
        }

        // Re-entrancy / self-join guard: teardown() may be entered ON the monitor
        // thread if a caller synchronously tears down from within on_death (which
        // the contract forbids, but we must not silently deadlock).  A thread
        // cannot join itself, so detach instead — the monitor is already unwinding
        // (the child was reaped before on_death ran, so the escalation above was a
        // no-op) and will finish on its own.  Otherwise, join normally.
        if (monitor_.joinable()) {
            if (std::this_thread::get_id() == monitor_.get_id()) {
                monitor_.detach();
            } else {
                monitor_.join();
            }
        }
    });
}

ModelProcess::~ModelProcess() {
    teardown();
}

} // namespace slash_sysemu
