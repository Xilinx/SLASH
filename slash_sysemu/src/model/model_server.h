// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace slash_sysemu::model {

// ─────────────────────────────────────────────────────────────────────────────
// ModelServer — the default "vpp_sim" model process
// ─────────────────────────────────────────────────────────────────────────────
//
// A minimal system-emulation model: it binds a ZMQ_REP socket on the endpoint
// the daemon passes as argv[1] and services the model dialect against an
// in-memory address→byte store.  It supports round-trip BAR/HBM/DDR reads and
// writes plus register scalars, but runs no compute kernels. It thus acts as
// a "default" model to use before an accelerator has been reconfigured by a user.
// It is built from source as part of the daemon and packaged into `default.vbin`
// (installed to <prefix>/lib/slash-sysemu/default.vbin); the daemon copies it
// into a fresh accelerator's main.vbin at bootstrap.  The path is overridable via
// configuration (see DaemonConfig::default_vbin_path).
//
// The dialect (frame 0 is a JSON command; `populate` carries a second raw byte
// frame) is the same one the daemon's ModelClient speaks and the test
// MockModelServer emulates; the happy paths are pinned to each other by the
// model_client tests and the daemon integration test that boots this binary.
class ModelServer {
public:
    // Bind a ZMQ_REP socket on @p endpoint (e.g. "ipc:///run/.../zmq.socket").
    // Throws std::runtime_error on any ZMQ setup/bind failure.
    explicit ModelServer(const std::string& endpoint);
    ~ModelServer();

    ModelServer(const ModelServer&)            = delete;
    ModelServer& operator=(const ModelServer&) = delete;

    // Serve requests on the calling thread until an `exit` verb is received or
    // stop() is called.  Returns once the loop observes either condition.
    void serve();

    // Ask serve() to return at the next poll.  Async-signal-safe (only stores a
    // flag), so it may be called from a signal handler.
    void stop() noexcept;

private:
    // Service one fully-received request; returns the reply frames to send
    // (empty ⇒ send nothing).  Sets @ref exit_requested_ on the `exit` verb.
    std::vector<std::vector<uint8_t>> handle(const std::string&          frame0,
                                             const std::vector<uint8_t>& frame1,
                                             bool                        has_frame1);

    std::string endpoint_;
    void*       ctx_    = nullptr;
    void*       socket_ = nullptr;

    std::atomic<bool> running_{false};       // cleared by stop()
    bool              exit_requested_{false}; // set by the `exit` verb

    std::map<uint64_t, uint8_t>  memory_;
    std::map<uint64_t, uint32_t> scalars_;
};

} // namespace slash_sysemu::model
