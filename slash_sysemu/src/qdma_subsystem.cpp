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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // memfd_create
#endif

#include "qdma_subsystem.h"

#include "qdma_ioctls.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace slash_sysemu {

namespace {

TransportError os_error(const std::string& what) {
    return TransportError{ErrorKind::Transport, what + ": " + std::strerror(errno)};
}

// Build a response header from a request header: mirror ioctl_op + sequence_id,
// set return_value.
slash_sysemu_socket_header make_response_header(const slash_sysemu_socket_header& req,
                                             int32_t return_value) {
    slash_sysemu_socket_header h{};
    h.ioctl_op     = req.ioctl_op;
    h.sequence_id  = req.sequence_id;
    h.return_value = static_cast<uint32_t>(return_value);
    h.pad          = 0;
    return h;
}

// Send a plain (no-fd) response echoing the payload bytes back to the user.
Result<void> send_plain_response(int fd, const slash_sysemu_socket_header& req,
                                 int32_t return_value, std::span<const uint8_t> payload) {
    slash_sysemu_socket_header h = make_response_header(req, return_value);
    return send_message(fd, h, payload, {});
}

// Chunk size for splitting a large sub-transfer into model requests / file reads.
// 64 KiB matches the reconfiguration-aperture write granularity and keeps a
// bounded amount buffered per chunk.
constexpr std::size_t kTransferChunk = 64 * 1024;

// Directions (struct slash_qdma_qpair_add::dir_mask / subxfer::direction bits).
constexpr uint32_t kDirH2cBit = 0x1;
constexpr uint32_t kDirC2hBit = 0x2;
constexpr uint32_t kDirCmptBit = 0x4;

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

QdmaSubsystem::QdmaSubsystem(std::string socket_path, std::string board_bdf,
                             ModelClient& model, VbinStore& vbin)
    : socket_path_(std::move(socket_path)),
      device_bdf_(board_bdf + ".1"),
      model_(model),
      vbin_(vbin) {}

QdmaSubsystem::~QdmaSubsystem() { remove(); }

// ─────────────────────────────────────────────────────────────────────────────
// setup() — (re)init qpairs, create socket, listener
// ─────────────────────────────────────────────────────────────────────────────

Result<void> QdmaSubsystem::setup() {
    if (active_.load()) {
        return Result<void>::ok(); // idempotent
    }

    // sun_path length guard: leave room for the trailing NUL.
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() + 1 > sizeof(addr.sun_path)) {
        return Result<void>::err(TransportError{
            ErrorKind::Transport,
            "slash_qdma_ctl socket path too long: '" + socket_path_ + "'"});
    }
    std::memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size());

    UniqueFd sock(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (!sock) {
        return Result<void>::err(os_error("socket(AF_UNIX, SOCK_SEQPACKET)"));
    }

    // Unlink any stale socket file first (leftover from a prior run / crash).
    ::unlink(socket_path_.c_str());

    if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return Result<void>::err(os_error("bind(" + socket_path_ + ")"));
    }

    // No chown/chmod: under systemd the socket inherits the daemon's User=/Group=
    // on bind() and the unit's UMask= fixes its mode atomically at creation.

    if (::listen(sock.get(), /*backlog=*/16) != 0) {
        auto err = os_error("listen(" + socket_path_ + ")");
        ::unlink(socket_path_.c_str());
        return Result<void>::err(std::move(err));
    }

    // (Re)initialise the qpair list (RESCAN half): a fresh subsystem tracks no
    // qpairs and restarts qid allocation.
    {
        std::lock_guard<std::mutex> lk(qpairs_mtx_);
        qpairs_.clear();
        next_qid_ = 0;
    }

    // Commit state before starting the listener so it observes a consistent view.
    listen_fd_ = std::move(sock);
    stop_.store(false);
    active_.store(true);
    listener_ = std::thread([this] { listener_loop(); });

    return Result<void>::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// remove() — teardown to inactive
// ─────────────────────────────────────────────────────────────────────────────

void QdmaSubsystem::remove() {
    if (!active_.exchange(false)) {
        return; // idempotent: already inactive
    }

    // 1. Signal stop and stop accepting: unlink the socket file and shut down the
    //    listening socket so accept() returns promptly.
    stop_.store(true);
    ::unlink(socket_path_.c_str());
    if (listen_fd_) {
        ::shutdown(listen_fd_.get(), SHUT_RDWR);
    }
    if (listener_.joinable()) {
        listener_.join();
    }
    listen_fd_.reset();

    // 2. Force-disconnect every LIVE worker (CTL connection + transfer session),
    //    then join all workers.
    //
    //    The done-check and ::shutdown() are performed UNDER workers_mtx_.
    //    Invariant (established in connection_loop / session_loop): a worker
    //    sets done=true under workers_mtx_ strictly BEFORE its UniqueFd
    //    destructor closes the fd.  Therefore, while we hold workers_mtx_:
    //      - If w->done == false  → the fd (w->fd) is guaranteed still OPEN;
    //        shutdown() is safe and will unblock the worker's recv().
    //      - If w->done == true   → the fd is already closed (or closing in a
    //        thread blocked waiting for this lock); the fd number may have been
    //        reused — skip shutdown().
    //    This closes the race where done-check and shutdown ran outside the
    //    lock: a worker could set done + close its fd between a !done read and
    //    the subsequent shutdown() call, hitting a recycled fd.
    //
    //    The map is moved out under the same lock so that any worker that races
    //    to set done after we release the lock finds an empty map (find() →
    //    end()) and silently does nothing.  Joins happen outside the lock.
    std::unordered_map<uint64_t, std::unique_ptr<Worker>> workers;
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        for (auto& [key, w] : workers_) {
            (void)key;
            if (w->fd >= 0 && !w->done.load()) {
                // fd guaranteed open (worker can't close without this lock).
                ::shutdown(w->fd, SHUT_RDWR);
            }
            // done==true: fd already closed or closing; skip.
        }
        workers = std::move(workers_);
        workers_.clear();
    }
    for (auto& [key, w] : workers) {
        (void)key;
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }

    // 3. Forget the qpair list (REMOVE).  The model and buffers are borrowed and
    //    deliberately left untouched.
    {
        std::lock_guard<std::mutex> lk(qpairs_mtx_);
        qpairs_.clear();
        next_qid_ = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// listener_loop — accept CTL connections, spawn one worker each
// ─────────────────────────────────────────────────────────────────────────────

void QdmaSubsystem::listener_loop() {
    while (!stop_.load()) {
        int conn = ::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (conn < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; // listener shut down by remove()
        }

        reap_finished();

        auto worker = std::make_unique<Worker>();
        worker->fd  = conn;
        {
            std::lock_guard<std::mutex> lk(workers_mtx_);
            if (stop_.load()) {
                ::close(conn);
                break;
            }
            uint64_t key       = next_worker_key_++;
            worker->thread     = std::thread([this, conn] { connection_loop(conn); });
            workers_.emplace(key, std::move(worker));
        }
    }
}

void QdmaSubsystem::reap_finished() {
    std::vector<std::unique_ptr<Worker>> finished;
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (it->second->done.load()) {
                finished.push_back(std::move(it->second));
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Join outside the lock.  The worker already flagged done and closed its
    // fd (via UniqueFd in the worker lambda), so the join is near-instantaneous.
    for (auto& w : finished) {
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// connection_loop — CTL endpoint: recv → dispatch → send, per connection
// ─────────────────────────────────────────────────────────────────────────────

void QdmaSubsystem::connection_loop(int conn_fd) {
    UniqueFd fd(conn_fd); // own the connection fd; closed on return

    while (!stop_.load()) {
        auto req = recv_message(fd.get());
        if (!req) {
            break; // peer closed / shutdown / protocol error
        }
        ReceivedMessage& msg = req.value();
        Result<void> sent = dispatch(fd.get(), Endpoint::Ctl, msg, /*session_qids=*/nullptr);
        if (!sent) {
            break;
        }
    }

    // Mark done BEFORE the fd closes (UniqueFd dtor below), so remove() sees
    // done=true and skips calling shutdown() on our (still-open) fd.  Once we
    // return the fd is closed; any further shutdown() on that fd number would
    // target a recycled fd — possibly a live connection — which must not be
    // disturbed.
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        for (auto& [key, w] : workers_) {
            (void)key;
            if (w->fd == fd.get()) {
                w->done.store(true);
                break;
            }
        }
    }
    // fd closes here (UniqueFd dtor).
}

// ─────────────────────────────────────────────────────────────────────────────
// session_loop — XFER endpoint: services a transfer session over an anon socket
// ─────────────────────────────────────────────────────────────────────────────

void QdmaSubsystem::session_loop(int sock_fd, uint64_t session_key,
                                 std::vector<uint32_t> qids) {
    UniqueFd fd(sock_fd); // own the daemon-side socket; closed on return

    while (!stop_.load()) {
        auto req = recv_message(fd.get());
        if (!req) {
            break; // user closed their end (EOF) / shutdown / protocol error
        }
        ReceivedMessage& msg = req.value();
        Result<void> sent = dispatch(fd.get(), Endpoint::Xfer, msg, &qids);
        if (!sent) {
            break;
        }
    }

    // Last close on the transfer fd: return the owned qpairs from Used to Started
    // (Used -[last close on FD]-> Started).  A qpair that was
    // STOPed/DELeted underneath is skipped (no longer Used / no longer present).
    {
        std::lock_guard<std::mutex> lk(qpairs_mtx_);
        for (uint32_t qid : qids) {
            auto it = qpairs_.find(qid);
            if (it != qpairs_.end() && it->second.state == QState::Used) {
                it->second.state = QState::Started;
            }
        }
    }

    // Mark done BEFORE the fd closes (UniqueFd dtor below), so remove() sees
    // done=true and skips calling shutdown() on our (still-open) fd.  Once we
    // return the fd is closed; shutdown() on that fd number then would target a
    // recycled fd — possibly a live connection — which must not be disturbed.
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        auto it = workers_.find(session_key);
        if (it != workers_.end()) {
            it->second->done.store(true);
        }
    }
    // fd closes here (UniqueFd dtor).
}

// ─────────────────────────────────────────────────────────────────────────────
// dispatch — endpoint-aware op routing
// ─────────────────────────────────────────────────────────────────────────────

Result<void> QdmaSubsystem::dispatch(int fd, Endpoint endpoint, ReceivedMessage& msg,
                                     const std::vector<uint32_t>* session_qids) {
    switch (msg.header.ioctl_op) {
    case kSlashQdmaIoctlInfo:
        if (endpoint != Endpoint::Ctl) {
            return send_plain_response(fd, msg.header, -EINVAL, {});
        }
        return handle_info(fd, msg);
    case kSlashQdmaIoctlQpairAdd:
        if (endpoint != Endpoint::Ctl) {
            return send_plain_response(fd, msg.header, -EINVAL, {});
        }
        return handle_qpair_add(fd, msg);
    case kSlashQdmaIoctlQOp:
        if (endpoint != Endpoint::Ctl) {
            return send_plain_response(fd, msg.header, -EINVAL, {});
        }
        return handle_q_op(fd, msg);
    case kSlashQdmaIoctlQpairGetFd:
        if (endpoint != Endpoint::Ctl) {
            return send_plain_response(fd, msg.header, -EINVAL, {});
        }
        return handle_qpair_get_fd(fd, msg);
    case kSlashQdmaIoctlBufCreate:
        // Accepted on BOTH endpoints.
        return handle_buf_create(fd, msg);
    case kSlashQdmaQpairIoctlTransfer:
        if (endpoint != Endpoint::Xfer || session_qids == nullptr) {
            return send_plain_response(fd, msg.header, -EINVAL, {});
        }
        return handle_transfer(fd, msg, *session_qids);
    default:
        // Unknown op → -ENOSYS; echo the received payload so the datagram is
        // well-formed.  The worker survives.
        return send_plain_response(fd, msg.header, -ENOSYS,
                                   std::span<const uint8_t>(msg.payload));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// INFO (0x50)
// ─────────────────────────────────────────────────────────────────────────────

Result<void> QdmaSubsystem::handle_info(int fd, ReceivedMessage& msg) {
    if (msg.payload.size() < sizeof(slash_qdma_info)) {
        return send_plain_response(fd, msg.header, -EINVAL, {});
    }
    slash_qdma_info info{};
    std::memcpy(&info, msg.payload.data(), sizeof(info));
    info.qsets_max   = 0;
    info.msix_qvecs  = 0;
    info.vf_max      = 0;
    info.caps        = 0;
    std::memset(info.bdf, 0, sizeof(info.bdf));
    // device_bdf_ is board_bdf + ".1"; fits in 32 chars ("DDDD:BB:DD.1").
    std::size_t n = std::min(device_bdf_.size(), sizeof(info.bdf) - 1);
    std::memcpy(info.bdf, device_bdf_.data(), n);
    info.bdf[n] = '\0';
    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&info), sizeof(info));
    return send_plain_response(fd, msg.header, 0, payload);
}

// ─────────────────────────────────────────────────────────────────────────────
// QPAIR_ADD (0x51)
// ─────────────────────────────────────────────────────────────────────────────

Result<void> QdmaSubsystem::handle_qpair_add(int fd, ReceivedMessage& msg) {
    if (msg.payload.size() < sizeof(slash_qdma_qpair_add)) {
        return send_plain_response(fd, msg.header, -EINVAL, {});
    }
    slash_qdma_qpair_add add{};
    std::memcpy(&add, msg.payload.data(), sizeof(add));

    // Streaming mode and the completion direction are unsupported (mirrors the
    // real driver and the ABI doc): -EOPNOTSUPP.
    if (add.mode != kQdmaQModeMm || (add.dir_mask & kDirCmptBit) != 0) {
        std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&add), sizeof(add));
        return send_plain_response(fd, msg.header, -EOPNOTSUPP, payload);
    }
    // At least one MM direction must be requested.
    if ((add.dir_mask & (kDirH2cBit | kDirC2hBit)) == 0) {
        std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&add), sizeof(add));
        return send_plain_response(fd, msg.header, -EINVAL, payload);
    }
    // Keyhole aperture: 0 (linear) or a power-of-two byte window (mirrors the
    // real driver's slash_qdma_ioctl_qpair_add_w validation).
    if (add.aperture_size != 0 &&
        (add.aperture_size & (add.aperture_size - 1)) != 0) {
        std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&add), sizeof(add));
        return send_plain_response(fd, msg.header, -EINVAL, payload);
    }

    uint32_t qid;
    {
        std::lock_guard<std::mutex> lk(qpairs_mtx_);
        qid = next_qid_++;
        qpairs_.emplace(qid, Qpair{qid, add.dir_mask, add.aperture_size, QState::Stopped});
    }
    add.qid = qid;
    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&add), sizeof(add));
    return send_plain_response(fd, msg.header, 0, payload);
}

// ─────────────────────────────────────────────────────────────────────────────
// Q_OP (0x52) — START / STOP / DEL
// ─────────────────────────────────────────────────────────────────────────────

Result<void> QdmaSubsystem::handle_q_op(int fd, ReceivedMessage& msg) {
    if (msg.payload.size() < sizeof(slash_qdma_qpair_op)) {
        return send_plain_response(fd, msg.header, -EINVAL, {});
    }
    slash_qdma_qpair_op op{};
    std::memcpy(&op, msg.payload.data(), sizeof(op));
    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&op), sizeof(op));

    int32_t rv = 0;
    {
        std::lock_guard<std::mutex> lk(qpairs_mtx_);
        auto it = qpairs_.find(op.qid);
        if (it == qpairs_.end()) {
            rv = -EINVAL; // no such qpair
        } else {
            Qpair& q = it->second;
            switch (op.op) {
            case SLASH_QDMA_QUEUE_OP_START:
                // Stopped|Started -[START]-> Started.  Used cannot be started.
                if (q.state == QState::Stopped || q.state == QState::Started) {
                    q.state = QState::Started;
                } else {
                    rv = -EINVAL;
                }
                break;
            case SLASH_QDMA_QUEUE_OP_STOP:
                // Started|Stopped -[STOP]-> Stopped.  STOP is only valid from
                // Started/Stopped; a Used qpair's stop models the driver marking
                // it stopped underneath a live session, so we also allow it and
                // let the transfer path observe -ENODEV.
                if (q.state == QState::Started || q.state == QState::Stopped ||
                    q.state == QState::Used) {
                    q.state = QState::Stopped;
                } else {
                    rv = -EINVAL;
                }
                break;
            case SLASH_QDMA_QUEUE_OP_DEL:
                // Started|Stopped -[DEL]-> removed.  A Used qpair may also be
                // deleted underneath a live session (driver behaviour); the
                // transfer path then observes -ENODEV (qpair gone).
                qpairs_.erase(it);
                break;
            default:
                rv = -EINVAL;
                break;
            }
        }
    }
    return send_plain_response(fd, msg.header, rv, payload);
}

// ─────────────────────────────────────────────────────────────────────────────
// QPAIR_GET_FD (0x53)
// ─────────────────────────────────────────────────────────────────────────────

Result<void> QdmaSubsystem::handle_qpair_get_fd(int fd, ReceivedMessage& msg) {
    if (msg.payload.size() < sizeof(slash_qdma_qpair_fd_request)) {
        return send_plain_response(fd, msg.header, -EINVAL, {});
    }
    slash_qdma_qpair_fd_request req{};
    std::memcpy(&req, msg.payload.data(), sizeof(req));
    std::span<const uint8_t> echo(reinterpret_cast<const uint8_t*>(&req), sizeof(req));

    // Resolve the qpair id list: qpair_count==0 uses the legacy single qid.
    std::vector<uint32_t> qids;
    if (req.qpair_count == 0) {
        qids.push_back(req.qid);
    } else {
        if (req.qpair_count > SLASH_QDMA_FD_MAX_QPAIRS) {
            return send_plain_response(fd, msg.header, -EINVAL, echo);
        }
        for (uint32_t i = 0; i < req.qpair_count; ++i) {
            qids.push_back(req.qpair_ids[i]);
        }
    }

    // Reject a list that binds the same qpair twice (mirrors the driver): binding
    // one qpair into two slots of a single session has no legitimate caller and
    // would otherwise transition the qpair to Used once but "own" it twice.  The
    // list is at most SLASH_QDMA_FD_MAX_QPAIRS long, so an O(n^2) scan is fine.
    for (std::size_t i = 0; i < qids.size(); ++i) {
        for (std::size_t j = i + 1; j < qids.size(); ++j) {
            if (qids[i] == qids[j]) {
                return send_plain_response(fd, msg.header, -EINVAL, echo);
            }
        }
    }

    // Validate + transition Started -> Used atomically.  All referenced qpairs
    // must currently be Started; otherwise nothing is moved and we fail.
    {
        std::lock_guard<std::mutex> lk(qpairs_mtx_);
        for (uint32_t qid : qids) {
            auto it = qpairs_.find(qid);
            if (it == qpairs_.end() || it->second.state != QState::Started) {
                return send_plain_response(fd, msg.header, -EINVAL, echo);
            }
        }
        for (uint32_t qid : qids) {
            qpairs_[qid].state = QState::Used;
        }
    }

    // Create the anonymous socketpair: one end serviced by a new session worker,
    // the other handed to the user via SCM_RIGHTS.
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        // Revert the Used transition on failure.
        std::lock_guard<std::mutex> lk(qpairs_mtx_);
        for (uint32_t qid : qids) {
            auto it = qpairs_.find(qid);
            if (it != qpairs_.end() && it->second.state == QState::Used) {
                it->second.state = QState::Started;
            }
        }
        return send_plain_response(fd, msg.header, -EIO, echo);
    }
    UniqueFd daemon_end(sv[0]);
    UniqueFd user_end(sv[1]);

    // Register + start the session worker.  Do this BEFORE sending the fd so a
    // racing remove() can find and tear it down.
    //
    // LOCK ORDERING: when both mutexes are needed at once they are taken
    // workers_mtx_ THEN qpairs_mtx_ (as below).  session_loop() only ever holds
    // one at a time (it releases qpairs_mtx_ before taking workers_mtx_), so no
    // cycle forms today.  Preserve this ordering — a nested acquire in the other
    // order would reintroduce an ABBA hazard (tracked in the PR's deferred items).
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        if (stop_.load()) {
            // Racing remove(): don't start a worker it won't see.  Revert Used.
            std::lock_guard<std::mutex> qlk(qpairs_mtx_);
            for (uint32_t qid : qids) {
                auto it = qpairs_.find(qid);
                if (it != qpairs_.end() && it->second.state == QState::Used) {
                    it->second.state = QState::Started;
                }
            }
            return send_plain_response(fd, msg.header, -ENODEV, echo);
        }
        uint64_t key    = next_worker_key_++;
        auto     worker = std::make_unique<Worker>();
        worker->fd      = daemon_end.get();
        worker->session = true;
        int      raw    = daemon_end.release();
        worker->thread =
            std::thread([this, raw, key, qids] { session_loop(raw, key, qids); });
        workers_.emplace(key, std::move(worker));
    }

    // Hand the user their end.  return_value 0; the fd is ancillary (index 0).
    slash_sysemu_socket_header h = make_response_header(msg.header, 0);
    int              raw_user = user_end.get();
    std::array<int, 1> fds{raw_user};
    Result<void> sent = send_message(fd, h, echo, fds);
    // user_end (UniqueFd) closes the daemon's copy on scope exit; the description
    // is fully owned by the user after the send.
    return sent;
}

// ─────────────────────────────────────────────────────────────────────────────
// BUF_CREATE (0x54)
// ─────────────────────────────────────────────────────────────────────────────

Result<void> QdmaSubsystem::handle_buf_create(int fd, ReceivedMessage& msg) {
    if (msg.payload.size() < sizeof(slash_qdma_buf_create)) {
        return send_plain_response(fd, msg.header, -EINVAL, {});
    }
    slash_qdma_buf_create req{};
    std::memcpy(&req, msg.payload.data(), sizeof(req));
    std::span<const uint8_t> echo(reinterpret_cast<const uint8_t*>(&req), sizeof(req));

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    if (req.length == 0 || (req.length % page) != 0) {
        return send_plain_response(fd, msg.header, -EINVAL, echo);
    }

    UniqueFd buf(::memfd_create("slash_qdma_buf", MFD_CLOEXEC));
    if (!buf) {
        return send_plain_response(fd, msg.header, -EIO, echo);
    }
    if (::ftruncate(buf.get(), static_cast<off_t>(req.length)) != 0) {
        return send_plain_response(fd, msg.header, -EIO, echo);
    }

    req.granule       = static_cast<uint32_t>(page);
    // Deliberate difference from the V80 driver: single-qpair hint.
    req.transfer_hint = SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR;

    slash_sysemu_socket_header h = make_response_header(msg.header, 0);
    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    int raw = buf.get();
    std::array<int, 1> fds{raw};
    Result<void> sent = send_message(fd, h, payload, fds);
    // buf (UniqueFd) closes the daemon's copy here: the daemon keeps NO reference
    // to the memfd, so it is released once the user drops their last fd/mapping.
    return sent;
}

// ─────────────────────────────────────────────────────────────────────────────
// TRANSFER (0x56) — XFER endpoint only
// ─────────────────────────────────────────────────────────────────────────────

Result<void> QdmaSubsystem::handle_transfer(int fd, ReceivedMessage& msg,
                                            const std::vector<uint32_t>& session_qids) {
    if (msg.payload.size() < sizeof(slash_qdma_transfer)) {
        return send_plain_response(fd, msg.header, -EINVAL, {});
    }
    slash_qdma_transfer xfer{};
    std::memcpy(&xfer, msg.payload.data(), sizeof(xfer));

    if (xfer.count == 0 || xfer.count > SLASH_QDMA_FD_MAX_QPAIRS) {
        return send_plain_response(fd, msg.header, -EINVAL, {});
    }

    // Per-sub-transfer keyhole aperture, captured from the bound qpair under the
    // qpairs lock below and consumed (unlocked) by the transfer loop.
    std::array<uint32_t, SLASH_QDMA_FD_MAX_QPAIRS> apertures{};

    // Precondition: every referenced qpair must currently be Started or Used
    // (i.e. present and not stopped).  A stopped/removed qpair fails the WHOLE
    // transfer with -ENODEV (models the driver not invalidating a live session).
    {
        std::lock_guard<std::mutex> lk(qpairs_mtx_);
        for (uint32_t i = 0; i < xfer.count; ++i) {
            uint32_t qidx = xfer.xfers[i].qpair_index;
            if (qidx >= session_qids.size()) {
                return send_plain_response(fd, msg.header, -EINVAL, {});
            }
            uint32_t qid = session_qids[qidx];
            auto     it  = qpairs_.find(qid);
            if (it == qpairs_.end() || it->second.state == QState::Stopped) {
                return send_plain_response(fd, msg.header, -ENODEV, {});
            }
            apertures[i] = it->second.aperture_size;
            // Direction must be enabled on the qpair.
            uint32_t dir = xfer.xfers[i].direction;
            if (dir == SLASH_QDMA_XFER_H2C && (it->second.dir_mask & kDirH2cBit) == 0) {
                return send_plain_response(fd, msg.header, -EINVAL, {});
            }
            if (dir == SLASH_QDMA_XFER_C2H && (it->second.dir_mask & kDirC2hBit) == 0) {
                return send_plain_response(fd, msg.header, -EINVAL, {});
            }
            if (dir != SLASH_QDMA_XFER_H2C && dir != SLASH_QDMA_XFER_C2H) {
                return send_plain_response(fd, msg.header, -EINVAL, {});
            }
        }
    }

    // Execute the sub-transfers.  buf_fd holds an INDEX into the transferred fd
    // list (resolve_fd_index moves the fd out of msg.fds, so each index resolves
    // once).  File I/O (pread/pwrite) is done OUTSIDE the model lock; the model
    // request itself (populate/fetch) serialises via ModelClient's internal mutex.
    uint64_t total = 0;
    for (uint32_t i = 0; i < xfer.count; ++i) {
        const slash_qdma_subxfer& sx = xfer.xfers[i];
        auto rfd = resolve_fd_index(msg, static_cast<uint32_t>(sx.buf_fd));
        if (!rfd) {
            return send_plain_response(fd, msg.header, -EINVAL, {});
        }
        UniqueFd data_fd = std::move(rfd.value());

        uint64_t remaining = sx.length;
        uint64_t buf_off   = sx.buf_offset;
        uint64_t dev_addr  = sx.dev_addr;
        const bool to_reconfig =
            (sx.direction == SLASH_QDMA_XFER_H2C && dev_addr == kReconfigApertureAddr);

        // TODO: Implement keyhole transfers if something actually needs it.

        while (remaining > 0) {
            std::size_t chunk = static_cast<std::size_t>(
                std::min<uint64_t>(remaining, kTransferChunk));

            if (sx.direction == SLASH_QDMA_XFER_H2C) {
                // Read from the user buffer (outside the model lock).
                std::vector<uint8_t> data(chunk);
                ssize_t got = ::pread(data_fd.get(), data.data(), chunk,
                                      static_cast<off_t>(buf_off));
                if (got < 0 || static_cast<std::size_t>(got) != chunk) {
                    return send_plain_response(fd, msg.header, -EIO, {});
                }
                if (to_reconfig) {
                    // H2C to the reconfiguration aperture: append to staging VBIN.
                    auto ap = vbin_.append_staging(std::span<const uint8_t>(data));
                    if (!ap) {
                        return send_plain_response(fd, msg.header, -EIO, {});
                    }
                } else {
                    // H2C to HBM/DDR: populate the model at dev_addr.
                    auto pr = model_.populate(dev_addr, std::span<const uint8_t>(data));
                    if (!pr) {
                        // Transport error → model assumed dead → -ENODEV.
                        return send_plain_response(fd, msg.header, -ENODEV, {});
                    }
                }
            } else { // C2H
                // Fetch from the model (serialised), then write to the user buffer
                // outside the lock.
                auto fr = model_.fetch_buffer(dev_addr, chunk);
                if (!fr) {
                    int32_t rv = (fr.error().kind == ErrorKind::Transport) ? -ENODEV : -EIO;
                    return send_plain_response(fd, msg.header, rv, {});
                }
                const std::vector<uint8_t>& data = fr.value();
                if (data.size() != chunk) {
                    return send_plain_response(fd, msg.header, -EIO, {});
                }
                ssize_t put = ::pwrite(data_fd.get(), data.data(), chunk,
                                       static_cast<off_t>(buf_off));
                if (put < 0 || static_cast<std::size_t>(put) != chunk) {
                    return send_plain_response(fd, msg.header, -EIO, {});
                }
            }

            remaining -= chunk;
            buf_off   += chunk;
            // Reconfig-aperture writes are a fixed "mailbox" append at a constant
            // device address, so dev_addr is deliberately NOT advanced for them;
            // HBM/DDR transfers walk forward through device memory as normal.
            if (!to_reconfig) {
                dev_addr += chunk;
            }
            total     += chunk;
        }
        // data_fd closes here; all transferred fds are released by loop end.
    }

    // Any fds beyond those resolved are dropped (closed) with msg on return.
    return send_plain_response(fd, msg.header, static_cast<int32_t>(total), {});
}

// ─────────────────────────────────────────────────────────────────────────────
// Introspection
// ─────────────────────────────────────────────────────────────────────────────

std::size_t QdmaSubsystem::connection_count() const {
    // Count only live (not-yet-finished) workers; a worker whose peer has closed
    // flags itself done and is reaped shortly after, but the count should reflect
    // it as gone immediately.
    std::lock_guard<std::mutex> lk(workers_mtx_);
    std::size_t live = 0;
    for (const auto& [key, w] : workers_) {
        (void)key;
        if (!w->done.load()) {
            ++live;
        }
    }
    return live;
}

std::size_t QdmaSubsystem::session_count() const {
    std::lock_guard<std::mutex> lk(workers_mtx_);
    std::size_t sessions = 0;
    for (const auto& [key, w] : workers_) {
        (void)key;
        if (w->session && !w->done.load()) {
            ++sessions;
        }
    }
    return sessions;
}

std::size_t QdmaSubsystem::qpair_count() const {
    std::lock_guard<std::mutex> lk(qpairs_mtx_);
    return qpairs_.size();
}

} // namespace slash_sysemu
