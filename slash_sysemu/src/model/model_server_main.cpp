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

// vpp_sim — the default system-emulation model process.
//
// The daemon launches this executable (extracted from a VBIN as `vpp_sim`) with
// the ZeroMQ endpoint to bind as argv[1], then drives it over the model dialect.
// It serves round-trip memory/register traffic until the daemon sends the `exit`
// verb or the process is signalled.

#include "model_server.h"

#include <csignal>
#include <cstdio>
#include <exception>

namespace {

// The running server, so the signal handler can ask it to stop.  Only ever set
// once, from main, before any signal can arrive.
slash_sysemu::model::ModelServer* g_server = nullptr;

extern "C" void on_signal(int /*sig*/) {
    if (g_server != nullptr) {
        g_server->stop(); // async-signal-safe: only stores a flag
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <zmq-endpoint>\n", argv[0]);
        return 2;
    }

    try {
        slash_sysemu::model::ModelServer server(argv[1]);
        g_server = &server;

        std::signal(SIGTERM, on_signal);
        std::signal(SIGINT, on_signal);

        server.serve();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vpp_sim: %s\n", e.what());
        return 1;
    }

    return 0;
}
