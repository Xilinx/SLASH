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

#include "bar_memfd.h"
#include "transport.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>

namespace slash_emu {

// ─────────────────────────────────────────────────────────────────────────────
// CtlSubsystem — the PF2 BAR / device-info subsystem (slash_ctl<N>)
// ─────────────────────────────────────────────────────────────────────────────
//
// Exposes a named AF_UNIX/SOCK_SEQPACKET socket (the emulated equivalent of the
// `/dev/slash_ctl<N>` control device) and services the three PF2 control ioctls
// as request/response datagrams (architecture: "BAR access and device
// information subsystem (`slash_ctl<N>`)"):
//
//   * SLASH_CTLDEV_IOCTL_GET_BAR_INFO   (0x30) — BAR enumeration
//   * SLASH_CTLDEV_IOCTL_GET_BAR_FD     (0x31) — hand a mappable BAR memfd back
//                                                as SCM_RIGHTS ancillary data
//   * SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO(0x32) — PCI identity of PF2
//
// Threading model:
//   * One listener thread runs accept() in a loop.
//   * Each accepted connection is serviced by its own worker thread (one thread
//     per connection).  A connection worker recv()s a request, dispatches on the
//     header's ioctl_op, and send()s the response datagram — mirroring the
//     sequence_id and ioctl_op and setting return_value to 0 on success or
//     -errno on error.  It loops until the peer closes or the subsystem tears
//     down the connection.
//   * One-thread-per-connection is chosen over a fixed pool because the expected
//     concurrency is small (a handful of libslash consumers per accelerator) and
//     each request is short and non-blocking (no model I/O on this PF).  This
//     keeps the shutdown story simple: shutdown(SHUT_RDWR) on each connection fd
//     unblocks its recv(), the worker exits, and we join it.
//
// Shutdown mechanism:
//   * remove() flips an atomic stop flag, unlinks the socket file, and
//     shutdown(SHUT_RDWR)s + closes the listening socket so the listener's
//     accept() returns and the listener thread exits.
//   * It then shutdown(SHUT_RDWR)s every live connection fd (forcing each
//     connection worker's blocked recv()/send() to fail — the "forced user
//     disconnect" the architecture requires) and joins every worker thread.
//   * No self-pipe/eventfd is needed: shutdown() on the listening and connection
//     sockets is sufficient to promptly unblock accept()/recv() on Linux.
//
// BAR ownership:
//   * The BarSet is BORROWED (const reference), owned by the accelerator
//     (Step 11).  REMOVE never touches the memfds — the model control workers
//     keep polling them.  GET_BAR_FD returns a reopen()ed DISTINCT open file
//     description so daemon-vs-user flocks genuinely collide.
//
// Idempotency: setup() on an already-active subsystem is a no-op-success;
// remove() on an inactive subsystem is a no-op.  setup→remove→setup cycles are
// safe.  The destructor calls remove() if still active.  Nothing throws across
// the API.

class CtlSubsystem {
public:
    /**
     * @brief Construct an (inactive) PF2 subsystem.
     *
     * @param socket_path Absolute path of the slash_ctl<N> socket to create.
     * @param board_bdf   Board BDF ("DDDD:BB:DD"); GET_DEVICE_INFO appends ".2".
     * @param bars        Borrowed BAR set; must outlive this object.
     *
     * Socket ownership/permissions are systemd's responsibility (User=/Group= +
     * UMask=): the socket inherits the daemon's identity on bind() and the unit's
     * umask sets its mode, so the subsystem performs no chown/chmod.
     */
    CtlSubsystem(std::string socket_path, std::string board_bdf, const BarSet& bars);

    CtlSubsystem(const CtlSubsystem&)            = delete;
    CtlSubsystem& operator=(const CtlSubsystem&) = delete;
    CtlSubsystem(CtlSubsystem&&)                 = delete;
    CtlSubsystem& operator=(CtlSubsystem&&)      = delete;

    ~CtlSubsystem();

    /**
     * @brief Create the socket, listener thread, and worker pool (RESCAN half).
     *
     * Creates the SEQPACKET socket, unlinks any stale file at the path, binds,
     * listens, and starts the listener thread (ownership/mode come from systemd).
     * Idempotent: a no-op-success if already active.
     *
     * @return ok() on success, or ErrorKind::Transport on any OS failure (the
     *         subsystem is left inactive with no leftover socket file/threads).
     */
    Result<void> setup();

    /**
     * @brief Tear down to inactive: stop accepting, unlink, force-disconnect all.
     *
     * Stops the listener, unlinks the socket file, forcibly disconnects every
     * open connection (their peers see send/recv failures), and joins all
     * threads.  The BAR memfds are left untouched (borrowed).  Idempotent.
     */
    void remove();

    // ── Introspection (for tests / orchestration) ───────────────────────────

    /** True while the socket + threads are up. */
    [[nodiscard]] bool is_active() const noexcept { return active_.load(); }

    /** The configured socket path. */
    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

    /** Number of currently open connections (approximate; for tests). */
    [[nodiscard]] std::size_t connection_count() const;

private:
    // Listener loop: accept() connections until stop; spawn a worker per accept.
    void listener_loop();
    // Per-connection worker: recv → dispatch → send, until peer/subsystem close.
    void connection_loop(int conn_fd);

    // Reap the threads of any connections that have finished on their own (peer
    // closed) so we do not accumulate joinable thread handles indefinitely.
    void reap_finished_connections();

    // ── Immutable configuration ──────────────────────────────────────────────
    std::string   socket_path_;
    std::string   device_bdf_;   // board_bdf + ".2"
    const BarSet& bars_;

    // ── Runtime state ─────────────────────────────────────────────────────────
    std::atomic<bool> active_{false};
    std::atomic<bool> stop_{false};
    UniqueFd          listen_fd_;
    std::thread       listener_;

    // Live connections.  Guarded by conns_mtx_.  The int key is the raw conn fd;
    // the fd is OWNED by its connection worker (closed when the worker exits), so
    // this map stores the raw fd only for shutdown() signalling — remove() must
    // not close it out from under the worker.
    struct Connection {
        std::thread       thread;
        std::atomic<bool> done{false}; // set by the worker just before it returns
    };
    mutable std::mutex                                     conns_mtx_;
    std::unordered_map<int, std::unique_ptr<Connection>>  conns_;
};

} // namespace slash_emu
