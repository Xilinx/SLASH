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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace slash_sysemu::test {

// ─────────────────────────────────────────────────────────────────────────────
// Reusable mock vpp_sim model server
// ─────────────────────────────────────────────────────────────────────────────
//
// Binds a ZMQ_REP socket on a caller-supplied endpoint and services the
// address-keyed `vpp_sim` dialect against an in-memory address→byte map.  A
// dedicated service thread runs the REQ/REP loop.  The server is scriptable so
// that tests can drive both the happy path and the client's Transport/Protocol
// error paths:
//
//   * FaultMode selects a per-request misbehaviour (wrong reply, delay, silence,
//     "ERR", malformed JSON, oversized byte, abrupt close).
//   * A "fault countdown" lets a test serve N normal requests and then inject a
//     fault on the (N+1)th, e.g. to test recovery/serialisation.
//   * Every received request is recorded (verb + a coarse "in flight" high-water
//     mark) so tests can assert correct serialisation and ordering.
//
// Shutdown is clean: the loop uses a short poll timeout, the socket has
// LINGER=0, and stop()/the destructor join the thread — keeping ASan/TSan quiet.

enum class FaultMode {
    None,          // normal, correct replies
    WrongReply,    // reply "OK" where a JSON value is expected (and vice-versa)
    ErrReply,      // reply the literal "ERR"
    MalformedJson, // reply non-JSON bytes where JSON is expected
    OversizedByte, // fetch buffer replies with a byte value > 255
    ShortBuffer,   // fetch buffer replies with fewer bytes than requested
    Delay,         // sleep past the client timeout before replying
    Silence,       // never reply (drop the request), forcing a client timeout
    ExtraFrame,    // reply with an extra ZMQ frame (multi-frame reply)
    Close,         // close the socket without replying (dead model)
    NegativeScalar,  // fetch scalar replies a negative JSON integer (-1)
    FloatScalar,     // fetch scalar replies a JSON float (3.5)
    JsonStringReply, // reply a quoted JSON string ("\"OK\"") where a value is expected
    NegativeByte,    // fetch buffer replies with a byte value of -1
    FloatByte,       // fetch buffer replies with a float element (3.5)
    EmptyReply,      // reply with an empty (zero-length) frame
    LongBuffer,      // fetch buffer replies with MORE bytes than requested
};

// A record of one serviced request, for ordering / serialisation assertions.
struct RequestRecord {
    std::string command;     // the "command" field (or "fetch:buffer"/"fetch:scalar")
    std::string raw_frame0;  // the raw JSON of frame 0
    uint64_t    addr = 0;    // decoded addr, if present
    uint64_t    size = 0;    // decoded size, if present
    std::size_t payload_len = 0; // frame 1 length, if present
};

class MockModelServer {
public:
    // Bind and start serving on @p endpoint (e.g. "ipc:///tmp/foo").  Throws
    // std::runtime_error on bind failure (tests treat that as a setup error).
    explicit MockModelServer(const std::string& endpoint);
    ~MockModelServer();

    MockModelServer(const MockModelServer&)            = delete;
    MockModelServer& operator=(const MockModelServer&) = delete;

    // Stop the service thread and release the socket/context.  Idempotent.
    void stop();

    // ── Fault scripting (thread-safe) ────────────────────────────────────────

    // Serve `normal_count` requests normally, then apply `mode` from then on.
    void set_fault(FaultMode mode, int normal_count = 0);

    // Set the delay used by FaultMode::Delay.
    void set_delay(std::chrono::milliseconds delay) { delay_ = delay; }

    // ── Introspection (thread-safe) ──────────────────────────────────────────

    // The endpoint this server is bound to.
    [[nodiscard]] const std::string& endpoint() const { return endpoint_; }

    // Snapshot of all serviced requests, in order.
    [[nodiscard]] std::vector<RequestRecord> requests() const;

    // Number of serviced requests.
    [[nodiscard]] std::size_t request_count() const;

    // Highest number of requests ever observed "in flight" simultaneously.
    // Correct serialisation keeps this at 1.
    [[nodiscard]] int max_in_flight() const { return max_in_flight_.load(); }

    // Read a byte from the in-memory model memory (0 if never written).
    [[nodiscard]] uint8_t peek(uint64_t addr) const;

    // Seed the in-memory memory (e.g. to stage a fetch result).
    void poke(uint64_t addr, uint8_t value);
    void poke_buffer(uint64_t addr, const std::vector<uint8_t>& bytes);

    // Seed a scalar register value returned by `fetch scalar`.
    void set_scalar(uint64_t addr, uint32_t value);

private:
    void run();
    // Dispatch one fully-received request.  Returns the reply frames to send;
    // an empty vector means "send nothing" (silence).  `close_after` requests
    // that the socket be torn down after (or instead of) replying.
    struct Reply {
        std::vector<std::vector<uint8_t>> frames; // 0 frames = stay silent
        bool                              close_after = false;
    };
    Reply dispatch(const std::string& frame0, const std::vector<uint8_t>& frame1, bool has_frame1);

    std::string endpoint_;
    void*       ctx_    = nullptr;
    void*       socket_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex        mu_;
    std::map<uint64_t, uint8_t> memory_;
    std::map<uint64_t, uint32_t> scalars_;
    std::vector<RequestRecord> records_;

    FaultMode                 fault_{FaultMode::None};
    int                       fault_normal_remaining_{0};
    std::chrono::milliseconds delay_{std::chrono::milliseconds(2000)};

    std::atomic<int> in_flight_{0};
    std::atomic<int> max_in_flight_{0};
};

} // namespace slash_sysemu::test
