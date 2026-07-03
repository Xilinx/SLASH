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

#pragma once

#include "model_client.h"
#include "system_map.h"
#include "vbin.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <sys/types.h> // pid_t

namespace slash_emu {

// Opaque reaping-coordination state (condition_variable etc.), defined in the
// .cpp so this header need not pull in <condition_variable>.
struct ModelProcessReap;

// ─────────────────────────────────────────────────────────────────────────────
// ModelProcess — RAII owner of one launched vpp_sim model process
// ─────────────────────────────────────────────────────────────────────────────
//
// A ModelProcess unpacks a selected VBIN, launches its vpp_sim executable, and
// connects a ModelClient to it.  It owns:
//   * the unpacked Vbin (its TempDir keeps the extracted tree — including the
//     executable and the ipc socket — alive for the process lifetime);
//   * the child pid (guaranteed reaped on destruction);
//   * a live ModelClient (used by the model control workers in Step 8);
//   * a monitor thread that detects unexpected process death.
//
// Launch convention (ground truth from vrt/src/device.cpp):
//   endpoint = "ipc://<extraction_dir>/zmq.socket"; the model BINDS it, we
//   CONNECT.  The executable is spawned with cwd = extraction dir and argv =
//   { "./vpp_sim", endpoint }, via posix_spawn (async-signal-safe in a
//   multithreaded daemon).
//
// Launch-success criterion: the process spawned AND ModelClient connected AND a
// probe `start()` returned OK within the timeout.  The probe `start` IS the
// one-time global sim-clock start — it is issued exactly once here, so the
// WorkerController must not re-send it.  On any failure the child is SIGKILLed
// and reaped and launch() returns an error.
//
// Death detection: a monitor thread blocks on waitpid(pid).  When the child
// exits it fires the death callback EXACTLY ONCE, unless an intentional-teardown
// flag was set first (normal exit()/SIGTERM/SIGKILL teardown), so "accelerator
// lost" never fires on an orderly shutdown.

/**
 * @brief Callback invoked once when the model process dies UNEXPECTEDLY.
 *
 * CONTRACT — read carefully:
 *   * on_death runs ON THE MONITOR THREAD (the thread that waitpid()'d the
 *     child).  Keep it short and non-blocking.
 *   * A caller MUST NOT synchronously tear down THIS ModelProcess from within
 *     on_death.  teardown() joins the monitor thread; a synchronous teardown
 *     from the callback would be the monitor thread trying to join itself.
 *     Step 11's on_death (which reacts to model death by tearing the accelerator
 *     down) MUST dispatch that teardown to a DIFFERENT thread (e.g. post it to a
 *     lifecycle work queue), never call it inline.
 *   * A caller MUST ALSO NOT synchronously DESTROY THIS ModelProcess from within
 *     on_death.  ~ModelProcess destroys the on_death_ std::function while its
 *     operator() is still executing on the monitor stack — a heap-use-after-free
 *     of the callable's own captures.  The detach guard below makes
 *     teardown-from-callback safe, but it CANNOT make delete-this-from-callback
 *     safe: the object (and the running callable) would be freed underneath the
 *     monitor thread.  So Step 11 must also post the DESTROY to another thread.
 *   * Required pattern (both cases): to react to model death, hand off to another
 *     thread — post a teardown/destroy task — and return from on_death promptly.
 *     Never tear down OR destroy this ModelProcess synchronously inside on_death.
 *   * As a safety net, teardown() detects being called on the monitor thread and
 *     does NOT self-join (it detaches instead), turning a would-be deadlock into
 *     safe behavior — but callers must still not rely on that; dispatch off-thread.
 */
using DeathCallback = std::function<void()>;

/** Tunable timeouts for launch and teardown; small values keep tests fast. */
struct ModelProcessTimeouts {
    std::chrono::milliseconds request{kDefaultModelTimeout}; /**< ModelClient per-request. */
    std::chrono::milliseconds exit_wait{2000};   /**< Wait after `exit` before SIGTERM. */
    std::chrono::milliseconds term_wait{2000};   /**< Wait after SIGTERM before SIGKILL. */
};

class ModelProcess {
public:
    /**
     * @brief Unpack @p vbin_path, launch its vpp_sim, and connect a client.
     *
     * @param vbin_path   Path to the VBIN to launch (main or staging).
     * @param on_death    Fired once if the process dies unexpectedly (may be null).
     * @param timeouts    Launch/teardown timeouts.
     * @return A ready ModelProcess (unique_ptr) on success, or:
     *   - VbinError (Archive/Contents/Parse) if the VBIN cannot be unpacked, or
     *   - VbinErrorKind::Io wrapping a launch failure (spawn/connect/probe).
     *   On any launch failure no child process is left running.
     */
    static VbinResult<std::unique_ptr<ModelProcess>> launch(
        const std::string&          vbin_path,
        DeathCallback               on_death = {},
        const ModelProcessTimeouts& timeouts = {});

    ModelProcess(const ModelProcess&)            = delete;
    ModelProcess& operator=(const ModelProcess&) = delete;

    /** Destruction performs an orderly teardown (see teardown()). */
    ~ModelProcess();

    /** The live model client (valid until teardown). */
    [[nodiscard]] ModelClient& client() noexcept { return client_; }

    /** The parsed system map of the launched VBIN. */
    [[nodiscard]] const SystemMap& system_map() const noexcept { return vbin_.map; }

    /** The child process id. */
    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    /** The ipc:// endpoint the model bound / the client connected to. */
    [[nodiscard]] const std::string& endpoint() const noexcept { return endpoint_; }

    /** True until teardown() has fully reaped the child. */
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    /**
     * @brief Orderly teardown: mark intentional, best-effort `exit`, escalate to
     *        SIGTERM then SIGKILL with bounded waits, join the monitor, reap.
     *
     * Idempotent.  Because the teardown is flagged intentional BEFORE the child
     * is asked to exit, the death callback does NOT fire.  Called by the
     * destructor; may also be called explicitly.
     */
    void teardown() noexcept;

private:
    ModelProcess() = default;

    // Monitor-thread body: waitpid(pid_) then fire the death callback unless the
    // teardown was intentional.
    void monitor_loop();

    Vbin        vbin_;
    std::string endpoint_;
    ModelClient client_;
    pid_t       pid_{-1};

    std::thread       monitor_;
    DeathCallback     on_death_;
    ModelProcessTimeouts timeouts_;

    std::atomic<bool> running_{false};             // child not yet reaped
    std::atomic<bool> intentional_teardown_{false}; // suppress death callback
    std::once_flag    teardown_once_;

    // Reaping coordination (mutex/condvar) between teardown() and monitor_loop().
    std::unique_ptr<ModelProcessReap> reap_;
};

} // namespace slash_emu
