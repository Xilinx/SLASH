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

#include "model_server.h"

#include <stdexcept>
#include <utility>

#include <unistd.h>

#include <json/json.h>
#include <zmq.h>

namespace slash_sysemu::model {

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

ModelServer::ModelServer(const std::string& endpoint) : endpoint_(endpoint) {
    ctx_ = zmq_ctx_new();
    if (ctx_ == nullptr) {
        throw std::runtime_error(zmq_reason("zmq_ctx_new"));
    }
    socket_ = zmq_socket(ctx_, ZMQ_REP);
    if (socket_ == nullptr) {
        std::string e = zmq_reason("zmq_socket");
        zmq_ctx_term(ctx_);
        throw std::runtime_error(e);
    }
    const int linger = 0;
    zmq_setsockopt(socket_, ZMQ_LINGER, &linger, sizeof(linger));
    // Short receive timeout so serve() can observe stop()/exit promptly.
    const int rcv_timeout = 50;
    zmq_setsockopt(socket_, ZMQ_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

    if (zmq_bind(socket_, endpoint.c_str()) != 0) {
        std::string e = zmq_reason("zmq_bind");
        zmq_close(socket_);
        zmq_ctx_term(ctx_);
        throw std::runtime_error(e);
    }
}

ModelServer::~ModelServer() {
    if (socket_ != nullptr) {
        zmq_close(socket_);
        socket_ = nullptr;
    }
    if (ctx_ != nullptr) {
        zmq_ctx_term(ctx_);
        ctx_ = nullptr;
    }
    // libzmq does not reliably unlink the ipc socket file; remove it ourselves.
    const std::string ipc_prefix = "ipc://";
    if (endpoint_.rfind(ipc_prefix, 0) == 0) {
        ::unlink(endpoint_.substr(ipc_prefix.size()).c_str());
    }
}

void ModelServer::stop() noexcept { running_.store(false); }

void ModelServer::serve() {
    running_.store(true);
    exit_requested_ = false;

    while (running_.load() && !exit_requested_) {
        zmq_msg_t msg;
        if (zmq_msg_init(&msg) != 0) {
            continue;
        }
        int rc = zmq_msg_recv(&msg, socket_, 0);
        if (rc < 0) {
            zmq_msg_close(&msg);
            // EAGAIN = poll timeout: loop and re-check the stop/exit flags.
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

        std::vector<std::vector<uint8_t>> frames = handle(frame0, frame1, has_frame1);

        for (std::size_t i = 0; i < frames.size(); ++i) {
            int flags = (i + 1 < frames.size()) ? ZMQ_SNDMORE : 0;
            zmq_send(socket_, frames[i].data(), frames[i].size(), flags);
        }
    }
}

std::vector<std::vector<uint8_t>> ModelServer::handle(const std::string&          frame0,
                                                      const std::vector<uint8_t>& frame1,
                                                      bool                        has_frame1) {
    Json::Value  cmd;
    Json::Reader reader;
    if (!reader.parse(frame0, cmd) || !cmd.isObject()) {
        return {str_bytes("ERR")};
    }
    const std::string command = cmd.get("command", "").asString();

    if (command == "start") {
        return {str_bytes("OK")};
    }
    if (command == "exit") {
        exit_requested_ = true;
        return {str_bytes("OK")};
    }
    if (command == "reg") {
        uint64_t addr    = cmd.get("addr", 0).asUInt64();
        uint32_t val     = static_cast<uint32_t>(cmd.get("val", 0).asUInt64());
        scalars_[addr]   = val;
        return {str_bytes("OK")};
    }
    if (command == "populate") {
        if (has_frame1) {
            uint64_t addr = cmd.get("addr", 0).asUInt64();
            for (std::size_t i = 0; i < frame1.size(); ++i) {
                memory_[addr + i] = frame1[i];
            }
        }
        return {str_bytes("OK")};
    }
    if (command == "fetch") {
        const std::string type = cmd.get("type", "").asString();
        if (type == "buffer") {
            uint64_t    addr = cmd.get("addr", 0).asUInt64();
            uint64_t    size = cmd.get("size", 0).asUInt64();
            Json::Value arr(Json::arrayValue);
            for (uint64_t i = 0; i < size; ++i) {
                auto it = memory_.find(addr + i);
                arr.append(Json::UInt(it == memory_.end() ? 0 : it->second));
            }
            return {str_bytes(compact(arr))};
        }
        if (type == "scalar") {
            uint64_t addr = cmd.get("addr", 0).asUInt64();
            uint32_t val  = 0;
            auto     it   = scalars_.find(addr);
            if (it != scalars_.end()) {
                val = it->second;
            }
            return {str_bytes(std::to_string(val))};
        }
    }

    return {str_bytes("ERR")};
}

} // namespace slash_sysemu::model
