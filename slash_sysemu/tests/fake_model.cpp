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

// A fake vpp_sim model executable used as a test artifact for Step 6.  It takes
// the ZeroMQ endpoint as argv[1], binds a ZMQ_REP socket, and services the sim
// verbs by reusing the same MockModelServer logic the Step 5 client tests use —
// so a launched fake model behaves exactly like the mock.
//
// A leading option (argv[1]) selects a failure mode; when present the endpoint
// moves to argv[2].  The failure modes drive the ModelProcess launch/death paths:
//
//   (none)             bind + serve normally until `exit` (or killed).
//   --exit-immediately exit(0) before binding — launch must fail (never serves).
//   --never-bind       do NOT bind; sleep forever — client connect probe times out.
//   --hang             bind but never reply — probe `start` times out.
//   --crash-after=N    serve normally, then _exit(139)-style abort after N requests.
//   --exit-after-ms=N  bind + serve, then exit(0) after N ms (drives death detect).
//
// The default (no-flag) model replies "OK" to start/exit/reg/populate and serves
// fetch reads from its in-memory map, so launch's probe `start` succeeds.

#include "mock_model_server.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <unistd.h>

#include <zmq.h>

using slash_sysemu::test::FaultMode;
using slash_sysemu::test::MockModelServer;

namespace {

// Serve a raw ZMQ_REP loop that never replies (for --hang): bind, then just
// sleep, leaving requests unanswered so the client's probe times out.
[[noreturn]] void serve_hang(const std::string& endpoint) {
    void* ctx    = zmq_ctx_new();
    void* socket = zmq_socket(ctx, ZMQ_REP);
    int   linger = 0;
    zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_bind(socket, endpoint.c_str());
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Bind normally and serve, but forcibly crash after N serviced requests.
[[noreturn]] void serve_crash_after(const std::string& endpoint, int n) {
    MockModelServer server(endpoint);
    // Poll the mock's request count; once it reaches N, abort hard.
    for (;;) {
        if (static_cast<int>(server.request_count()) >= n) {
            std::abort(); // SIGABRT: models a model that crashes mid-run.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

// A compile-time default mode lets us build behaviour-specialised variants of
// this one source (packed as `vpp_sim` in a fixture) without needing the daemon
// to pass a flag — the daemon always launches `./vpp_sim <endpoint>`.  When
// FAKE_MODEL_DEFAULT_MODE is defined, argv[1] is treated as the endpoint and the
// baked-in mode applies.  Values: "serve" (default), "exit", "hang".
#ifndef FAKE_MODEL_DEFAULT_MODE
#define FAKE_MODEL_DEFAULT_MODE "serve"
#endif

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }

    const std::string default_mode = FAKE_MODEL_DEFAULT_MODE;
    if (default_mode == "exit") {
        // Valid executable that quits immediately regardless of args: models a
        // VBIN whose model process dies before serving (launch must fail).
        return 0;
    }
    if (default_mode == "hang") {
        // Binds but never replies: the launch probe `start` times out.
        serve_hang(argv[1]);
    }

    std::string first = argv[1];

    // --exit-immediately: quit before doing anything (never binds).
    if (first == "--exit-immediately") {
        return 0;
    }

    // Options that carry the endpoint in argv[2].
    if (first == "--never-bind") {
        // Do not bind at all; sleep forever so the client connect/probe times out
        // against an endpoint no one is serving.
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (first == "--hang") {
        if (argc < 3) return 2;
        serve_hang(argv[2]);
    }
    if (first.rfind("--crash-after=", 0) == 0) {
        if (argc < 3) return 2;
        int n = std::atoi(first.c_str() + std::strlen("--crash-after="));
        serve_crash_after(argv[2], n < 1 ? 1 : n);
    }
    if (first.rfind("--exit-after-ms=", 0) == 0) {
        if (argc < 3) return 2;
        int ms = std::atoi(first.c_str() + std::strlen("--exit-after-ms="));
        {
            MockModelServer server(argv[2]);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms < 0 ? 0 : ms));
            // server stops cleanly on scope exit, then we exit(0).
        }
        return 0;
    }

    // Normal model: argv[1] is the endpoint.  Serve until we receive the `exit`
    // verb (then terminate cleanly, exercising ModelProcess's graceful teardown
    // path) or until we are killed.  The MockModelServer already replies "OK" to
    // exit; we additionally poll its recorded requests and quit once seen.
    const std::string endpoint = first;
    MockModelServer   server(endpoint);
    for (;;) {
        for (const auto& rec : server.requests()) {
            if (rec.command == "exit") {
                return 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}
