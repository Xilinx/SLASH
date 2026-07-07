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

#include "ctl_subsystem.h"

#include "ctl_ioctls.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

CtlSubsystem::CtlSubsystem(std::string socket_path, std::string board_bdf,
                           const BarSet& bars)
    : socket_path_(std::move(socket_path)),
      device_bdf_(board_bdf + ".2"),
      bars_(bars) {}

CtlSubsystem::~CtlSubsystem() { remove(); }

// ─────────────────────────────────────────────────────────────────────────────
// setup() — create socket, listener, and (implicitly) the worker pool
// ─────────────────────────────────────────────────────────────────────────────

Result<void> CtlSubsystem::setup() {
    if (active_.load()) {
        return Result<void>::ok(); // idempotent
    }

    // sun_path length guard: leave room for the trailing NUL.
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() + 1 > sizeof(addr.sun_path)) {
        return Result<void>::err(TransportError{
            ErrorKind::Transport,
            "slash_ctl socket path too long: '" + socket_path_ + "'"});
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

    // Commit state before starting the listener so it observes a consistent view.
    listen_fd_   = std::move(sock);
    stop_.store(false);
    active_.store(true);
    listener_ = std::thread([this] { listener_loop(); });

    return Result<void>::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// remove() — teardown to inactive
// ─────────────────────────────────────────────────────────────────────────────

void CtlSubsystem::remove() {
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

    // 2. Force-disconnect every live connection: shutdown() each connection fd so
    //    its worker's blocked recv()/send() fails (the "forced user disconnect"),
    //    then join the workers.  The worker owns and closes the fd itself; we only
    //    signal via shutdown().  Move the map out under the lock, then join
    //    outside it (a worker calls reap/erase paths that also take the lock).
    std::unordered_map<int, std::unique_ptr<Connection>> conns;
    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        conns = std::move(conns_);
        conns_.clear();
    }
    for (auto& [conn_fd, conn] : conns) {
        ::shutdown(conn_fd, SHUT_RDWR);
    }
    for (auto& [conn_fd, conn] : conns) {
        (void)conn_fd;
        if (conn->thread.joinable()) {
            conn->thread.join();
        }
    }

    // The BAR memfds are borrowed and deliberately left untouched.
}

// ─────────────────────────────────────────────────────────────────────────────
// listener_loop — accept connections, spawn one worker each
// ─────────────────────────────────────────────────────────────────────────────

void CtlSubsystem::listener_loop() {
    while (!stop_.load()) {
        int conn = ::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (conn < 0) {
            if (errno == EINTR) {
                continue; // interrupted by a signal; retry
            }
            // accept() failed — typically because remove() shut the listener
            // socket down (EINVAL/EBADF/ECONNABORTED).  Exit the loop.
            break;
        }

        // Opportunistically reap connections whose peers have already closed, so
        // finished worker threads are joined and their handles freed.
        reap_finished_connections();

        auto connection = std::make_unique<Connection>();
        {
            std::lock_guard<std::mutex> lk(conns_mtx_);
            if (stop_.load()) {
                // Racing with remove(): don't register a worker remove() won't
                // see.  Close the accepted fd and stop accepting.
                ::close(conn);
                break;
            }
            connection->thread = std::thread([this, conn] { connection_loop(conn); });
            conns_.emplace(conn, std::move(connection));
        }
    }
}

void CtlSubsystem::reap_finished_connections() {
    std::vector<std::unique_ptr<Connection>> finished;
    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        for (auto it = conns_.begin(); it != conns_.end();) {
            if (it->second->done.load()) {
                finished.push_back(std::move(it->second));
                it = conns_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Join outside the lock (the worker has already flagged done and is about to
    // return, so this join does not block meaningfully).
    for (auto& conn : finished) {
        if (conn->thread.joinable()) {
            conn->thread.join();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// connection_loop — recv → dispatch → send, per connection
// ─────────────────────────────────────────────────────────────────────────────

void CtlSubsystem::connection_loop(int conn_fd) {
    UniqueFd fd(conn_fd); // own the connection fd; closed on return

    while (!stop_.load()) {
        auto req = recv_message(fd.get());
        if (!req) {
            // Transport error (peer closed / shutdown by remove()) or a protocol
            // error: end the connection either way.
            break;
        }
        ReceivedMessage& msg = req.value();

        Result<void> sent = Result<void>::ok();
        switch (msg.header.ioctl_op) {
        case kSlashCtldevIoctlGetBarInfo: {
            // Reject a payload smaller than the argument struct: a malformed
            // request must not be silently defaulted (bar_number 0 would look
            // like a present BAR).  Respond -EINVAL with no payload.
            if (msg.payload.size() < sizeof(slash_ioctl_bar_info)) {
                sent = send_plain_response(fd.get(), msg.header, -EINVAL, {});
                break;
            }
            slash_ioctl_bar_info info{};
            std::memcpy(&info, msg.payload.data(), sizeof(info));
            const BarMemfd* bar = bars_.by_index(info.bar_number);
            if (bar != nullptr) {
                info.usable        = 1;
                info.in_use        = 0; // never in_use
                info.start_address = 0;
                info.length        = bar->size();
            } else {
                info.usable        = 0;
                info.in_use        = 0;
                info.start_address = 0;
                info.length        = 0;
            }
            std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&info),
                                             sizeof(info));
            sent = send_plain_response(fd.get(), msg.header, 0, payload);
            break;
        }
        case kSlashCtldevIoctlGetBarFd: {
            if (msg.payload.size() < sizeof(slash_ioctl_bar_fd_request)) {
                sent = send_plain_response(fd.get(), msg.header, -EINVAL, {});
                break;
            }
            slash_ioctl_bar_fd_request fdreq{};
            std::memcpy(&fdreq, msg.payload.data(), sizeof(fdreq));
            const BarMemfd* bar = bars_.by_index(fdreq.bar_number);
            if (bar == nullptr) {
                // Absent BAR → -EINVAL, no fd, echo request struct unchanged.
                std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&fdreq),
                                                 sizeof(fdreq));
                sent = send_plain_response(fd.get(), msg.header, -EINVAL, payload);
                break;
            }
            auto reopened = bar->reopen();
            if (!reopened) {
                std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&fdreq),
                                                 sizeof(fdreq));
                sent = send_plain_response(fd.get(), msg.header, -EIO, payload);
                break;
            }
            UniqueFd out_fd = std::move(reopened.value());
            fdreq.length    = bar->size();
            // return_value is the index of the fd in the transferred list (0).
            slash_sysemu_socket_header h = make_response_header(msg.header, 0);
            std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&fdreq),
                                             sizeof(fdreq));
            int raw = out_fd.get();
            std::array<int, 1> fds{raw};
            sent = send_message(fd.get(), h, payload, fds);
            // out_fd (UniqueFd) closes the daemon's copy on scope exit, so the
            // description is fully owned by the user after the send.
            break;
        }
        case kSlashCtldevIoctlGetDeviceInfo: {
            if (msg.payload.size() < sizeof(slash_ioctl_device_info)) {
                sent = send_plain_response(fd.get(), msg.header, -EINVAL, {});
                break;
            }
            slash_ioctl_device_info dev{};
            std::memcpy(&dev, msg.payload.data(), sizeof(dev));
            std::memset(dev.bdf, 0, sizeof(dev.bdf));
            // device_bdf_ is board_bdf + ".2"; it fits in 32 chars ("DDDD:BB:DD.2").
            std::size_t n = std::min(device_bdf_.size(), sizeof(dev.bdf) - 1);
            std::memcpy(dev.bdf, device_bdf_.data(), n);
            dev.bdf[n]               = '\0';
            dev.vendor_id            = kPf2VendorId;
            dev.device_id            = kPf2DeviceId;
            dev.subsystem_vendor_id  = kPf2SubsystemVendorId;
            dev.subsystem_device_id  = kPf2SubsystemDeviceId;
            std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&dev),
                                             sizeof(dev));
            sent = send_plain_response(fd.get(), msg.header, 0, payload);
            break;
        }
        default: {
            // Unknown ioctl_op → -ENOSYS; echo the received payload back so the
            // datagram is well-formed.  The worker survives and serves the next
            // request.
            sent = send_plain_response(fd.get(), msg.header, -ENOSYS,
                                       std::span<const uint8_t>(msg.payload));
            break;
        }
        }

        if (!sent) {
            // Failed to send the response (peer gone / shutdown): end connection.
            break;
        }
    }

    // Flag done so the listener's opportunistic reaper can join+erase us.  If
    // remove() moved us out first, this flag is harmless.
    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        if (auto it = conns_.find(fd.get()); it != conns_.end()) {
            it->second->done.store(true);
        }
    }
    // fd closes here (UniqueFd dtor).
}

std::size_t CtlSubsystem::connection_count() const {
    std::lock_guard<std::mutex> lk(conns_mtx_);
    return conns_.size();
}

} // namespace slash_sysemu
