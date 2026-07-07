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

#include "accelerator.h"
#include "config.h"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace slash_emu {

// ─────────────────────────────────────────────────────────────────────────────
// HotplugSubsystem — the daemon-level slash_hotplug socket + lifecycle orchestrator
// ─────────────────────────────────────────────────────────────────────────────
//
// Serves the single daemon-level `slash_hotplug` AF_UNIX/SOCK_SEQPACKET socket
// (emulated `/dev/slash_hotplug`) and orchestrates the lifecycle of every
// emulated accelerator (architecture: "Life cycle operations (Hotplugging)").
//
// Operations (all under ONE daemon-wide lifecycle lock — only one in flight):
//   * RESCAN      (0x30, no arg): reload the config, (re)instantiate configs that
//     don't conflict with a (partially) active accelerator, restore removed PFs of
//     partial accelerators using their ORIGINAL config (PF2 restore reconfigures).
//   * REMOVE      (0x31, bdf):    remove the targeted PF; the model + workers
//     follow once the last PF (incl. PF0) is gone.
//   * HOTPLUG     (0x33, bdf):    REMOVE(target) then RESCAN, as one lock op.
//   * TOGGLE_SBR  (0x32, bdf):    REMOVE all PFs of all accelerators on the target
//     bus, RESCAN, then sleep the (injectable) link-training delay.
//
// The BDF targeting rule (see resolve_target):
//   * ".2" → PF2, ".1" → PF1, ".0" → PF0; a board BDF with no suffix → board-level
//     (all PFs); empty BDF → the only tracked device (or -EOPNOTSUPP/-ENODEV).
//
// Threading:
//   * One listener thread + one worker per connection (mirrors CtlSubsystem);
//     workers are keyed by a monotonic id to dodge fd-reuse collisions.
//   * One lifecycle thread drains a work queue.  The socket handlers and the
//     model-death poster both go through this single serialisation point.  A
//     model-process death callback (fired on a ModelProcess monitor thread) only
//     ENQUEUES a teardown task — it never tears down inline (model_process.h
//     DeathCallback contract).  The teardown runs on the lifecycle thread so the
//     monitor-thread join inside ModelInstance::teardown() is never a self-join.
//
// Shutdown drains the queue and tears down every accelerator without deadlock:
// the socket is stopped first (no new ops), then the lifecycle thread is joined
// (WITHOUT holding the lifecycle lock), then every accelerator is torn down.

class HotplugSubsystem {
public:
    /** Tunables (the 1s TOGGLE_SBR link-training delay is injectable for tests). */
    struct Options {
        std::chrono::milliseconds sbr_delay{1000};
        /** Model launch/teardown timeouts applied to every accelerator (tests
         *  shorten these so a dead-model teardown does not wait the full 10s). */
        ModelProcessTimeouts      model_timeouts{};
    };

    /**
     * @brief Construct an (inactive) hotplug subsystem for @p cfg.
     *
     * No side effects until setup().  @p cfg carries the base dir / ownership /
     * mode / config-file path / accelerator list.  RESCAN re-reads @p
     * cfg.config_file each time.
     */
    explicit HotplugSubsystem(DaemonConfig cfg);
    HotplugSubsystem(DaemonConfig cfg, Options opts);

    HotplugSubsystem(const HotplugSubsystem&)            = delete;
    HotplugSubsystem& operator=(const HotplugSubsystem&) = delete;
    HotplugSubsystem(HotplugSubsystem&&)                 = delete;
    HotplugSubsystem& operator=(HotplugSubsystem&&)      = delete;

    ~HotplugSubsystem();

    /**
     * @brief Start the lifecycle thread and open the slash_hotplug socket.
     *
     * Does NOT perform the initial RESCAN — the daemon drives that after setup()
     * (so cold-reboot cleanup can run first).  Idempotent.
     */
    Result<void> setup();

    /**
     * @brief Stop the socket, drain + stop the lifecycle thread, tear down all
     *        accelerators.  Idempotent.  Called by the destructor.
     */
    void remove();

    // ── Programmatic lifecycle ops (also reachable over the socket) ───────────
    // Each takes the lifecycle lock exactly once.  Return 0 on success or -errno.
    int op_rescan();
    int op_remove(const std::string& bdf);
    int op_hotplug(const std::string& bdf);
    int op_toggle_sbr(const std::string& bdf);

    // ── Introspection (for tests / the daemon) ───────────────────────────────
    [[nodiscard]] bool               is_active() const noexcept { return active_.load(); }
    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }
    /** State of the accelerator with board BDF @p board_bdf (nullopt if unknown). */
    [[nodiscard]] std::optional<AccelState> state_of(const std::string& board_bdf) const;
    /** The accelerator for @p board_bdf, or nullptr (tests only; not thread-safe). */
    [[nodiscard]] Accelerator* accelerator(const std::string& board_bdf) const;
    /** Number of tracked accelerators (for tests). */
    [[nodiscard]] std::size_t accelerator_count() const;

private:
    // ── Lifecycle work queue ──────────────────────────────────────────────────
    void lifecycle_thread_main();
    void post_lifecycle(std::function<void()> task);

    // ── Socket listener / workers (mirrors CtlSubsystem) ─────────────────────
    void listener_loop();
    void connection_loop(int conn_fd);
    void reap_finished();
    Result<void> dispatch(int fd, ReceivedMessage& msg);

    // ── Ops, assuming the lifecycle lock is held ──────────────────────────────
    int rescan_locked();
    int remove_locked(const std::string& bdf);
    int hotplug_locked(const std::string& bdf);
    int toggle_sbr_locked(const std::string& bdf, std::unique_lock<std::mutex>& lk);

    // Remove all present PFs of one accelerator (board-level REMOVE).
    void remove_all_pfs(Accelerator& acc);

    // ── BDF targeting ─────────────────────────────────────────────────────────
    struct Target {
        std::string        board_bdf;        // canonical "DDDD:BB:DD"
        std::optional<Pf>  pf;               // nullopt → board-level (all PFs)
    };
    // Resolve a raw request bdf to a target.  Returns 0 or -errno; caller holds
    // the lifecycle lock (reads the registry).
    int resolve_target(const std::string& raw, Target& out) const;

    // Extract the PCI bus token ("BB") from a canonical board BDF "DDDD:BB:DD".
    static std::string bus_of(const std::string& board_bdf);

    DaemonConfig socket_cfg_; // for base_dir/uid/gid/mode + config_file (RESCAN reloads)
    Options      opts_;
    std::string  socket_path_;

    // The one daemon-wide lifecycle mutex.
    mutable std::mutex lifecycle_mu_;
    // Registry keyed by canonical board BDF.  Guarded by lifecycle_mu_.
    std::map<std::string, std::unique_ptr<Accelerator>> accels_;

    // Lifecycle work queue + its single worker thread.
    std::mutex                        q_mu_;
    std::condition_variable           q_cv_;
    std::deque<std::function<void()>> q_;
    bool                              q_stop_ = false;
    std::thread                       lifecycle_thread_;

    // Interruptible sleep for the TOGGLE_SBR delay (woken on shutdown).
    std::condition_variable sbr_cv_;

    // Socket runtime state.
    std::atomic<bool> active_{false};
    std::atomic<bool> stop_{false};
    UniqueFd          listen_fd_;
    std::thread       listener_;
    struct Conn {
        std::thread       thread;
        int               fd{-1};
        std::atomic<bool> done{false};
    };
    mutable std::mutex                                   conns_mtx_;
    std::unordered_map<uint64_t, std::unique_ptr<Conn>> conns_;
    uint64_t                                            next_conn_key_ = 0;
};

} // namespace slash_emu
