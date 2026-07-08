/*
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _GNU_SOURCE

#include "sysemu_test_server.h"

#include <slash/uapi/slash_interface.h>
#include <slash/uapi/slash_sysemu.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Maximum payload bytes we ever need to read from the client. */
static constexpr size_t kMaxPayload = 65536;
/* Maximum fds per message. */
static constexpr size_t kMaxFds     = 64;
/* cmsg buffer large enough for kMaxFds fds. */
static constexpr size_t kCmsgBufSz  =
    /* CMSG_SPACE is a macro, evaluate at runtime via inline */ 0; /* computed below */

/* ── Helpers ────────────────────────────────────────────────────────────────── */

static std::size_t cmsg_buf_size()
{
    return CMSG_SPACE(sizeof(int) * kMaxFds);
}

/* ── Constructor / Destructor ────────────────────────────────────────────────── */

SysemuTestServer::SysemuTestServer()
{
    for (int i = 0; i < kMaxQdmaQpairs; ++i) {
        xfer_sv_[i][0] = -1;
        xfer_sv_[i][1] = -1;
    }
}

SysemuTestServer::~SysemuTestServer()
{
    Stop();
}

/* ── Start / Stop ─────────────────────────────────────────────────────────── */

bool SysemuTestServer::Start()
{
    if (running_.load()) return true;

    /* Initialise XFER socket-pair slots. */
    for (int i = 0; i < kMaxQdmaQpairs; ++i) {
        xfer_sv_[i][0] = -1;
        xfer_sv_[i][1] = -1;
    }

    /* Initialise device memory (simulate card-side DRAM). */
    dev_mem_.assign(kDevMemSize, 0);

    /* Create persistent memfds for present BARs. */
    for (int i = 0; i < 6; ++i) {
        if (bar_size[i] == 0) {
            bar_memfd[i] = -1;
            continue;
        }
        bar_memfd[i] = ::memfd_create("slash_bar_sysemu", MFD_CLOEXEC);
        if (bar_memfd[i] < 0) return false;
        if (::ftruncate(bar_memfd[i], static_cast<off_t>(bar_size[i])) < 0) {
            ::close(bar_memfd[i]);
            bar_memfd[i] = -1;
            return false;
        }
    }

    /* Build a unique temp path. */
    char tmpl[] = "/tmp/slash_sysemu_test_XXXXXX";
    int tmp_fd = ::mkstemp(tmpl);
    if (tmp_fd < 0) return false;
    ::close(tmp_fd);
    ::unlink(tmpl);            /* we want the path, not the file */
    socket_path_ = tmpl;

    listen_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size());

    if (::bind(listen_fd_,
               reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 16) != 0) {
        ::unlink(socket_path_.c_str());
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread([this] { AcceptLoop(); });
    return true;
}

void SysemuTestServer::Stop()
{
    if (!running_.exchange(false)) return;

    /* Unblock accept() by shutting the listen socket. */
    ::shutdown(listen_fd_, SHUT_RDWR);
    if (accept_thread_.joinable()) accept_thread_.join();

    ::close(listen_fd_);
    listen_fd_ = -1;
    ::unlink(socket_path_.c_str());

    /* Close persistent BAR memfds. */
    for (int i = 0; i < 6; ++i) {
        if (bar_memfd[i] >= 0) {
            ::close(bar_memfd[i]);
            bar_memfd[i] = -1;
        }
    }

    /* Unblock XFER session threads by shutting down the server-side fds.
     * ConnectionLoop owns the fd lifecycle and will close them on exit.
     * sv[1] (client fd) is already closed after SCM_RIGHTS transfer; skip
     * any remaining slots that were never handed off. */
    {
        std::lock_guard<std::mutex> lk(qdma_mtx_);
        for (int i = 0; i < kMaxQdmaQpairs; ++i) {
            if (xfer_sv_[i][0] >= 0) {
                ::shutdown(xfer_sv_[i][0], SHUT_RDWR);
                xfer_sv_[i][0] = -1;
            }
            if (xfer_sv_[i][1] >= 0) { ::close(xfer_sv_[i][1]); xfer_sv_[i][1] = -1; }
        }
    }

    /* Join all connection workers. */
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        workers = std::move(workers_);
        workers_.clear();
    }
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
}

/* ── Fault injection ──────────────────────────────────────────────────────── */

void SysemuTestServer::InjectFault(SysemuFault fault, int errno_code)
{
    std::lock_guard<std::mutex> lk(fault_mtx_);
    fault_        = fault;
    fault_errno_  = errno_code;
}

void SysemuTestServer::ClearFault()
{
    std::lock_guard<std::mutex> lk(fault_mtx_);
    fault_       = SysemuFault::None;
    fault_errno_ = 0;
}

/* ── Request recording ────────────────────────────────────────────────────── */

std::optional<RecordedRequest> SysemuTestServer::LastRequest()
{
    std::lock_guard<std::mutex> lk(records_mtx_);
    if (records_.empty()) return std::nullopt;
    return records_.back();
}

void SysemuTestServer::ClearRecords()
{
    std::lock_guard<std::mutex> lk(records_mtx_);
    records_.clear();
}

/* ── AcceptLoop ───────────────────────────────────────────────────────────── */

void SysemuTestServer::AcceptLoop()
{
    while (running_.load()) {
        int conn = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (conn < 0) {
            if (errno == EINTR) continue;
            break; /* shutdown() caused EINVAL/EBADF */
        }
        std::lock_guard<std::mutex> lk(workers_mtx_);
        workers_.emplace_back([this, conn] { ConnectionLoop(conn); });
    }
}

/* ── ConnectionLoop ──────────────────────────────────────────────────────── */

void SysemuTestServer::ConnectionLoop(int conn_fd)
{
    /* Receive buffer: header + max payload. */
    const size_t recv_buf_size =
        sizeof(slash_sysemu_socket_header) + kMaxPayload;
    std::vector<uint8_t> recv_buf(recv_buf_size);

    /* cmsg buffer. */
    size_t cmsg_sz = CMSG_SPACE(sizeof(int) * kMaxFds);
    std::vector<char> cmsg_buf(cmsg_sz, 0);

    while (running_.load()) {
        struct iovec iov{};
        iov.iov_base = recv_buf.data();
        iov.iov_len  = recv_buf.size();

        struct msghdr msg{};
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = cmsg_buf.data();
        msg.msg_controllen = static_cast<socklen_t>(cmsg_buf.size());

        ssize_t n = ::recvmsg(conn_fd, &msg, MSG_CMSG_CLOEXEC);
        if (n <= 0) break;
        if (static_cast<size_t>(n) < sizeof(slash_sysemu_socket_header)) break;

        /* Extract received fds from SCM_RIGHTS. */
        std::vector<int> recv_fds;
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
             cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET ||
                cmsg->cmsg_type  != SCM_RIGHTS) continue;
            size_t fd_bytes  = cmsg->cmsg_len - CMSG_LEN(0);
            size_t fd_count  = fd_bytes / sizeof(int);
            const int* fdp   = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
            for (size_t i = 0; i < fd_count; ++i) {
                recv_fds.push_back(fdp[i]);
            }
        }

        slash_sysemu_socket_header req_hdr{};
        std::memcpy(&req_hdr, recv_buf.data(), sizeof(req_hdr));

        size_t payload_len = static_cast<size_t>(n) -
                             sizeof(slash_sysemu_socket_header);
        std::vector<uint8_t> payload(
            recv_buf.begin() + sizeof(slash_sysemu_socket_header),
            recv_buf.begin() + sizeof(slash_sysemu_socket_header) + payload_len);

        /* Record the request. */
        {
            RecordedRequest rec;
            rec.ioctl_op    = req_hdr.ioctl_op;
            rec.sequence_id = req_hdr.sequence_id;
            rec.payload     = payload;
            /* dup received fds into the record. */
            for (int fd : recv_fds) {
                rec.fds.push_back(::dup(fd));
            }
            std::lock_guard<std::mutex> lk(records_mtx_);
            records_.push_back(std::move(rec));
        }

        bool keep = Dispatch(conn_fd, req_hdr, payload, recv_fds);

        /* Close received fds (we've duped what we need). */
        for (int fd : recv_fds) {
            ::close(fd);
        }

        if (!keep) break;
    }

    ::close(conn_fd);
}

/* ── Dispatch ─────────────────────────────────────────────────────────────── */

bool SysemuTestServer::Dispatch(int conn_fd,
                                const slash_sysemu_socket_header& req_hdr,
                                const std::vector<uint8_t>& payload,
                                const std::vector<int>& recv_fds)
{
    /* Check fault first. */
    SysemuFault fault;
    int         fault_errno;
    {
        std::lock_guard<std::mutex> lk(fault_mtx_);
        fault       = fault_;
        fault_errno = fault_errno_;
    }

    if (fault == SysemuFault::PeerClose) {
        return false; /* drop connection without reply */
    }

    if (fault == SysemuFault::TruncatedReply) {
        /* Send a single byte — client should see MSG_TRUNC or short read. */
        char one = 0;
        ::send(conn_fd, &one, 1, MSG_NOSIGNAL);
        return false;
    }

    if (fault == SysemuFault::DaemonError) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -fault_errno);
        return SendResponse(conn_fd, resp, payload.data(), payload.size(), nullptr, 0);
    }

    /* Apply WrongSeq / WrongOp by dispatching normally then patching. */
    /* Dispatch to the appropriate handler. */
    uint32_t op = req_hdr.ioctl_op;

    bool ok = false;
    if (op == static_cast<uint32_t>(SLASH_CTLDEV_IOCTL_GET_BAR_INFO)) {
        ok = HandleGetBarInfo(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_CTLDEV_IOCTL_GET_BAR_FD)) {
        ok = HandleGetBarFd(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO)) {
        ok = HandleGetDeviceInfo(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_QDMA_IOCTL_INFO)) {
        ok = HandleQdmaInfo(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_QDMA_IOCTL_QPAIR_ADD)) {
        ok = HandleQdmaQpairAdd(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_QDMA_IOCTL_Q_OP)) {
        ok = HandleQdmaQOp(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_QDMA_IOCTL_QPAIR_GET_FD)) {
        ok = HandleQdmaQpairGetFd(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_QDMA_IOCTL_BUF_CREATE)) {
        ok = HandleQdmaBufCreate(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_QDMA_QPAIR_IOCTL_TRANSFER)) {
        ok = HandleQdmaTransfer(conn_fd, req_hdr, payload, recv_fds);
    } else if (op == static_cast<uint32_t>(SLASH_HOTPLUG_IOCTL_RESCAN)) {
        ok = HandleHotplugRescan(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_HOTPLUG_IOCTL_REMOVE)) {
        ok = HandleHotplugRemove(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_HOTPLUG_IOCTL_TOGGLE_SBR)) {
        ok = HandleHotplugToggleSbr(conn_fd, req_hdr, payload);
    } else if (op == static_cast<uint32_t>(SLASH_HOTPLUG_IOCTL_HOTPLUG)) {
        ok = HandleHotplugHotplug(conn_fd, req_hdr, payload);
    } else {
        /* Unknown op → -ENOSYS */
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -ENOSYS);
        ok = SendResponse(conn_fd, resp, payload.data(), payload.size(), nullptr, 0);
    }

    (void)ok;
    return true;
}

slash_sysemu_socket_header SysemuTestServer::MakeResponseHeader(
    const slash_sysemu_socket_header& req, int32_t return_value)
{
    slash_sysemu_socket_header h{};

    SysemuFault fault;
    {
        std::lock_guard<std::mutex> lk(fault_mtx_);
        fault = fault_;
    }

    h.ioctl_op     = req.ioctl_op;
    h.sequence_id  = req.sequence_id;
    h.return_value = static_cast<uint32_t>(return_value);
    h.pad          = 0;

    if (fault == SysemuFault::WrongSeq) h.sequence_id  ^= 0xFFFFFFFFu;
    if (fault == SysemuFault::WrongOp)  h.ioctl_op     ^= 0xFFu;

    return h;
}

bool SysemuTestServer::SendResponse(int conn_fd,
                                    const slash_sysemu_socket_header& resp_hdr,
                                    const void* payload, size_t payload_len,
                                    const int* fds, size_t n_fds)
{
    struct iovec iov[2]{};
    iov[0].iov_base = const_cast<void*>(static_cast<const void*>(&resp_hdr));
    iov[0].iov_len  = sizeof(resp_hdr);
    iov[1].iov_base = const_cast<void*>(payload);
    iov[1].iov_len  = payload_len;

    struct msghdr msg{};
    msg.msg_iov    = iov;
    msg.msg_iovlen = (payload_len > 0) ? 2 : 1;

    /* cmsg for SCM_RIGHTS fds. */
    std::vector<char> cmsg_buf;
    if (n_fds > 0 && fds != nullptr) {
        size_t fd_bytes = sizeof(int) * n_fds;
        cmsg_buf.resize(CMSG_SPACE(fd_bytes), 0);
        msg.msg_control    = cmsg_buf.data();
        msg.msg_controllen = static_cast<socklen_t>(cmsg_buf.size());
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = static_cast<socklen_t>(CMSG_LEN(fd_bytes));
        std::memcpy(CMSG_DATA(cmsg), fds, fd_bytes);
    }

    ssize_t sent = ::sendmsg(conn_fd, &msg, MSG_NOSIGNAL);
    return sent > 0;
}

/* ── Handlers ─────────────────────────────────────────────────────────────── */

bool SysemuTestServer::HandleGetBarInfo(int conn_fd,
                                        const slash_sysemu_socket_header& req_hdr,
                                        const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_ioctl_bar_info)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_ioctl_bar_info info{};
    std::memcpy(&info, payload.data(), sizeof(info));

    int bar = info.bar_number;
    if (bar >= 0 && bar < 6 && bar_size[bar] > 0) {
        info.usable        = 1;
        info.in_use        = 0;
        info.start_address = 0;
        info.length        = bar_size[bar];
    } else {
        info.usable        = 0;
        info.in_use        = 0;
        info.start_address = 0;
        info.length        = 0;
    }

    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, 0);
    return SendResponse(conn_fd, resp, &info, sizeof(info), nullptr, 0);
}

bool SysemuTestServer::HandleGetBarFd(int conn_fd,
                                      const slash_sysemu_socket_header& req_hdr,
                                      const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_ioctl_bar_fd_request)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_ioctl_bar_fd_request req{};
    std::memcpy(&req, payload.data(), sizeof(req));

    int bar = req.bar_number;
    if (bar < 0 || bar >= 6 || bar_size[bar] == 0 || bar_memfd[bar] < 0) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
    }

    /*
     * Reopen via /proc/self/fd/<n> to get a distinct open file description
     * on the same inode.  This mirrors the daemon's BarMemfd::reopen() so
     * that flock(2) operates on the same inode for all clients — which is
     * required for the LOCK_EX / LOCK_SH exclusion proof in the tests.
     */
    char proc_path[64];
    std::snprintf(proc_path, sizeof(proc_path),
                  "/proc/self/fd/%d", bar_memfd[bar]);
    int out_fd = ::open(proc_path, O_RDWR | O_CLOEXEC);
    if (out_fd < 0) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EIO);
        return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
    }

    req.length = bar_size[bar];
    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, 0);
    bool ok = SendResponse(conn_fd, resp, &req, sizeof(req), &out_fd, 1);
    ::close(out_fd); /* daemon closes its copy; client owns the transferred fd */
    return ok;
}

bool SysemuTestServer::HandleGetDeviceInfo(int conn_fd,
                                           const slash_sysemu_socket_header& req_hdr,
                                           const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_ioctl_device_info)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_ioctl_device_info info{};
    std::memcpy(&info, payload.data(), sizeof(info));

    std::memset(info.bdf, 0, sizeof(info.bdf));
    size_t n = std::min(device_bdf.size(), sizeof(info.bdf) - 1);
    std::memcpy(info.bdf, device_bdf.data(), n);
    info.bdf[n]               = '\0';
    info.vendor_id            = 0x10EE;
    info.device_id            = 0x50B6;
    info.subsystem_vendor_id  = 0x10EE;
    info.subsystem_device_id  = 0x000e;

    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, 0);
    return SendResponse(conn_fd, resp, &info, sizeof(info), nullptr, 0);
}

/* ── QDMA Handlers ────────────────────────────────────────────────────────── */

bool SysemuTestServer::HandleQdmaInfo(int conn_fd,
                                      const slash_sysemu_socket_header& req_hdr,
                                      const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_qdma_info)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_qdma_info info{};
    std::memcpy(&info, payload.data(), sizeof(info));

    info.qsets_max  = qdma_qsets_max;
    info.msix_qvecs = qdma_msix_qvecs;
    info.vf_max     = qdma_vf_max;
    info.caps       = qdma_caps;

    std::memset(info.bdf, 0, sizeof(info.bdf));
    size_t n = std::min(qdma_bdf.size(), sizeof(info.bdf) - 1);
    std::memcpy(info.bdf, qdma_bdf.data(), n);
    info.bdf[n] = '\0';

    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, 0);
    return SendResponse(conn_fd, resp, &info, sizeof(info), nullptr, 0);
}

bool SysemuTestServer::HandleQdmaQpairAdd(int conn_fd,
                                          const slash_sysemu_socket_header& req_hdr,
                                          const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_qdma_qpair_add)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_qdma_qpair_add req{};
    std::memcpy(&req, payload.data(), sizeof(req));

    {
        std::lock_guard<std::mutex> lk(qdma_mtx_);
        req.qid = next_qid++;
    }

    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, 0);
    return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
}

bool SysemuTestServer::HandleQdmaQOp(int conn_fd,
                                     const slash_sysemu_socket_header& req_hdr,
                                     const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_qdma_qpair_op)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_qdma_qpair_op req{};
    std::memcpy(&req, payload.data(), sizeof(req));

    /* For the test server, all start/stop/del ops simply succeed. */
    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, 0);
    return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
}

bool SysemuTestServer::HandleQdmaQpairGetFd(int conn_fd,
                                            const slash_sysemu_socket_header& req_hdr,
                                            const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_qdma_qpair_fd_request)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_qdma_qpair_fd_request req{};
    std::memcpy(&req, payload.data(), sizeof(req));

    /*
     * Determine the primary qid (compat: qpair_count==0 uses req.qid).
     */
    uint32_t primary_qid = (req.qpair_count == 0) ? req.qid : req.qpair_ids[0];

    if (primary_qid >= static_cast<uint32_t>(kMaxQdmaQpairs)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
    }

    /*
     * Create a SOCK_SEQPACKET socket pair.  The server-side fd (sv[0])
     * will be read by a session thread that handles TRANSFER requests.
     * The client-side fd (sv[1]) is handed to libslash via SCM_RIGHTS.
     */
    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EIO);
        return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
    }

    {
        std::lock_guard<std::mutex> lk(qdma_mtx_);
        /* Close any previous pair on this slot. */
        if (xfer_sv_[primary_qid][0] >= 0) ::close(xfer_sv_[primary_qid][0]);
        if (xfer_sv_[primary_qid][1] >= 0) ::close(xfer_sv_[primary_qid][1]);
        xfer_sv_[primary_qid][0] = sv[0];
        xfer_sv_[primary_qid][1] = sv[1];
    }

    /* Spawn a thread to serve TRANSFER requests on the server-side fd. */
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        int server_fd = sv[0];
        workers_.emplace_back([this, server_fd] { ConnectionLoop(server_fd); });
    }

    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, 0);
    bool ok = SendResponse(conn_fd, resp, &req, sizeof(req), &sv[1], 1);
    ::close(sv[1]); /* server no longer needs the client fd */
    xfer_sv_[primary_qid][1] = -1; /* prevent double-close in Stop() */
    return ok;
}

bool SysemuTestServer::HandleQdmaBufCreate(int conn_fd,
                                           const slash_sysemu_socket_header& req_hdr,
                                           const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_qdma_buf_create)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_qdma_buf_create req{};
    std::memcpy(&req, payload.data(), sizeof(req));

    if (req.length == 0) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
    }

    /* Create a memfd to represent the "DMA buffer". */
    int buf_fd = ::memfd_create("slash_qdma_buf", MFD_CLOEXEC);
    if (buf_fd < 0) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -ENOMEM);
        return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
    }
    if (::ftruncate(buf_fd, static_cast<off_t>(req.length)) < 0) {
        ::close(buf_fd);
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -ENOMEM);
        return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
    }

    req.granule       = 4096u; /* 4 KiB page granule */
    req.transfer_hint = static_cast<uint32_t>(SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR);

    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, 0);
    bool ok = SendResponse(conn_fd, resp, &req, sizeof(req), &buf_fd, 1);
    ::close(buf_fd);
    return ok;
}

bool SysemuTestServer::HandleQdmaTransfer(int conn_fd,
                                          const slash_sysemu_socket_header& req_hdr,
                                          const std::vector<uint8_t>& payload,
                                          const std::vector<int>& recv_fds)
{
    if (payload.size() < sizeof(slash_qdma_transfer)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_qdma_transfer req{};
    std::memcpy(&req, payload.data(), sizeof(req));

    if (req.count == 0 || req.count > SLASH_QDMA_FD_MAX_QPAIRS) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
    }

    /*
     * Emulate each sub-transfer by copying between the buffer memfd
     * (received via SCM_RIGHTS by index) and dev_mem_.
     *
     * Addresses at kReconfigApertureAddr are the reconfig aperture:
     *   H2C  → append bytes to staging_ (does NOT write dev_mem_).
     *   C2H  → invalid; return -EINVAL.
     * All other device addresses map into dev_mem_ by offset.
     * Out-of-range addresses are accepted; data is silently discarded / zeroed.
     */
    int64_t total = 0;
    for (uint32_t i = 0; i < req.count; ++i) {
        const slash_qdma_subxfer& sx = req.xfers[i];

        int buf_idx = static_cast<int>(sx.buf_fd); /* index into recv_fds */
        if (buf_idx < 0 ||
            static_cast<size_t>(buf_idx) >= recv_fds.size()) {
            slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EBADF);
            return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
        }
        int buf_fd = recv_fds[static_cast<size_t>(buf_idx)];

        uint64_t length = sx.length;
        bool is_aperture = (sx.dev_addr == kReconfigApertureAddr);

        /* Reconfig aperture: H2C appends to staging_; C2H is invalid. */
        if (is_aperture) {
            if (sx.direction == static_cast<uint32_t>(SLASH_QDMA_XFER_C2H)) {
                slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
                return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
            }
            /* H2C: read from host buffer and append to staging_. */
            std::vector<uint8_t> tmp(static_cast<size_t>(length));
            ssize_t got = ::pread(buf_fd, tmp.data(),
                                  static_cast<size_t>(length),
                                  static_cast<off_t>(sx.buf_offset));
            if (got < 0) {
                slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EIO);
                return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
            }
            staging_.insert(staging_.end(), tmp.begin(),
                            tmp.begin() + static_cast<ptrdiff_t>(got));
            total += got;
            continue;
        }

        /* Normal device memory transfer. */
        uint64_t dev_offset = 0;
        bool in_dev_mem = false;
        if (sx.dev_addr < kDevMemSize) {
            dev_offset = sx.dev_addr;
            if (dev_offset + length <= kDevMemSize) {
                in_dev_mem = true;
            }
        }

        if (sx.direction == static_cast<uint32_t>(SLASH_QDMA_XFER_H2C)) {
            /* Read from host buffer, write to device. */
            std::vector<uint8_t> tmp(static_cast<size_t>(length));
            ssize_t got = ::pread(buf_fd, tmp.data(),
                                  static_cast<size_t>(length),
                                  static_cast<off_t>(sx.buf_offset));
            if (got < 0) {
                slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EIO);
                return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
            }
            if (in_dev_mem) {
                std::memcpy(dev_mem_.data() + dev_offset,
                            tmp.data(), static_cast<size_t>(got));
            }
            total += got;
        } else if (sx.direction == static_cast<uint32_t>(SLASH_QDMA_XFER_C2H)) {
            /* Read from device, write to host buffer. */
            std::vector<uint8_t> tmp(static_cast<size_t>(length), 0);
            if (in_dev_mem) {
                std::memcpy(tmp.data(),
                            dev_mem_.data() + dev_offset,
                            static_cast<size_t>(length));
            }
            ssize_t put = ::pwrite(buf_fd, tmp.data(),
                                   static_cast<size_t>(length),
                                   static_cast<off_t>(sx.buf_offset));
            if (put < 0) {
                slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EIO);
                return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
            }
            total += put;
        } else {
            slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
            return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
        }
    }

    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr,
                                                          static_cast<int32_t>(total));
    return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
}

/* ── Hotplug handlers ─────────────────────────────────────────────────────── */

bool SysemuTestServer::HandleHotplugRescan(int conn_fd,
                                           const slash_sysemu_socket_header& req_hdr,
                                           const std::vector<uint8_t>& /*payload*/)
{
    /* RESCAN carries no argument struct — payload is ignored. */
    int32_t rv = 0;
    if (hotplug_no_device) {
        rv = -ENODEV;
    }
    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, rv);
    return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
}

bool SysemuTestServer::HandleHotplugRemove(int conn_fd,
                                           const slash_sysemu_socket_header& req_hdr,
                                           const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_hotplug_device_request)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_hotplug_device_request req{};
    std::memcpy(&req, payload.data(), sizeof(req));
    req.bdf[SLASH_HOTPLUG_BDF_LEN - 1] = '\0'; /* ensure NUL-termination */

    last_hotplug_bdf = req.bdf;

    int32_t rv = (hotplug_error_code != 0) ? static_cast<int32_t>(hotplug_error_code) : 0;
    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, rv);
    return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
}

bool SysemuTestServer::HandleHotplugToggleSbr(int conn_fd,
                                              const slash_sysemu_socket_header& req_hdr,
                                              const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_hotplug_device_request)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_hotplug_device_request req{};
    std::memcpy(&req, payload.data(), sizeof(req));
    req.bdf[SLASH_HOTPLUG_BDF_LEN - 1] = '\0';

    last_hotplug_bdf = req.bdf;

    int32_t rv = (hotplug_error_code != 0) ? static_cast<int32_t>(hotplug_error_code) : 0;
    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, rv);
    return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
}

bool SysemuTestServer::HandleHotplugHotplug(int conn_fd,
                                            const slash_sysemu_socket_header& req_hdr,
                                            const std::vector<uint8_t>& payload)
{
    if (payload.size() < sizeof(slash_hotplug_device_request)) {
        slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, -EINVAL);
        return SendResponse(conn_fd, resp, nullptr, 0, nullptr, 0);
    }

    slash_hotplug_device_request req{};
    std::memcpy(&req, payload.data(), sizeof(req));
    req.bdf[SLASH_HOTPLUG_BDF_LEN - 1] = '\0';

    last_hotplug_bdf = req.bdf;

    int32_t rv = (hotplug_error_code != 0) ? static_cast<int32_t>(hotplug_error_code) : 0;
    slash_sysemu_socket_header resp = MakeResponseHeader(req_hdr, rv);
    return SendResponse(conn_fd, resp, &req, sizeof(req), nullptr, 0);
}
