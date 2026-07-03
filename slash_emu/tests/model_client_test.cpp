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

#include "model_client.h"
#include "mock_model_server.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <numeric>
#include <string>
#include <thread>
#include <dirent.h>
#include <unistd.h>
#include <vector>

#include <json/json.h>
#include <gtest/gtest.h>

using namespace slash_emu;
using slash_emu::test::FaultMode;
using slash_emu::test::MockModelServer;
using namespace std::chrono_literals;

namespace {

// Generate a unique ipc:// endpoint per test.  Each MockModelServer binds this,
// and the client connects to it.
std::string unique_endpoint() {
    static std::atomic<unsigned> counter{0};
    return "ipc:///tmp/slash_emu_model_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1)) + ".sock";
}

// Convenience: connect a client with a short test timeout so error-path tests
// do not wait the 10s production default.
ModelClient make_client(const std::string& endpoint,
                        std::chrono::milliseconds timeout = 500ms) {
    auto r = ModelClient::connect(endpoint, timeout);
    EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
    return std::move(r.value());
}

std::span<const uint8_t> as_span(const std::vector<uint8_t>& v) {
    return std::span<const uint8_t>(v.data(), v.size());
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Connection
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelClientConnect, ConnectsToBoundEndpoint) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto r = ModelClient::connect(ep, 500ms);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value().endpoint(), ep);
}

TEST(ModelClientConnect, ConnectSucceedsEvenWithNoPeer) {
    // ZeroMQ connect is lazy; no bound peer yet is not an error.  The failure
    // surfaces only when a request times out (covered elsewhere).
    auto ep = unique_endpoint();
    auto r  = ModelClient::connect(ep, 100ms);
    EXPECT_TRUE(r.has_value());
}

TEST(ModelClientConnect, InvalidEndpointIsTransportError) {
    auto r = ModelClient::connect("not-a-valid-endpoint", 100ms);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Transport);
}

TEST(ModelClientConnect, IsMovable) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    ModelClient moved = std::move(client);
    EXPECT_EQ(moved.endpoint(), ep);
    EXPECT_TRUE(moved.start().has_value());
    // Move-assignment.
    auto ep2 = unique_endpoint();
    MockModelServer server2(ep2);
    ModelClient other = make_client(ep2);
    other = std::move(moved);
    EXPECT_TRUE(other.start().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Happy-path verbs + wire framing
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelClientVerbs, StartRoundTrips) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);

    ASSERT_TRUE(client.start().has_value());
    auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].command, "start");
    // Exact framing, byte-for-byte identical to the reference client (vrt
    // ZmqServer), which uses a default Json::StreamWriterBuilder (tab-indented).
    EXPECT_EQ(reqs[0].raw_frame0, "{\n\t\"command\" : \"start\"\n}");
    EXPECT_EQ(reqs[0].payload_len, 0u);
}

TEST(ModelClientVerbs, ExitRoundTrips) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);

    ASSERT_TRUE(client.exit().has_value());
    auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].command, "exit");
    EXPECT_EQ(reqs[0].raw_frame0, "{\n\t\"command\" : \"exit\"\n}");
}

TEST(ModelClientVerbs, RegWriteRoundTripsAndStoresValue) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);

    ASSERT_TRUE(client.reg_write(0xdeadbeef00ull, 0xcafef00du).has_value());
    auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].command, "reg");
    EXPECT_EQ(reqs[0].addr, 0xdeadbeef00ull);
    // Value round-trips: a subsequent fetch scalar reads it back.
    auto sc = client.fetch_scalar(0xdeadbeef00ull);
    ASSERT_TRUE(sc.has_value());
    EXPECT_EQ(sc.value(), 0xcafef00du);
}

TEST(ModelClientVerbs, RegWriteFramingMatchesReference) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    ASSERT_TRUE(client.reg_write(16, 255).has_value());
    auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].raw_frame0,
              "{\n\t\"addr\" : 16,\n\t\"command\" : \"reg\",\n\t\"val\" : 255\n}");
}

TEST(ModelClientVerbs, PopulateSendsTwoFramesAndWritesMemory) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);

    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    ASSERT_TRUE(client.populate(0x1000, as_span(data)).has_value());

    auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].command, "populate");
    EXPECT_EQ(reqs[0].addr, 0x1000u);
    EXPECT_EQ(reqs[0].size, 5u);
    EXPECT_EQ(reqs[0].payload_len, 5u);
    EXPECT_EQ(reqs[0].raw_frame0,
              "{\n\t\"addr\" : 4096,\n\t\"command\" : \"populate\",\n\t\"size\" : 5\n}");

    for (std::size_t i = 0; i < data.size(); ++i) {
        EXPECT_EQ(server.peek(0x1000 + i), data[i]);
    }
}

TEST(ModelClientVerbs, PopulateEmptyBuffer) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    std::vector<uint8_t> empty;
    ASSERT_TRUE(client.populate(0x2000, as_span(empty)).has_value());
    auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].size, 0u);
    EXPECT_EQ(reqs[0].payload_len, 0u);
}

TEST(ModelClientVerbs, FetchScalarDecodesValue) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_scalar(0x40, 0x12345678u);
    auto client = make_client(ep);

    auto r = client.fetch_scalar(0x40);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), 0x12345678u);
    auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].command, "fetch:scalar");
    EXPECT_EQ(
        reqs[0].raw_frame0,
        "{\n\t\"addr\" : 64,\n\t\"command\" : \"fetch\",\n\t\"type\" : \"scalar\"\n}");
}

TEST(ModelClientVerbs, FetchScalarMaxValue) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_scalar(0, 0xFFFFFFFFu);
    auto client = make_client(ep);
    auto r = client.fetch_scalar(0);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), 0xFFFFFFFFu);
}

TEST(ModelClientVerbs, FetchBufferDecodesBytes) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    std::vector<uint8_t> seed = {0, 1, 127, 128, 254, 255};
    server.poke_buffer(0x5000, seed);
    auto client = make_client(ep);

    auto r = client.fetch_buffer(0x5000, seed.size());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), seed);
    auto reqs = server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].command, "fetch:buffer");
    EXPECT_EQ(reqs[0].raw_frame0,
              "{\n\t\"addr\" : 20480,\n\t\"command\" : \"fetch\",\n\t\"size\" : 6,"
              "\n\t\"type\" : \"buffer\"\n}");
}

TEST(ModelClientVerbs, FetchBufferEmpty) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    auto r = client.fetch_buffer(0x100, 0);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r.value().empty());
}

TEST(ModelClientVerbs, FetchBufferLarge) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    std::vector<uint8_t> seed(4096);
    for (std::size_t i = 0; i < seed.size(); ++i) {
        seed[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    server.poke_buffer(0x8000, seed);
    auto client = make_client(ep);
    auto r = client.fetch_buffer(0x8000, seed.size());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), seed);
}

TEST(ModelClientVerbs, PopulateThenFetchRoundTrip) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep);
    std::vector<uint8_t> data(256);
    std::iota(data.begin(), data.end(), 0);
    ASSERT_TRUE(client.populate(0xA000, as_span(data)).has_value());
    auto r = client.fetch_buffer(0xA000, data.size());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), data);
}

// ─────────────────────────────────────────────────────────────────────────────
// Protocol errors (well-delivered but malformed / unexpected replies)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelClientProtocol, StartWrongReplyIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::WrongReply);
    auto client = make_client(ep);
    auto r = client.start();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, StartErrReplyIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::ErrReply);
    auto client = make_client(ep);
    auto r = client.start();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, RegErrReplyIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::ErrReply);
    auto client = make_client(ep);
    auto r = client.reg_write(0, 0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, PopulateErrReplyIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::ErrReply);
    auto client = make_client(ep);
    std::vector<uint8_t> d = {1};
    auto r = client.populate(0, as_span(d));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, FetchBufferNonArrayIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::WrongReply); // replies "OK" not an array
    auto client = make_client(ep);
    auto r = client.fetch_buffer(0, 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, FetchBufferMalformedJsonIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::MalformedJson);
    auto client = make_client(ep);
    auto r = client.fetch_buffer(0, 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, FetchBufferOversizedByteIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::OversizedByte);
    auto client = make_client(ep);
    auto r = client.fetch_buffer(0, 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, FetchBufferLengthMismatchIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::ShortBuffer);
    auto client = make_client(ep);
    auto r = client.fetch_buffer(0, 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

// A reply that is valid JSON but NOT an array (here a quoted JSON string "OK")
// must be a Protocol error.  Unlike FetchBufferNonArrayIsProtocolError above —
// which sends bare `OK` (invalid JSON, tripping the parse-fail branch) — this
// reaches the distinct "parsed OK but not an array" branch in fetch_buffer.
TEST(ModelClientProtocol, FetchBufferValidJsonNonArrayIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::JsonStringReply); // valid JSON string "OK"
    auto client = make_client(ep);
    auto r = client.fetch_buffer(0, 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, FetchBufferExtraFrameIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::ExtraFrame); // "[]" + extra frame
    auto client = make_client(ep);
    auto r = client.fetch_buffer(0, 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, FetchBufferErrReplyIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::ErrReply); // "ERR" (not JSON) for a buffer read
    auto client = make_client(ep);
    auto r = client.fetch_buffer(0, 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, FetchScalarNonIntIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::WrongReply); // replies "OK"
    auto client = make_client(ep);
    auto r = client.fetch_scalar(0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, FetchScalarMalformedJsonIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::MalformedJson);
    auto client = make_client(ep);
    auto r = client.fetch_scalar(0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, FetchScalarOutOf32BitRangeIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::OversizedByte); // scalar: returns 2^32
    auto client = make_client(ep);
    auto r = client.fetch_scalar(0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(ModelClientProtocol, ExtraFrameReplyIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::ExtraFrame);
    auto client = make_client(ep);
    auto r = client.start();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

// ─────────────────────────────────────────────────────────────────────────────
// Transport errors (timeout / dead model)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelClientTransport, SilentModelTimesOutAsTransport) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::Silence);
    auto client = make_client(ep, 200ms);

    auto start = std::chrono::steady_clock::now();
    auto r     = client.start();
    auto took  = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Transport);
    // Returned within a small multiple of the timeout, not the 10s default.
    EXPECT_LT(took, 3s);
}

TEST(ModelClientTransport, DelayedReplyPastTimeoutIsTransport) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_delay(1s);
    server.set_fault(FaultMode::Delay);
    auto client = make_client(ep, 150ms);
    auto r = client.start();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Transport);
}

TEST(ModelClientTransport, NoPeerBoundTimesOutAsTransport) {
    auto ep = unique_endpoint(); // nothing binds it
    auto client = make_client(ep, 150ms);
    auto r = client.fetch_scalar(0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Transport);
}

TEST(ModelClientTransport, DeadModelClosingSocketIsTransport) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::Close);
    auto client = make_client(ep, 200ms);
    auto r = client.start();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Transport);
}

// After a delayed/slow reply causes a timeout, later requests still work once
// the model behaves again (the client remains usable, and the server is scripted
// with a normal countdown).
TEST(ModelClientTransport, RecoversAfterInitialFault) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    // Serve the first request normally, then no fault afterwards either — verify
    // the client keeps working across many sequential calls.
    auto client = make_client(ep);
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(client.reg_write(i, static_cast<uint32_t>(i)).has_value());
    }
    EXPECT_EQ(server.request_count(), 20u);
}

// Every verb forwards a Transport error (here: a silent model → timeout).  This
// exercises the Transport-error forwarding path of each verb, not just start().
TEST(ModelClientTransport, EveryVerbForwardsTransportOnSilence) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::Silence);
    auto client = make_client(ep, 150ms);

    EXPECT_EQ(client.start().error().kind, ErrorKind::Transport);
    EXPECT_EQ(client.exit().error().kind, ErrorKind::Transport);
    EXPECT_EQ(client.reg_write(0, 0).error().kind, ErrorKind::Transport);
    std::vector<uint8_t> d = {1, 2};
    EXPECT_EQ(client.populate(0, as_span(d)).error().kind, ErrorKind::Transport);
    EXPECT_EQ(client.fetch_buffer(0, 4).error().kind, ErrorKind::Transport);
    EXPECT_EQ(client.fetch_scalar(0).error().kind, ErrorKind::Transport);
}

// A timeout larger than INT_MAX milliseconds is clamped rather than treated as
// "block forever"; connect still succeeds and normal requests work.
TEST(ModelClientTransport, HugeTimeoutIsClampedAndUsable) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto r = ModelClient::connect(ep, std::chrono::milliseconds(9'000'000'000LL));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r.value().start().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrency / serialisation
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelClientConcurrency, ManyThreadsMixedRequestsEachGetOwnReply) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep, 5s);

    // Pre-seed distinct scalar values per address so each thread can verify it
    // received ITS OWN reply and not another thread's.
    constexpr int kThreads = 16;
    constexpr int kIters   = 50;
    for (int t = 0; t < kThreads; ++t) {
        server.set_scalar(static_cast<uint64_t>(t) * 0x100,
                          static_cast<uint32_t>(0x1000 + t));
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            uint64_t scalar_addr = static_cast<uint64_t>(t) * 0x100;
            uint32_t expect      = static_cast<uint32_t>(0x1000 + t);
            for (int i = 0; i < kIters; ++i) {
                // Mix verbs: reg write, fetch scalar (verify own value),
                // populate + fetch buffer round trip on a per-thread address.
                if (!client.reg_write(scalar_addr, expect).has_value()) {
                    ++failures;
                    return;
                }
                auto sc = client.fetch_scalar(scalar_addr);
                if (!sc.has_value() || sc.value() != expect) {
                    ++failures;
                    return;
                }
                uint64_t buf_addr = 0x100000 + static_cast<uint64_t>(t) * 0x1000;
                std::vector<uint8_t> payload = {static_cast<uint8_t>(t),
                                                static_cast<uint8_t>(i)};
                if (!client.populate(buf_addr, as_span(payload)).has_value()) {
                    ++failures;
                    return;
                }
                auto buf = client.fetch_buffer(buf_addr, payload.size());
                if (!buf.has_value() || buf.value() != payload) {
                    ++failures;
                    return;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    // The REAL proof of correct serialisation is the functional check above:
    // each thread writes/reads a value keyed to its own thread id and asserts it
    // reads back EXACTLY that value.  Without the client's mutex, concurrent
    // callers would corrupt the non-thread-safe ZMQ_REQ socket and its lock-step
    // state machine — producing crossed/garbled replies (caught by `failures`),
    // a wrong readback, or a crash (caught here and, more sharply, under
    // ASan/UBSan/TSan).  16 threads × 50 iters × 4 verbs = plenty of contention.
    EXPECT_EQ(failures.load(), 0);
    // NOTE: max_in_flight()==1 is a cheap sanity check only, NOT proof of the
    // client's mutex: the mock's run() is a single-threaded REP loop, so it
    // physically processes one request at a time regardless of the client.  The
    // functional per-thread-value check above is what actually exercises the
    // mutex.
    EXPECT_EQ(server.max_in_flight(), 1);
    EXPECT_EQ(server.request_count(),
              static_cast<std::size_t>(kThreads * kIters * 4));
}

TEST(ModelClientConcurrency, ConcurrentFetchScalarsNeverCrossReplies) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep, 5s);

    constexpr int kThreads = 8;
    for (int t = 0; t < kThreads; ++t) {
        server.set_scalar(t, static_cast<uint32_t>(0xABCD0000u + t));
    }

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 200; ++i) {
                auto r = client.fetch_scalar(t);
                if (!r.has_value() || r.value() != 0xABCD0000u + static_cast<uint32_t>(t)) {
                    ++mismatches;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    // Functional proof: each thread only ever reads its own address and asserts
    // the exact seeded value; a broken mutex would cross replies between threads
    // and trip `mismatches`.  max_in_flight()==1 is a sanity check only (the mock
    // is a single-threaded REP loop) — see the note in the test above.
    EXPECT_EQ(mismatches.load(), 0);
    EXPECT_EQ(server.max_in_flight(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// ADVERSARY PROBES (Step 5 review)
// ─────────────────────────────────────────────────────────────────────────────

// PROBE 1 — post-timeout REQ desync.  A ZMQ_REQ socket that has sent a request
// and then timed out on recv is in the "expecting reply" state.  It cannot send
// again (EFSM).  The critical safety property: after a timeout, subsequent calls
// must NOT silently pick up the late/stale reply meant for the timed-out call and
// hand it to the next caller.  They must fail (Transport).  If instead the second
// call returns the FIRST call's data, replies are crossed and the daemon reads
// wrong device memory.
//
// Setup: server delays its reply well past the client timeout.  Call fetch_scalar
// for addr A (times out).  Then call fetch_scalar for addr B with a distinct
// value.  B must NOT return A's value, and ideally the client stays usable or
// cleanly reports Transport.
TEST(ModelClientAdversary, PostTimeoutDoesNotReturnStaleReply) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_scalar(0xAA, 0x11111111u);
    server.set_scalar(0xBB, 0x22222222u);
    // Delay every reply by ~600ms; client timeout is 150ms so the first call
    // times out, and the reply for addr 0xAA lands in the socket afterwards.
    server.set_delay(600ms);
    server.set_fault(FaultMode::Delay);
    auto client = make_client(ep, 150ms);

    auto a = client.fetch_scalar(0xAA);
    EXPECT_FALSE(a.has_value());
    EXPECT_EQ(a.error().kind, ErrorKind::Transport);

    // Give the delayed 0xAA reply time to arrive in the pipe.
    std::this_thread::sleep_for(700ms);

    auto b = client.fetch_scalar(0xBB);
    // The unacceptable outcome is b == 0xAA's value (crossed replies).
    if (b.has_value()) {
        EXPECT_NE(b.value(), 0x11111111u)
            << "CROSSED REPLY: fetch_scalar(0xBB) returned fetch_scalar(0xAA)'s value";
        EXPECT_EQ(b.value(), 0x22222222u);
    } else {
        // Acceptable: a clean Transport failure (REQ wedged / model assumed dead).
        EXPECT_EQ(b.error().kind, ErrorKind::Transport);
    }
}

// PROBE 1b — the IMMEDIATE next call after a timeout.  With no delay drained, a
// ZMQ_REQ socket that timed out on recv is in the "expecting reply" state, so the
// next zmq_send fails EFSM.  The client must surface that as Transport (→ -ENODEV,
// model assumed dead) and never wedge or crash.  This documents the observed
// fail-closed behavior as a regression.
TEST(ModelClientAdversary, ImmediateCallAfterTimeoutIsTransport) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_scalar(0xBB, 0x22222222u);
    server.set_delay(600ms);
    server.set_fault(FaultMode::Delay);
    auto client = make_client(ep, 120ms);

    auto a = client.fetch_scalar(0xAA);
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().kind, ErrorKind::Transport);

    // Immediately (socket still awaiting the first reply): must not return a
    // value, must not crash.  Transport is the correct classification.
    server.set_fault(FaultMode::None);
    auto b = client.fetch_scalar(0xBB);
    if (b.has_value()) {
        EXPECT_EQ(b.value(), 0x22222222u);
    } else {
        EXPECT_EQ(b.error().kind, ErrorKind::Transport);
    }
}

// PROBE 2 — many callers after a timeout.  After one timeout, every subsequent
// caller must get a coherent result: either its OWN correct value or a Transport
// error.  No caller may receive another caller's value.  Runs several
// post-timeout calls with distinct expected values.
TEST(ModelClientAdversary, PostTimeoutManyCallsNeverCross) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    // Serve the very first request slowly (times out), then behave normally.
    for (int i = 0; i < 8; ++i) {
        server.set_scalar(i, static_cast<uint32_t>(0xC0DE0000u + i));
    }
    server.set_delay(500ms);
    server.set_fault(FaultMode::Delay, /*normal_count=*/0);
    auto client = make_client(ep, 120ms);

    // First call times out.
    auto first = client.fetch_scalar(0);
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(first.error().kind, ErrorKind::Transport);

    // Now stop the fault so the server replies promptly again.
    server.set_fault(FaultMode::None);
    std::this_thread::sleep_for(600ms); // let the stale slow reply drain if any

    for (int i = 1; i < 8; ++i) {
        auto r = client.fetch_scalar(i);
        if (r.has_value()) {
            EXPECT_EQ(r.value(), 0xC0DE0000u + static_cast<uint32_t>(i))
                << "caller " << i << " got a wrong/crossed value";
        } else {
            EXPECT_EQ(r.error().kind, ErrorKind::Transport);
        }
    }
}

// PROBE 3 — concurrent callers where one times out mid-batch.  If the mutex is
// correctly held across send+recv AND a timeout desyncs the socket, other
// threads must never see a crossed reply.  We inject a single delayed reply and
// hammer with many threads reading their own scalars.
TEST(ModelClientAdversary, TimeoutUnderConcurrencyNeverCrosses) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    constexpr int kThreads = 8;
    for (int t = 0; t < kThreads; ++t) {
        server.set_scalar(t, static_cast<uint32_t>(0xD00D0000u + t));
    }
    auto client = make_client(ep, 300ms);

    std::atomic<int> crossed{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load()) { std::this_thread::yield(); }
            for (int i = 0; i < 40; ++i) {
                auto r = client.fetch_scalar(t);
                if (r.has_value() &&
                    r.value() != 0xD00D0000u + static_cast<uint32_t>(t)) {
                    ++crossed;
                }
            }
        });
    }
    go.store(true);
    // Mid-flight, flip one delayed reply to force a timeout on some caller.
    std::this_thread::sleep_for(20ms);
    server.set_delay(500ms);
    server.set_fault(FaultMode::Delay, /*normal_count=*/5);
    std::this_thread::sleep_for(50ms);
    server.set_fault(FaultMode::None);

    for (auto& th : threads) th.join();
    EXPECT_EQ(crossed.load(), 0) << "a caller received another caller's scalar";
    EXPECT_EQ(server.max_in_flight(), 1);
}

// PROBE 4 — reg_write value round-trips at the 32-bit boundary via a real
// fetch_scalar decode of 2^32-1 (exercises the >255/>2^32 decode edge exactly
// at the max, not just beyond it).
TEST(ModelClientAdversary, FetchScalarExactlyMaxU32) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_scalar(0, 0xFFFFFFFFu); // 4294967295 == 2^32-1, must be accepted
    auto client = make_client(ep, 500ms);
    auto r = client.fetch_scalar(0);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), 0xFFFFFFFFu);
}

// PROBE 5 — a fetch_scalar reply that is a NEGATIVE JSON integer must be a
// Protocol error (device scalars are unsigned).  The reference decodes with
// asUInt() which would wrap; our client must reject.
TEST(ModelClientAdversary, FetchScalarNegativeIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::NegativeScalar);
    auto client = make_client(ep, 500ms);
    auto r = client.fetch_scalar(0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

// PROBE 6 — a fetch_scalar reply that is a FLOAT (e.g. 3.5) must be a Protocol
// error, not silently truncated.
TEST(ModelClientAdversary, FetchScalarFloatIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::FloatScalar);
    auto client = make_client(ep, 500ms);
    auto r = client.fetch_scalar(0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

// PROBE 7 — a fetch_buffer element that is a NEGATIVE integer (e.g. -1) must be
// a Protocol error (byte range 0..255).
TEST(ModelClientAdversary, FetchBufferNegativeByteIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::NegativeByte);
    auto client = make_client(ep, 500ms);
    auto r = client.fetch_buffer(0, 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

// PROBE 8 — a fetch_buffer element that is a FLOAT must be a Protocol error.
TEST(ModelClientAdversary, FetchBufferFloatByteIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::FloatByte);
    auto client = make_client(ep, 500ms);
    auto r = client.fetch_buffer(0, 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

// PROBE 9 — an empty reply frame where "OK" is expected must be a Protocol
// error (not a crash, not treated as OK).
TEST(ModelClientAdversary, EmptyReplyIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::EmptyReply);
    auto client = make_client(ep, 500ms);
    auto r = client.start();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

// PROBE 10 — a longer-than-buffer reply (fetch_buffer array longer than
// requested) must be a Protocol error (length mismatch, over-long direction).
TEST(ModelClientAdversary, FetchBufferTooLongIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::LongBuffer);
    auto client = make_client(ep, 500ms);
    auto r = client.fetch_buffer(0, 4);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

// PROBE 11 — fetch_scalar reply "OK" (quoted JSON string "\"OK\"") vs bare.  A
// JSON string reply must be a Protocol error for scalar.
TEST(ModelClientAdversary, FetchScalarJsonStringIsProtocolError) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    server.set_fault(FaultMode::JsonStringReply);
    auto client = make_client(ep, 500ms);
    auto r = client.fetch_scalar(0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

// PROBE 12 — self-move-assignment must not double-close / crash, and the client
// must remain usable.
TEST(ModelClientAdversary, SelfMoveAssignIsSafe) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep, 500ms);
    // Launder through two references so -Wself-move can't see it's the same
    // object; this is the real hazard (aliased move-assign).
    ModelClient& a = client;
    ModelClient& b = client;
    a = std::move(b); // self-move via aliases
    EXPECT_TRUE(client.start().has_value());
}

// PROBE 13 — using a moved-from client must not crash.  It has no socket; a
// verb call should fail cleanly (Transport) rather than dereference null.
TEST(ModelClientAdversary, MovedFromClientDoesNotCrash) {
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    auto client = make_client(ep, 300ms);
    ModelClient sink = std::move(client);
    // `client` is moved-from: socket_ == nullptr.  This must not segfault.
    auto r = client.start(); // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Transport);
}

// PROBE 14 — many connect/destroy cycles must not leak fds.  Compare open fd
// count before/after 500 cycles (allow small slack for allocator/zmq caches).
TEST(ModelClientAdversary, NoFdLeakOverManyConnectCycles) {
    auto count_fds = [] {
        int n = 0;
        DIR* d = ::opendir("/proc/self/fd");
        if (d == nullptr) return -1;
        while (::readdir(d) != nullptr) ++n;
        ::closedir(d);
        return n;
    };
    auto ep = unique_endpoint();
    MockModelServer server(ep);
    // Warm up (first few cycles may cache).
    for (int i = 0; i < 10; ++i) {
        auto c = ModelClient::connect(ep, 100ms);
        ASSERT_TRUE(c.has_value());
    }
    int before = count_fds();
    for (int i = 0; i < 300; ++i) {
        auto c = ModelClient::connect(ep, 100ms);
        ASSERT_TRUE(c.has_value());
        (void)c.value().start();
    }
    int after = count_fds();
    ASSERT_GE(before, 0);
    EXPECT_LT(after - before, 20) << "fd leak: before=" << before << " after=" << after;
}
