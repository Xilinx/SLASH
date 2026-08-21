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

#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <json/json.h>
#include <zmq.h>

namespace slash_sysemu {

namespace {

Result<std::vector<uint8_t>> transport_err(const std::string& what) {
    return Result<std::vector<uint8_t>>::err({ErrorKind::Transport, what});
}

// Compose the "<what>: <zmq_strerror(errno)>" message used for libzmq failures.
std::string zmq_reason(const char* what) {
    return std::string(what) + ": " + zmq_strerror(zmq_errno());
}

// Serialise a jsoncpp value to a frame-0 command string, byte-for-byte
// identical to the reference client (vrt ZmqServer), which uses a default
// Json::StreamWriterBuilder.  In jsoncpp 1.9.x that default emits pretty-printed
// (tab-indented, multi-line) JSON — the exact bytes a real vpp_sim receives from
// the runtime.  The model parses frame 0 with a tolerant Json::Reader, so key
// order / whitespace do not affect interoperability, but matching the reference
// keeps the wire bytes identical.
std::string to_command_json(const Json::Value& cmd) {
    Json::StreamWriterBuilder builder;
    return Json::writeString(builder, cmd);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

Result<ModelClient> ModelClient::connect(const std::string&        endpoint,
                                         std::chrono::milliseconds timeout) {
    ModelClient client;
    client.endpoint_ = endpoint;

    client.ctx_ = zmq_ctx_new();
    if (client.ctx_ == nullptr) {
        return Result<ModelClient>::err({ErrorKind::Transport, zmq_reason("zmq_ctx_new")});
    }

    client.socket_ = zmq_socket(client.ctx_, ZMQ_REQ);
    if (client.socket_ == nullptr) {
        TransportError e{ErrorKind::Transport, zmq_reason("zmq_socket")};
        client.close();
        return Result<ModelClient>::err(std::move(e));
    }

    // Clamp the timeout to int milliseconds (ZMQ_RCVTIMEO/ZMQ_SNDTIMEO are int).
    // A negative value would mean "block forever"; we never want that here, so
    // treat any out-of-range value as the maximum finite timeout.
    long long ms = timeout.count();
    int       timeout_ms;
    if (ms < 0 || ms > std::numeric_limits<int>::max()) {
        timeout_ms = std::numeric_limits<int>::max();
    } else {
        timeout_ms = static_cast<int>(ms);
    }

    const int linger = 0;
    if (zmq_setsockopt(client.socket_, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms)) != 0 ||
        zmq_setsockopt(client.socket_, ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms)) != 0 ||
        zmq_setsockopt(client.socket_, ZMQ_LINGER, &linger, sizeof(linger)) != 0) {
        TransportError e{ErrorKind::Transport, zmq_reason("zmq_setsockopt")};
        client.close();
        return Result<ModelClient>::err(std::move(e));
    }

    if (zmq_connect(client.socket_, endpoint.c_str()) != 0) {
        TransportError e{ErrorKind::Transport, zmq_reason("zmq_connect")};
        client.close();
        return Result<ModelClient>::err(std::move(e));
    }

    return Result<ModelClient>::ok(std::move(client));
}

ModelClient::ModelClient(ModelClient&& o) noexcept
    : ctx_(std::exchange(o.ctx_, nullptr)),
      socket_(std::exchange(o.socket_, nullptr)),
      endpoint_(std::move(o.endpoint_)),
      mutex_(std::move(o.mutex_)) {}

ModelClient& ModelClient::operator=(ModelClient&& o) noexcept {
    if (this != &o) {
        close();
        ctx_      = std::exchange(o.ctx_, nullptr);
        socket_   = std::exchange(o.socket_, nullptr);
        endpoint_ = std::move(o.endpoint_);
        mutex_    = std::move(o.mutex_);
    }
    return *this;
}

ModelClient::~ModelClient() { close(); }

void ModelClient::close() noexcept {
    if (socket_ != nullptr) {
        zmq_close(socket_);
        socket_ = nullptr;
    }
    if (ctx_ != nullptr) {
        // zmq_ctx_term blocks until all sockets are closed; LINGER=0 above
        // guarantees it does not hang on undelivered messages.
        zmq_ctx_term(ctx_);
        ctx_ = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Low-level send / receive (caller holds mutex_)
// ─────────────────────────────────────────────────────────────────────────────

Result<void> ModelClient::send_frames(const std::string&       frame0,
                                      std::span<const uint8_t> frame1,
                                      bool                     has_frame1) {
    const int flags0 = has_frame1 ? ZMQ_SNDMORE : 0;
    if (zmq_send(socket_, frame0.data(), frame0.size(), flags0) < 0) {
        return Result<void>::err({ErrorKind::Transport, zmq_reason("zmq_send frame0")});
    }
    if (has_frame1) {
        if (zmq_send(socket_, frame1.data(), frame1.size(), 0) < 0) {
            return Result<void>::err({ErrorKind::Transport, zmq_reason("zmq_send frame1")});
        }
    }
    return Result<void>::ok();
}

Result<std::vector<uint8_t>> ModelClient::recv_reply() {
    zmq_msg_t msg;
    if (zmq_msg_init(&msg) != 0) {
        return transport_err(zmq_reason("zmq_msg_init"));
    }

    int rc = zmq_msg_recv(&msg, socket_, 0);
    if (rc < 0) {
        // EAGAIN here is a timeout (ZMQ_RCVTIMEO expired): treat as a dead model.
        std::string reason = zmq_reason("zmq_msg_recv");
        zmq_msg_close(&msg);
        return transport_err(reason);
    }

    const auto* data = static_cast<const uint8_t*>(zmq_msg_data(&msg));
    std::vector<uint8_t> reply(data, data + zmq_msg_size(&msg));

    // Drain and reject any extra frames: a vpp_sim reply is always a single
    // frame.  Leaving extra frames queued would desynchronise the REQ state
    // machine for the next request.
    bool multipart = false;
    int  more      = 0;
    size_t more_sz = sizeof(more);
    if (zmq_getsockopt(socket_, ZMQ_RCVMORE, &more, &more_sz) == 0 && more != 0) {
        multipart = true;
        zmq_msg_t extra;
        while (more != 0) {
            if (zmq_msg_init(&extra) != 0) {
                break;
            }
            if (zmq_msg_recv(&extra, socket_, 0) < 0) {
                zmq_msg_close(&extra);
                break;
            }
            zmq_msg_close(&extra);
            more_sz = sizeof(more);
            if (zmq_getsockopt(socket_, ZMQ_RCVMORE, &more, &more_sz) != 0) {
                break;
            }
        }
    }

    zmq_msg_close(&msg);

    if (multipart) {
        return Result<std::vector<uint8_t>>::err(
            {ErrorKind::Protocol, "unexpected multi-frame reply from model"});
    }
    return Result<std::vector<uint8_t>>::ok(std::move(reply));
}

Result<std::vector<uint8_t>> ModelClient::transact(const std::string&       frame0,
                                                   std::span<const uint8_t> frame1,
                                                   bool                     has_frame1) {
    // A moved-from client owns no mutex or socket.  Calling a verb on it is a
    // programming error, but must not crash: fail closed as a Transport error
    // (the same classification a dead model gets → -ENODEV downstream).
    if (mutex_ == nullptr || socket_ == nullptr) {
        return transport_err("model client is not connected (moved-from or closed)");
    }
    std::lock_guard<std::mutex> guard(*mutex_);

    Result<void> sent = send_frames(frame0, frame1, has_frame1);
    if (!sent) {
        return Result<std::vector<uint8_t>>::err(std::move(sent.error()));
    }
    return recv_reply();
}

// ─────────────────────────────────────────────────────────────────────────────
// Reply decoding helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::string reply_as_string(const std::vector<uint8_t>& reply) {
    return std::string(reinterpret_cast<const char*>(reply.data()), reply.size());
}

// Expect a literal ASCII "OK" reply; anything else ("ERR", empty, other) is a
// Protocol error.
Result<void> expect_ok(const std::vector<uint8_t>& reply) {
    std::string s = reply_as_string(reply);
    if (s == "OK") {
        return Result<void>::ok();
    }
    return Result<void>::err({ErrorKind::Protocol, "model did not reply OK: got \"" + s + "\""});
}

// Parse a reply as a single JSON value with a strict (non-tolerant) reader.
bool parse_json(const std::vector<uint8_t>& reply, Json::Value& out) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    const char* begin = reinterpret_cast<const char*>(reply.data());
    const char* end   = begin + reply.size();
    std::string errs;
    return reader->parse(begin, end, &out, &errs);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// vpp_sim verbs
// ─────────────────────────────────────────────────────────────────────────────

Result<void> ModelClient::start() {
    Json::Value cmd;
    cmd["command"] = "start";
    auto reply = transact(to_command_json(cmd), {}, false);
    if (!reply) {
        return Result<void>::err(std::move(reply.error()));
    }
    return expect_ok(reply.value());
}

Result<void> ModelClient::exit() {
    Json::Value cmd;
    cmd["command"] = "exit";
    auto reply = transact(to_command_json(cmd), {}, false);
    if (!reply) {
        return Result<void>::err(std::move(reply.error()));
    }
    return expect_ok(reply.value());
}

Result<void> ModelClient::reg_write(uint64_t addr, uint32_t val) {
    Json::Value cmd;
    cmd["command"] = "reg";
    cmd["addr"]    = Json::UInt64(addr);
    // Match the reference client: the 32-bit AXI-Lite value is encoded as a
    // Json::UInt64.
    cmd["val"] = Json::UInt64(val);
    auto reply = transact(to_command_json(cmd), {}, false);
    if (!reply) {
        return Result<void>::err(std::move(reply.error()));
    }
    return expect_ok(reply.value());
}

Result<void> ModelClient::populate(uint64_t addr, std::span<const uint8_t> data) {
    Json::Value cmd;
    cmd["command"] = "populate";
    cmd["addr"]    = Json::UInt64(addr);
    // INVARIANT: size must equal the frame-1 payload length.  The sim reads
    // exactly `size` bytes from frame 1; a larger size would OOB-read the model.
    cmd["size"] = Json::UInt64(data.size());
    auto reply  = transact(to_command_json(cmd), data, true);
    if (!reply) {
        return Result<void>::err(std::move(reply.error()));
    }
    return expect_ok(reply.value());
}

Result<std::vector<uint8_t>> ModelClient::fetch_buffer(uint64_t addr, uint64_t size) {
    Json::Value cmd;
    cmd["command"] = "fetch";
    cmd["type"]    = "buffer";
    cmd["addr"]    = Json::UInt64(addr);
    cmd["size"]    = Json::UInt64(size);

    auto reply = transact(to_command_json(cmd), {}, false);
    if (!reply) {
        return reply; // Transport error, forwarded as-is.
    }

    Json::Value response;
    if (!parse_json(reply.value(), response)) {
        return Result<std::vector<uint8_t>>::err(
            {ErrorKind::Protocol,
             "fetch buffer reply is not JSON: \"" + reply_as_string(reply.value()) + "\""});
    }
    if (!response.isArray()) {
        return Result<std::vector<uint8_t>>::err(
            {ErrorKind::Protocol, "fetch buffer reply is not a JSON array"});
    }
    if (response.size() != size) {
        return Result<std::vector<uint8_t>>::err(
            {ErrorKind::Protocol, "fetch buffer reply length " +
                                      std::to_string(response.size()) + " != requested " +
                                      std::to_string(size)});
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(size));
    for (const auto& elem : response) {
        if (!elem.isIntegral()) {
            return Result<std::vector<uint8_t>>::err(
                {ErrorKind::Protocol, "fetch buffer element is not an integer"});
        }
        Json::Int64 v = elem.asInt64();
        if (v < 0 || v > 255) {
            return Result<std::vector<uint8_t>>::err(
                {ErrorKind::Protocol,
                 "fetch buffer element out of byte range: " + std::to_string(v)});
        }
        bytes.push_back(static_cast<uint8_t>(v));
    }
    return Result<std::vector<uint8_t>>::ok(std::move(bytes));
}

Result<uint32_t> ModelClient::fetch_scalar(uint64_t addr) {
    Json::Value cmd;
    cmd["command"] = "fetch";
    cmd["type"]    = "scalar";
    cmd["addr"]    = Json::UInt64(addr);

    auto reply = transact(to_command_json(cmd), {}, false);
    if (!reply) {
        return Result<uint32_t>::err(std::move(reply.error()));
    }

    Json::Value response;
    if (!parse_json(reply.value(), response)) {
        return Result<uint32_t>::err(
            {ErrorKind::Protocol,
             "fetch scalar reply is not JSON: \"" + reply_as_string(reply.value()) + "\""});
    }
    if (!response.isIntegral()) {
        return Result<uint32_t>::err(
            {ErrorKind::Protocol, "fetch scalar reply is not an integer"});
    }
    Json::Int64 v = response.asInt64();
    if (v < 0 || v > std::numeric_limits<uint32_t>::max()) {
        return Result<uint32_t>::err(
            {ErrorKind::Protocol, "fetch scalar reply out of 32-bit range: " + std::to_string(v)});
    }
    return Result<uint32_t>::ok(static_cast<uint32_t>(v));
}

} // namespace slash_sysemu
