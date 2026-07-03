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

#include "mock_model_server.h"

#include <stdexcept>
#include <string>
#include <utility>

#include <unistd.h>

#include <json/json.h>
#include <zmq.h>

namespace slash_emu::test {

namespace {

std::string zmq_reason(const char* what) {
    return std::string(what) + ": " + zmq_strerror(zmq_errno());
}

std::vector<uint8_t> str_bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder builder;
    return Json::writeString(builder, v);
}

} // namespace

MockModelServer::MockModelServer(const std::string& endpoint) : endpoint_(endpoint) {
    ctx_ = zmq_ctx_new();
    if (ctx_ == nullptr) {
        throw std::runtime_error(zmq_reason("mock zmq_ctx_new"));
    }
    socket_ = zmq_socket(ctx_, ZMQ_REP);
    if (socket_ == nullptr) {
        std::string e = zmq_reason("mock zmq_socket");
        zmq_ctx_term(ctx_);
        throw std::runtime_error(e);
    }
    const int linger = 0;
    zmq_setsockopt(socket_, ZMQ_LINGER, &linger, sizeof(linger));
    // Short receive timeout so run() can observe the stop flag promptly.
    const int rcv_timeout = 50;
    zmq_setsockopt(socket_, ZMQ_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

    if (zmq_bind(socket_, endpoint.c_str()) != 0) {
        std::string e = zmq_reason("mock zmq_bind");
        zmq_close(socket_);
        zmq_ctx_term(ctx_);
        throw std::runtime_error(e);
    }

    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

MockModelServer::~MockModelServer() { stop(); }

void MockModelServer::stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    if (socket_ != nullptr) {
        zmq_close(socket_);
        socket_ = nullptr;
    }
    if (ctx_ != nullptr) {
        zmq_ctx_term(ctx_);
        ctx_ = nullptr;
    }
    // libzmq does not reliably unlink the ipc socket file (especially after our
    // close+rebind fault paths); remove it ourselves to keep /tmp clean.
    const std::string ipc_prefix = "ipc://";
    if (endpoint_.rfind(ipc_prefix, 0) == 0) {
        ::unlink(endpoint_.substr(ipc_prefix.size()).c_str());
    }
}

void MockModelServer::set_fault(FaultMode mode, int normal_count) {
    std::lock_guard<std::mutex> g(mu_);
    fault_                  = mode;
    fault_normal_remaining_ = normal_count;
}

std::vector<RequestRecord> MockModelServer::requests() const {
    std::lock_guard<std::mutex> g(mu_);
    return records_;
}

std::size_t MockModelServer::request_count() const {
    std::lock_guard<std::mutex> g(mu_);
    return records_.size();
}

uint8_t MockModelServer::peek(uint64_t addr) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = memory_.find(addr);
    return it == memory_.end() ? 0 : it->second;
}

void MockModelServer::poke(uint64_t addr, uint8_t value) {
    std::lock_guard<std::mutex> g(mu_);
    memory_[addr] = value;
}

void MockModelServer::poke_buffer(uint64_t addr, const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> g(mu_);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        memory_[addr + i] = bytes[i];
    }
}

void MockModelServer::set_scalar(uint64_t addr, uint32_t value) {
    std::lock_guard<std::mutex> g(mu_);
    scalars_[addr] = value;
}

// ─────────────────────────────────────────────────────────────────────────────
// Service loop
// ─────────────────────────────────────────────────────────────────────────────

void MockModelServer::run() {
    while (running_.load()) {
        zmq_msg_t msg;
        if (zmq_msg_init(&msg) != 0) {
            continue;
        }
        int rc = zmq_msg_recv(&msg, socket_, 0);
        if (rc < 0) {
            zmq_msg_close(&msg);
            // EAGAIN = poll timeout: loop and re-check the stop flag.
            continue;
        }

        const auto* d0 = static_cast<const uint8_t*>(zmq_msg_data(&msg));
        std::string frame0(reinterpret_cast<const char*>(d0), zmq_msg_size(&msg));
        zmq_msg_close(&msg);

        // Collect any additional frames (frame 1 = populate payload).
        std::vector<uint8_t> frame1;
        bool                 has_frame1 = false;
        int                  more       = 0;
        size_t               more_sz    = sizeof(more);
        zmq_getsockopt(socket_, ZMQ_RCVMORE, &more, &more_sz);
        while (more != 0) {
            zmq_msg_t m1;
            zmq_msg_init(&m1);
            if (zmq_msg_recv(&m1, socket_, 0) < 0) {
                zmq_msg_close(&m1);
                break;
            }
            const auto* d1 = static_cast<const uint8_t*>(zmq_msg_data(&m1));
            frame1.assign(d1, d1 + zmq_msg_size(&m1));
            has_frame1 = true;
            zmq_msg_close(&m1);
            more_sz = sizeof(more);
            zmq_getsockopt(socket_, ZMQ_RCVMORE, &more, &more_sz);
        }

        int now = in_flight_.fetch_add(1) + 1;
        int prev_max = max_in_flight_.load();
        while (now > prev_max && !max_in_flight_.compare_exchange_weak(prev_max, now)) {
            // retry
        }

        Reply reply = dispatch(frame0, frame1, has_frame1);

        in_flight_.fetch_sub(1);

        if (reply.frames.empty() && !reply.close_after) {
            // Silence: to keep the REP state machine consistent (REP must reply
            // before the next recv) we tear the socket down and rebuild it, so a
            // silent server truly never answers this request.  Simpler and
            // equally effective: recreate the REP socket.
            zmq_close(socket_);
            socket_ = zmq_socket(ctx_, ZMQ_REP);
            const int linger = 0;
            zmq_setsockopt(socket_, ZMQ_LINGER, &linger, sizeof(linger));
            const int rcv_timeout = 50;
            zmq_setsockopt(socket_, ZMQ_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
            zmq_bind(socket_, endpoint_.c_str());
            continue;
        }

        for (std::size_t i = 0; i < reply.frames.size(); ++i) {
            int flags = (i + 1 < reply.frames.size()) ? ZMQ_SNDMORE : 0;
            zmq_send(socket_, reply.frames[i].data(), reply.frames[i].size(), flags);
        }

        if (reply.close_after) {
            // Simulate a dead model: drop the socket entirely and rebind a fresh
            // REP so subsequent tests on the same endpoint still work if they
            // reconnect.  The in-flight client sees ECONNRESET / timeout.
            zmq_close(socket_);
            socket_ = zmq_socket(ctx_, ZMQ_REP);
            const int linger = 0;
            zmq_setsockopt(socket_, ZMQ_LINGER, &linger, sizeof(linger));
            const int rcv_timeout = 50;
            zmq_setsockopt(socket_, ZMQ_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
            zmq_bind(socket_, endpoint_.c_str());
        }
    }
}

MockModelServer::Reply MockModelServer::dispatch(const std::string&          frame0,
                                                 const std::vector<uint8_t>& frame1,
                                                 bool                        has_frame1) {
    // Decide the fault for this request under the lock, and record it.
    FaultMode mode;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (fault_normal_remaining_ > 0) {
            --fault_normal_remaining_;
            mode = FaultMode::None;
        } else {
            mode = fault_;
        }
    }

    Json::Value  cmd;
    Json::Reader reader;
    bool         parsed = reader.parse(frame0, cmd);

    RequestRecord rec;
    rec.raw_frame0  = frame0;
    rec.payload_len = has_frame1 ? frame1.size() : 0;
    if (parsed && cmd.isObject()) {
        std::string command = cmd.get("command", "").asString();
        rec.command         = command;
        if (cmd.isMember("addr")) {
            rec.addr = cmd["addr"].asUInt64();
        }
        if (cmd.isMember("size")) {
            rec.size = cmd["size"].asUInt64();
        }
        if (command == "fetch") {
            rec.command = "fetch:" + cmd.get("type", "").asString();
        }
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        records_.push_back(rec);
    }

    // Handle faults that don't depend on the verb first.
    switch (mode) {
        case FaultMode::Silence:
            return Reply{{}, false};
        case FaultMode::Close:
            return Reply{{}, true};
        case FaultMode::Delay:
            std::this_thread::sleep_for(delay_);
            break; // then fall through to a normal reply
        default:
            break;
    }

    auto ok_reply  = [] { return Reply{{str_bytes("OK")}, false}; };
    auto err_reply = [] { return Reply{{str_bytes("ERR")}, false}; };

    if (!parsed || !cmd.isObject()) {
        return err_reply();
    }
    const std::string command = cmd.get("command", "").asString();

    // Verb handling with fault overlays.
    if (command == "start" || command == "exit" || command == "reg" || command == "populate") {
        // These expect "OK".
        if (command == "populate" && has_frame1) {
            uint64_t addr = cmd.get("addr", 0).asUInt64();
            std::lock_guard<std::mutex> g(mu_);
            for (std::size_t i = 0; i < frame1.size(); ++i) {
                memory_[addr + i] = frame1[i];
            }
        }
        if (command == "reg") {
            uint64_t addr = cmd.get("addr", 0).asUInt64();
            uint32_t val  = static_cast<uint32_t>(cmd.get("val", 0).asUInt64());
            std::lock_guard<std::mutex> g(mu_);
            scalars_[addr] = val;
        }
        switch (mode) {
            case FaultMode::WrongReply:
                return Reply{{str_bytes("42")}, false}; // JSON where OK expected
            case FaultMode::ErrReply:
                return err_reply();
            case FaultMode::ExtraFrame:
                return Reply{{str_bytes("OK"), str_bytes("extra")}, false};
            case FaultMode::EmptyReply:
                return Reply{{str_bytes("")}, false}; // empty frame where OK expected
            case FaultMode::JsonStringReply:
                return Reply{{str_bytes("\"OK\"")}, false}; // quoted, not bare OK
            default:
                return ok_reply();
        }
    }

    if (command == "fetch") {
        const std::string type = cmd.get("type", "").asString();
        if (type == "buffer") {
            uint64_t addr = cmd.get("addr", 0).asUInt64();
            uint64_t size = cmd.get("size", 0).asUInt64();
            switch (mode) {
                case FaultMode::WrongReply:
                    return ok_reply(); // "OK" where array expected
                case FaultMode::ErrReply:
                    return err_reply();
                case FaultMode::MalformedJson:
                    return Reply{{str_bytes("not json <<<")}, false};
                case FaultMode::ExtraFrame:
                    return Reply{{str_bytes("[]"), str_bytes("extra")}, false};
                case FaultMode::JsonStringReply:
                    return Reply{{str_bytes("\"OK\"")}, false};
                default:
                    break;
            }
            Json::Value arr(Json::arrayValue);
            uint64_t    count = size;
            if (mode == FaultMode::ShortBuffer && count > 0) {
                count -= 1;
            }
            if (mode == FaultMode::LongBuffer) {
                count += 1; // one MORE element than requested
            }
            std::lock_guard<std::mutex> g(mu_);
            for (uint64_t i = 0; i < count; ++i) {
                if (mode == FaultMode::OversizedByte && i == 0) {
                    arr.append(Json::UInt(999));
                    continue;
                }
                if (mode == FaultMode::NegativeByte && i == 0) {
                    arr.append(Json::Int(-1));
                    continue;
                }
                if (mode == FaultMode::FloatByte && i == 0) {
                    arr.append(3.5);
                    continue;
                }
                auto it = memory_.find(addr + i);
                arr.append(Json::UInt(it == memory_.end() ? 0 : it->second));
            }
            return Reply{{str_bytes(compact(arr))}, false};
        }
        if (type == "scalar") {
            uint64_t addr = cmd.get("addr", 0).asUInt64();
            switch (mode) {
                case FaultMode::WrongReply:
                    return ok_reply(); // "OK" where int expected
                case FaultMode::ErrReply:
                    return err_reply();
                case FaultMode::MalformedJson:
                    return Reply{{str_bytes("not json <<<")}, false};
                case FaultMode::OversizedByte:
                    // For scalar, reuse OversizedByte to mean "> 32 bits".
                    return Reply{{str_bytes("4294967296")}, false};
                case FaultMode::NegativeScalar:
                    return Reply{{str_bytes("-1")}, false};
                case FaultMode::FloatScalar:
                    return Reply{{str_bytes("3.5")}, false};
                case FaultMode::JsonStringReply:
                    return Reply{{str_bytes("\"OK\"")}, false};
                default:
                    break;
            }
            uint32_t val = 0;
            {
                std::lock_guard<std::mutex> g(mu_);
                auto it = scalars_.find(addr);
                if (it != scalars_.end()) {
                    val = it->second;
                }
            }
            return Reply{{str_bytes(std::to_string(val))}, false};
        }
    }

    return err_reply();
}

} // namespace slash_emu::test
