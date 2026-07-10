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

#include "transport.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace slash_sysemu {

// ─────────────────────────────────────────────────────────────────────────────
// ZeroMQ model client (vpp_sim address-keyed dialect)
// ─────────────────────────────────────────────────────────────────────────────
//
// A ModelClient owns a ZeroMQ context and a single ZMQ_REQ socket connected to
// one model process, and speaks the address-keyed `vpp_sim` dialect of the model
// protocol documented in docs/reference/model-protocol/index.rst.
//
// SCOPE: only the address-keyed `vpp_sim` (FPGA-simulation) dialect is supported.
// FPGA *emulation* models (`vpp_emu`) are out of scope for this sprint — they
// provide no way to asynchronously check kernel state, which the polling worker
// design relies on — so only simulation VBINs are launched.
//
// The `vpp_sim` model binds a ZMQ_REP endpoint (an ipc:// AF_UNIX path, or a
// tcp:// endpoint); this client connects to it.  The REQ/REP pair is strict
// lock-step: exactly one request may be in flight at a time.  ZeroMQ sockets are
// additionally not thread-safe.  We therefore guard the whole send→recv cycle
// with a std::mutex, which both serialises concurrent callers (each gets its own
// correct reply) and provides a queue of waiting threads.
//
// Error taxonomy (reusing transport.h's Result / ErrorKind):
//   * ErrorKind::Transport — the request could not be delivered or no reply was
//     received within the timeout: send/recv OS failure, timeout, dead or closed
//     socket.  Downstream this maps to -ENODEV ("model assumed dead"), which
//     tears down the accelerator.
//   * ErrorKind::Protocol  — a reply was received but was malformed or of an
//     unexpected shape: not "OK" where "OK" was expected, "ERR", non-JSON, wrong
//     JSON type, a byte value outside 0..255, or a buffer whose length does not
//     match the requested size.
//
// The reference client (vrt ZmqServer) throws on these conditions; this client
// never throws across its API — every operation returns a Result.  If the
// underlying libzmq call fails it is converted to a Transport error at the
// boundary.

/** Default per-request timeout.  Short enough that a dead model is noticed
 *  promptly; the daemon issues no genuinely blocking model requests. */
inline constexpr std::chrono::milliseconds kDefaultModelTimeout{10000};

class ModelClient {
public:
    /**
     * @brief Connect a new client to a model endpoint.
     *
     * Creates a ZeroMQ context and a ZMQ_REQ socket, sets the send/recv
     * timeouts and LINGER=0, and connects to @p endpoint.  The model process is
     * expected to have BOUND the endpoint (ordering does not matter for
     * ZeroMQ connect, but a reply will only arrive once a peer is bound).
     *
     * @param endpoint  A ZeroMQ endpoint: `ipc://<path>` (an AF_UNIX socket) or
     *                  `tcp://host:port`.
     * @param timeout   Per-request timeout applied to both send and receive.
     * @return Result<ModelClient> on success, or ErrorKind::Transport if the
     *         context/socket could not be created or configured.
     */
    static Result<ModelClient> connect(const std::string&        endpoint,
                                       std::chrono::milliseconds timeout = kDefaultModelTimeout);

    ModelClient(const ModelClient&)            = delete;
    ModelClient& operator=(const ModelClient&) = delete;
    ModelClient(ModelClient&& o) noexcept;
    ModelClient& operator=(ModelClient&& o) noexcept;
    ~ModelClient();

    // ── vpp_sim verbs ────────────────────────────────────────────────────────
    //
    // Calling any verb below on a moved-from client (its context/socket have been
    // transferred away) or a client whose connection has otherwise been closed is
    // DEFINED behavior, not UB: the verb returns an ErrorKind::Transport error
    // ("model client is not connected") without dereferencing the null socket or
    // locking a null mutex.  This is the same classification a dead model gets, so
    // downstream (-ENODEV) handling is uniform.  See ModelClient::transact.

    /** `start` (global): start the simulation clock/driver.  Expects "OK". */
    Result<void> start();

    /** `exit`: tear down the model.  The model replies "OK" then terminates. */
    Result<void> exit();

    /** `reg`: 32-bit AXI-Lite register write of @p val at @p addr.  Expects "OK". */
    Result<void> reg_write(uint64_t addr, uint32_t val);

    /**
     * `populate`: host-to-device write of @p data at device address @p addr.
     * Sent as two frames (JSON command + raw bytes).  Expects "OK".
     *
     * INVARIANT: the JSON `size` field is always set to `data.size()` — the two
     * MUST stay lockstep.  The real `vpp_sim` reads exactly `size` bytes out of
     * frame 1 regardless of the frame's actual length, so a `size` larger than
     * the payload would make the model read out of bounds.  Do not decouple them.
     */
    Result<void> populate(uint64_t addr, std::span<const uint8_t> data);

    /**
     * `fetch buffer`: device-to-host read of @p size bytes from @p addr.
     * The reply is a JSON array of byte-sized integers (0..255).
     *
     * ERROR TAXONOMY (matters for the QDMA C2H path): a well-delivered
     * but malformed reply is an ErrorKind::Protocol error, NOT Transport — this
     * includes a byte value outside 0..255, a non-integer element, a non-array
     * reply, and an array whose length != @p size (a short OR long read).  Only a
     * timeout or a closed/dead socket is ErrorKind::Transport (→ -ENODEV, "model
     * dead").  So a length mismatch signals a protocol/model bug, not a missing
     * device, and callers must not translate it to -ENODEV.
     */
    Result<std::vector<uint8_t>> fetch_buffer(uint64_t addr, uint64_t size);

    /**
     * `fetch scalar`: read a 32-bit register/scalar at @p addr.  The reply is a
     * JSON unsigned integer; a value that does not fit in 32 bits, or a
     * non-integer reply, is a Protocol error.
     */
    Result<uint32_t> fetch_scalar(uint64_t addr);

    /** The endpoint this client is connected to. */
    [[nodiscard]] const std::string& endpoint() const noexcept { return endpoint_; }

    // A default-constructed client owns no context or socket.  It exists so that
    // Result<ModelClient> can hold the moved-into slot; use connect() to obtain a
    // usable client.
    ModelClient() = default;

private:

    // Send a single-frame JSON request and receive the single-frame reply, or
    // send a two-frame (JSON + payload) request.  Holds the socket mutex for the
    // whole cycle.  Returns the raw reply bytes or a Transport error.
    Result<std::vector<uint8_t>> transact(const std::string&       frame0,
                                          std::span<const uint8_t> frame1,
                                          bool                     has_frame1);

    // Non-locking primitives; callers must hold mutex_.
    Result<void>                 send_frames(const std::string&       frame0,
                                             std::span<const uint8_t> frame1,
                                             bool                     has_frame1);
    Result<std::vector<uint8_t>> recv_reply();

    void close() noexcept;

    void*       ctx_    = nullptr; // zmq context
    void*       socket_ = nullptr; // ZMQ_REQ socket
    std::string endpoint_;
    // Guards the whole send→recv cycle: ZMQ REQ forbids overlapping requests and
    // the socket itself is not thread-safe.  A unique_ptr so the client stays
    // movable (a std::mutex is neither movable nor copyable).
    std::unique_ptr<std::mutex> mutex_ = std::make_unique<std::mutex>();
};

} // namespace slash_sysemu
