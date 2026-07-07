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
#include "transport.h"
#include "vbin_store.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>

namespace slash_emu {

// ─────────────────────────────────────────────────────────────────────────────
// QdmaSubsystem — the PF1 QDMA subsystem (slash_qdma_ctl<N>)
// ─────────────────────────────────────────────────────────────────────────────
//
// Exposes a named AF_UNIX/SOCK_SEQPACKET socket (the emulated equivalent of the
// `/dev/slash_qdma_ctl<N>` control device) and services the PF1 QDMA ioctls as
// request/response datagrams (architecture: "QDMA subsystem (`slash_qdma_ctl<N>`)").
//
// Two kinds of endpoint (see the CTL/XFER table in the architecture):
//   * CTL  — the top-level slash_qdma_ctl<N> socket.  Accepts:
//       INFO(0x50), QPAIR_ADD(0x51), Q_OP(0x52), QPAIR_GET_FD(0x53), BUF_CREATE(0x54).
//   * XFER — a per-transfer-session anonymous socketpair returned by QPAIR_GET_FD.
//       Accepts: BUF_CREATE(0x54), TRANSFER(0x56).
//   An op arriving on the wrong endpoint is rejected with -EINVAL; an unknown op
//   with -ENOSYS.  Workers survive both.
//
// Threading model (mirrors CtlSubsystem):
//   * One listener thread runs accept() in a loop, spawning one CTL connection
//     worker per accepted connection.
//   * QPAIR_GET_FD creates an anonymous socketpair, hands one end to the user via
//     SCM_RIGHTS, and starts a dedicated *transfer-session* worker on the other
//     end.  A transfer session owns a set of qpair ids (moved to the Used state)
//     and services BUF_CREATE + TRANSFER until the user closes their end (recv
//     returns EOF), at which point the owned qpairs transition back to Started.
//
// Qpair state machine (architecture "Mechanisms"):
//   Initial   -[QPAIR_ADD]->        Stopped
//   Stopped|Started -[START]->      Started
//   Started   -[GET_FD]->          Used
//   Used      -[last close on FD]-> Started
//   Started|Stopped -[STOP]->      Stopped
//   Started|Stopped -[DEL]->       removed from the list
//   Invalid transitions return -EINVAL.  A transfer referencing a qpair that is
//   Stopped or removed fails with -ENODEV (models the driver NOT invalidating a
//   session when a qpair stops).
//
// Transfer mechanism (per sub-transfer):
//   * H2C to HBM/DDR:   pread from the referenced fd → ModelClient::populate.
//   * C2H from HBM/DDR: ModelClient::fetch_buffer → pwrite to the referenced fd.
//   * H2C to the reconfiguration aperture (dev_addr == kReconfigApertureAddr):
//       pread from the fd → VbinStore::append_staging (NOT sent to the model).
//   The user first sends the source/target fds as SCM_RIGHTS ancillary data;
//   xfers[i].buf_fd is an INDEX into that list (resolve_fd_index).  The response
//   return_value is the total bytes transferred; transferred fds are then closed.
//
// Model serialisation:
//   ModelClient's internal per-socket mutex guarantees only one model request is
//   ever in flight, satisfying the architecture's "only one transfer session ever
//   has an open request with the model process".  File I/O (pread/pwrite on the
//   user buffer) is deliberately done OUTSIDE that lock — each chunk is
//   pread-then-populate / fetch-then-pwrite — so two sessions' file I/O can
//   overlap while their model requests serialise.  No extra transfer mutex is
//   needed; leaning on ModelClient's lock keeps the critical section minimal.
//
// Ownership / borrowing:
//   The ModelClient and VbinStore are BORROWED (owned by the accelerator / Steps
//   6/11).  REMOVE forgets the qpair list and drops all CTL connections and
//   transfer sessions but does NOT touch the model or any buffers.  Host buffers
//   are memfds handed to the user and CLOSED by the daemon immediately after
//   responding (so they release on the user's last ref); the daemon keeps no
//   reference to them.
//
// Idempotency: setup() on an active subsystem is a no-op-success; remove() on an
// inactive subsystem is a no-op.  setup→remove→setup cycles are safe.  The
// destructor calls remove() if still active.  Nothing throws across the API.

// The reconfiguration-aperture device address.  H2C writes here are appended to
// the staging VBIN instead of being forwarded to the model process (architecture
// "Writing the staging VBIN": chunks of up to 64 KiB, always at device address
// 0x102100000).  Step 13 moves this constant into the kernel ABI header along
// with the HBM/DDR/reconfiguration memory ranges; until then it lives here.
inline constexpr uint64_t kReconfigApertureAddr = 0x102100000ull;

class QdmaSubsystem {
public:
    /**
     * @brief Construct an (inactive) PF1 QDMA subsystem.
     *
     * @param socket_path Absolute path of the slash_qdma_ctl<N> socket to create.
     * @param board_bdf   Board BDF ("DDDD:BB:DD"); INFO returns board_bdf + ".1".
     * @param model       Borrowed model client; must outlive this object.  Used
     *                    for TRANSFER H2C/C2H to HBM/DDR.
     * @param vbin        Borrowed VBIN store; must outlive this object.  Used for
     *                    TRANSFER H2C to the reconfiguration aperture.
     *
     * Socket ownership/permissions are systemd's responsibility (User=/Group= +
     * UMask=): the socket inherits the daemon's identity on bind() and the unit's
     * umask sets its mode atomically, so the subsystem performs no chown/chmod.
     */
    QdmaSubsystem(std::string socket_path, std::string board_bdf,
                  ModelClient& model, VbinStore& vbin);

    QdmaSubsystem(const QdmaSubsystem&)            = delete;
    QdmaSubsystem& operator=(const QdmaSubsystem&) = delete;
    QdmaSubsystem(QdmaSubsystem&&)                 = delete;
    QdmaSubsystem& operator=(QdmaSubsystem&&)      = delete;

    ~QdmaSubsystem();

    /**
     * @brief Create the socket, listener thread, and worker pool (RESCAN half).
     *
     * (Re)initialises the qpair list, creates the SEQPACKET socket, unlinks any
     * stale file, binds/listens, and starts the listener thread (ownership/mode
     * come from systemd).  Idempotent: a no-op-success if already active.
     *
     * @return ok() on success, or ErrorKind::Transport on any OS failure (the
     *         subsystem is left inactive with no leftover socket file/threads).
     */
    Result<void> setup();

    /**
     * @brief Tear down to inactive: stop accepting, unlink, force-disconnect all.
     *
     * Stops the listener, unlinks the socket file, forcibly disconnects every
     * CTL connection and transfer session (their peers see send/recv failures),
     * joins all threads, and forgets the qpair list.  The model and buffers are
     * left untouched (borrowed).  Idempotent.
     */
    void remove();

    // ── Introspection (for tests / orchestration) ───────────────────────────

    /** True while the socket + threads are up. */
    [[nodiscard]] bool is_active() const noexcept { return active_.load(); }

    /** The configured socket path. */
    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

    /** Number of currently open CTL connections (approximate; for tests). */
    [[nodiscard]] std::size_t connection_count() const;

    /** Number of currently live transfer sessions (approximate; for tests). */
    [[nodiscard]] std::size_t session_count() const;

    /** Number of qpairs currently tracked (for tests). */
    [[nodiscard]] std::size_t qpair_count() const;

private:
    // ── Qpair state machine ──────────────────────────────────────────────────
    enum class QState { Stopped, Started, Used };
    struct Qpair {
        uint32_t id;
        uint32_t dir_mask; // enabled directions (bit0 H2C, bit1 C2H)
        QState   state;
    };

    // ── CTL / XFER endpoint dispatch ─────────────────────────────────────────
    // A connection is either the CTL control endpoint or an XFER transfer session
    // endpoint; the op table differs (see class comment).
    enum class Endpoint { Ctl, Xfer };

    // Listener loop: accept() CTL connections until stop; spawn a worker each.
    void listener_loop();
    // Per-CTL-connection worker: recv → dispatch → send, until peer/subsystem close.
    void connection_loop(int conn_fd);
    // Per-transfer-session worker: services an XFER endpoint over @p sock_fd and
    // owns @p qids (already moved to Used).  On EOF/teardown it returns the qids
    // to Started and flags the session done.
    void session_loop(int sock_fd, uint64_t session_key, std::vector<uint32_t> qids);

    // Dispatch one received datagram against @p endpoint on connection @p fd.
    // Returns whether the response was sent (false → drop the connection).
    Result<void> dispatch(int fd, Endpoint endpoint, ReceivedMessage& msg,
                          const std::vector<uint32_t>* session_qids);

    // Individual ioctl handlers.  Each sends exactly one response datagram.
    Result<void> handle_info(int fd, ReceivedMessage& msg);
    Result<void> handle_qpair_add(int fd, ReceivedMessage& msg);
    Result<void> handle_q_op(int fd, ReceivedMessage& msg);
    Result<void> handle_qpair_get_fd(int fd, ReceivedMessage& msg);
    Result<void> handle_buf_create(int fd, ReceivedMessage& msg);
    Result<void> handle_transfer(int fd, ReceivedMessage& msg,
                                 const std::vector<uint32_t>& session_qids);

    // Reap CTL connections / sessions whose workers have finished on their own.
    void reap_finished();

    // ── Immutable configuration ──────────────────────────────────────────────
    std::string  socket_path_;
    std::string  device_bdf_; // board_bdf + ".1"
    ModelClient& model_;      // borrowed
    VbinStore&   vbin_;       // borrowed

    // ── Runtime state ─────────────────────────────────────────────────────────
    std::atomic<bool> active_{false};
    std::atomic<bool> stop_{false};
    UniqueFd          listen_fd_;
    std::thread       listener_;

    // Qpair table.  Guarded by qpairs_mtx_.  Keyed by qid.
    mutable std::mutex                       qpairs_mtx_;
    std::unordered_map<uint32_t, Qpair>      qpairs_;
    uint32_t                                 next_qid_{0};

    // Live workers.  A single map holds both CTL connections and transfer
    // sessions; the int key is the raw serviced fd (owned by its worker).  Keyed
    // this way for shutdown() signalling, exactly like CtlSubsystem.
    struct Worker {
        std::thread       thread;
        int               fd{-1};   // fd to shutdown() on teardown
        bool              session{false}; // true for XFER transfer sessions
        std::atomic<bool> done{false};
    };
    mutable std::mutex                                    workers_mtx_;
    std::unordered_map<uint64_t, std::unique_ptr<Worker>> workers_;
    uint64_t                                              next_worker_key_{0};
};

} // namespace slash_emu
