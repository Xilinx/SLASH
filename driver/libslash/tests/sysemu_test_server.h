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

/**
 * @file sysemu_test_server.h
 *
 * Reusable in-process AF_UNIX/SOCK_SEQPACKET test server that simulates
 * the slash_sysemu daemon for libslash unit tests.
 *
 * Lifecycle:
 *   SysemuTestServer srv;
 *   srv.Start();                   // binds socket, starts accept thread
 *   std::string path = srv.Path(); // pass to slash_ctldev_open / qdma_open
 *   ... exercise libslash ...
 *   srv.Stop();                    // clean shutdown, joins all threads
 *
 * Fault injection (set before the client call, reset after):
 *   srv.InjectFault(SysemuFault::PeerClose);   // next connection is closed immediately
 *   srv.InjectFault(SysemuFault::TruncatedReply); // response is 1 byte
 *   srv.InjectFault(SysemuFault::WrongSeq);    // response sequence_id incremented by 1
 *   srv.InjectFault(SysemuFault::WrongOp);     // response ioctl_op XOR'd with 0xFF
 *   srv.InjectFault(SysemuFault::DaemonError, errno_code); // return_value set to -errno
 *   srv.ClearFault();
 *
 * Request recording:
 *   srv.LastRequest()  → std::optional<RecordedRequest>
 *   srv.ClearRecords()
 */

#pragma once

#include <slash/uapi/slash_hotplug.h>
#include <slash/uapi/slash_interface.h>
#include <slash/uapi/slash_sysemu.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/* ── Fault injection ──────────────────────────────────────────────────────── */

enum class SysemuFault {
    None,
    PeerClose,       /* close the connection before sending a reply */
    TruncatedReply,  /* send a 1-byte reply (triggers MSG_TRUNC on the client) */
    WrongSeq,        /* flip sequence_id in the response */
    WrongOp,         /* flip ioctl_op in the response */
    DaemonError,     /* set return_value to -errno_code */
};

/* ── Recorded request (for test assertions) ──────────────────────────────── */

struct RecordedRequest {
    uint32_t             ioctl_op;
    uint32_t             sequence_id;
    std::vector<uint8_t> payload;    /* arg struct bytes following the header */
    std::vector<int>     fds;        /* received SCM_RIGHTS fds (already duped) */
};

/* ── Server ───────────────────────────────────────────────────────────────── */

class SysemuTestServer {
public:
    SysemuTestServer();
    ~SysemuTestServer();

    /* Non-copyable, non-movable. */
    SysemuTestServer(const SysemuTestServer&)            = delete;
    SysemuTestServer& operator=(const SysemuTestServer&) = delete;

    /**
     * Bind a temporary AF_UNIX socket and start the accept loop.
     * Returns false on failure (check errno).
     */
    bool Start();

    /**
     * Stop accepting, close all connections, join all threads.
     * Safe to call from any thread.  Idempotent.
     */
    void Stop();

    /** Socket filesystem path (valid after Start()). */
    const std::string& Path() const { return socket_path_; }

    /* ── Fault injection ──────────────────────────────────────────────────── */

    void InjectFault(SysemuFault fault, int errno_code = 0);
    void ClearFault();

    /* ── Request recording ────────────────────────────────────────────────── */

    /** Returns the most-recently-completed request, or nullopt. */
    std::optional<RecordedRequest> LastRequest();
    void ClearRecords();

    /**
     * BDF string returned by GET_DEVICE_INFO and QDMA INFO.
     * Settable before Start() or between requests.
     */
    std::string device_bdf = "0000:01:00.2";

    /**
     * BDF string returned by QDMA INFO (PF1).  Defaults to PF1 of device_bdf.
     * Set before Start() if a distinct QDMA BDF is needed.
     */
    std::string qdma_bdf = "0000:01:00.1";

    /**
     * BAR sizes indexed by BAR number.  BARs 0, 2, 4 are present
     * by default (64 MiB each); 1, 3, 5 are absent (size 0 → usable=0).
     */
    uint64_t bar_size[6] = {
        64ULL * 1024 * 1024,  /* BAR 0 */
        0,                    /* BAR 1 */
        64ULL * 1024 * 1024,  /* BAR 2 */
        0,                    /* BAR 3 */
        64ULL * 1024 * 1024,  /* BAR 4 */
        0,                    /* BAR 5 */
    };

    /**
     * Persistent memfds for each BAR, created on Start().
     * GET_BAR_FD reopens via /proc/self/fd/<n> so every client gets a
     * distinct open file description on the *same* inode — exactly
     * mirroring the daemon's BarMemfd::reopen() behaviour.
     * bar_memfd[n] == -1 when bar_size[n] == 0 (absent BAR).
     */
    int bar_memfd[6] = {-1, -1, -1, -1, -1, -1};

    /* ── Hotplug state (public for test assertions) ──────────────────────── */

    /**
     * BDF string recorded by the last REMOVE / TOGGLE_SBR / HOTPLUG request.
     * Empty after ClearRecords() or before any such request.
     */
    std::string last_hotplug_bdf;

    /**
     * When non-zero, HandleHotplug* handlers for REMOVE / TOGGLE_SBR / HOTPLUG
     * return this value as the daemon's return_value (negative errno).
     * Set to e.g. -ENODEV before the call, reset to 0 afterwards.
     * Does NOT affect RESCAN.
     */
    int hotplug_error_code = 0;

    /**
     * When true, HandleHotplugRescan returns -ENODEV (simulates
     * "no device tracked").  Set before the call, reset after.
     */
    bool hotplug_no_device = false;

    /* ── QDMA state (public for test assertions) ──────────────────────────── */

    /**
     * QDMA capabilities returned by QDMA_INFO.
     */
    uint32_t qdma_qsets_max  = 64;
    uint32_t qdma_msix_qvecs = 8;
    uint32_t qdma_vf_max     = 0;
    uint32_t qdma_caps       = 0;

    /**
     * Next qid to hand out on QPAIR_ADD.  Auto-incremented.
     */
    uint32_t next_qid = 0;

    /**
     * "Device memory" address that maps to the reconfig-aperture.
     * Tests use this to target H2C/C2H transfers at the staging area.
     */
    static constexpr uint64_t kReconfigApertureAddr = 0x102100000ULL;

    /**
     * Simulated device memory.  H2C writes land here; C2H reads come from here.
     * Accessible by tests to verify transfer contents.
     * Transfers targeting kReconfigApertureAddr are NOT routed here.
     */
    std::vector<uint8_t> dev_mem_;

    /**
     * Staging buffer for reconfig-aperture H2C data.
     * H2C writes to kReconfigApertureAddr append here; C2H readback from the
     * aperture is invalid (the daemon does not support it).
     * Accessible by tests to assert staged bytes.
     */
    std::vector<uint8_t> staging_;

private:
    /* Dispatch a single received datagram; send the response.
     * Returns false if the connection should be dropped. */
    bool Dispatch(int conn_fd, const slash_sysemu_socket_header& req_hdr,
                  const std::vector<uint8_t>& payload,
                  const std::vector<int>& recv_fds);

    bool HandleGetBarInfo(int conn_fd,
                          const slash_sysemu_socket_header& req_hdr,
                          const std::vector<uint8_t>& payload);

    bool HandleGetBarFd(int conn_fd,
                        const slash_sysemu_socket_header& req_hdr,
                        const std::vector<uint8_t>& payload);

    bool HandleGetDeviceInfo(int conn_fd,
                             const slash_sysemu_socket_header& req_hdr,
                             const std::vector<uint8_t>& payload);

    /* QDMA handlers (CTL socket). */
    bool HandleQdmaInfo(int conn_fd,
                        const slash_sysemu_socket_header& req_hdr,
                        const std::vector<uint8_t>& payload);

    bool HandleQdmaQpairAdd(int conn_fd,
                            const slash_sysemu_socket_header& req_hdr,
                            const std::vector<uint8_t>& payload);

    bool HandleQdmaQOp(int conn_fd,
                       const slash_sysemu_socket_header& req_hdr,
                       const std::vector<uint8_t>& payload);

    bool HandleQdmaQpairGetFd(int conn_fd,
                              const slash_sysemu_socket_header& req_hdr,
                              const std::vector<uint8_t>& payload);

    bool HandleQdmaBufCreate(int conn_fd,
                             const slash_sysemu_socket_header& req_hdr,
                             const std::vector<uint8_t>& payload);

    /* QDMA transfer handler — issued on the XFER socket (session thread). */
    bool HandleQdmaTransfer(int conn_fd,
                            const slash_sysemu_socket_header& req_hdr,
                            const std::vector<uint8_t>& payload,
                            const std::vector<int>& recv_fds);

    /* Hotplug handlers. */
    bool HandleHotplugRescan(int conn_fd,
                             const slash_sysemu_socket_header& req_hdr,
                             const std::vector<uint8_t>& payload);

    bool HandleHotplugRemove(int conn_fd,
                             const slash_sysemu_socket_header& req_hdr,
                             const std::vector<uint8_t>& payload);

    bool HandleHotplugToggleSbr(int conn_fd,
                                const slash_sysemu_socket_header& req_hdr,
                                const std::vector<uint8_t>& payload);

    bool HandleHotplugHotplug(int conn_fd,
                              const slash_sysemu_socket_header& req_hdr,
                              const std::vector<uint8_t>& payload);

    /* Low-level send helpers. */
    bool SendResponse(int conn_fd,
                      const slash_sysemu_socket_header& resp_hdr,
                      const void* payload, size_t payload_len,
                      const int* fds, size_t n_fds);

    slash_sysemu_socket_header MakeResponseHeader(
        const slash_sysemu_socket_header& req, int32_t return_value);

    /* Accept loop thread. */
    void AcceptLoop();
    /* Per-connection worker thread. */
    void ConnectionLoop(int conn_fd);

    /* Socket + threads. */
    std::string socket_path_;
    int         listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    /*
     * QDMA XFER socket pairs.
     * When QPAIR_GET_FD is handled, the server creates a socketpair.
     * The server-side fd is stored here (indexed by the qid returned at
     * QPAIR_ADD time, bounded by kMaxQdmaQpairs).  The client-side fd is
     * sent to the client via SCM_RIGHTS.  A session thread reads TRANSFER
     * datagrams from the server-side fd, writes device memory, and responds.
     *
     * xfer_sv[qid][0] = server fd  (owned by session thread)
     * xfer_sv[qid][1] = client fd  (transferred to libslash via SCM_RIGHTS)
     */
    static constexpr int kMaxQdmaQpairs = 16;
    int xfer_sv_[kMaxQdmaQpairs][2];  /* -1 = slot free */

    /* Mutex protecting xfer_sv_ and next_qid (written from connection threads). */
    std::mutex qdma_mtx_;

    /*
     * Device memory size constant for internal use.
     * kDevMemSize must match the initial size assigned to dev_mem_ in Start().
     */
    static constexpr size_t kDevMemSize = 4ULL * 1024 * 1024; /* 4 MiB */

    /* Live connection threads (joined in Stop). */
    std::mutex              workers_mtx_;
    std::vector<std::thread> workers_;

    /* Fault injection. */
    std::mutex  fault_mtx_;
    SysemuFault fault_{SysemuFault::None};
    int         fault_errno_{0};

    /* Request recording. */
    std::mutex                       records_mtx_;
    std::vector<RecordedRequest>     records_;
};
