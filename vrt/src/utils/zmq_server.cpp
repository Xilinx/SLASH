/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <vrt/utils/zmq_server.hpp>

#include <limits>
#include <stdexcept>

namespace vrt {

ZmqServer::ZmqServer() : context(1), socket(context, ZMQ_REQ) { socket.connect(address); }

void ZmqServer::sendBuffer(const std::string& name, const std::vector<uint8_t>& buffer) {
    Json::Value command;
    command["command"] = "populate";
    command["name"] = name;
    command["size"] = static_cast<Json::UInt64>(buffer.size());

    zmq::message_t request(command.toStyledString().size());
    memcpy(request.data(), command.toStyledString().c_str(), command.toStyledString().size());
    socket.send(request, zmq::send_flags::sndmore);

    zmq::message_t data(buffer.size());
    memcpy(data.data(), buffer.data(), buffer.size());
    socket.send(data, zmq::send_flags::none);

    zmq::message_t reply;
    auto recvResult = socket.recv(reply);
    if (!recvResult) {
        throw std::runtime_error("ZMQ recv failed in sendBuffer");
    }
    std::string replyStr(static_cast<char*>(reply.data()), reply.size());
}

void ZmqServer::sendCommand(const Json::Value& command) {
    Json::StreamWriterBuilder writer;
    std::string commandStr = Json::writeString(writer, command);

    zmq::message_t request(commandStr.size());
    memcpy(request.data(), commandStr.data(), commandStr.size());
    socket.send(request, zmq::send_flags::none);
    zmq::message_t reply;
    auto recvResult = socket.recv(reply);
    if (!recvResult) {
        throw std::runtime_error("ZMQ recv failed in sendCommand");
    }
    std::string replyStr(static_cast<char*>(reply.data()), reply.size());
    if (replyStr != "OK") {
        throw std::runtime_error("ZMQ command failed: " + replyStr);
    }
}

uint32_t ZmqServer::fetchScalar(const std::string& function, const std::string& argIdx) {
    return fetchScalar(function, argIdx, std::numeric_limits<uint32_t>::max());
}

uint32_t ZmqServer::fetchScalar(const std::string& function, const std::string& argIdx, uint32_t offset) {
    Json::Value command;
    command["command"] = "fetch";
    command["type"] = "scalar";
    command["function"] = function;
    command["arg"] = argIdx;
    if (offset != std::numeric_limits<uint32_t>::max()) {
        command["offset"] = offset;
    }

    Json::StreamWriterBuilder writer;
    std::string commandStr = Json::writeString(writer, command);

    zmq::message_t request(commandStr.size());
    memcpy(request.data(), commandStr.c_str(), commandStr.size());
    socket.send(request, zmq::send_flags::none);

    zmq::message_t reply;
    auto recvResult = socket.recv(reply);
    if (!recvResult) {
        throw std::runtime_error("ZMQ recv failed in fetchScalar");
    }
    std::string replyStr(static_cast<char*>(reply.data()), reply.size());

    Json::Value response;
    Json::Reader reader;
    if (!reader.parse(replyStr, response)) {
        throw std::runtime_error("Invalid scalar fetch reply (not JSON): " + replyStr);
    }
    if (response.isObject() && response.isMember("error")) {
        throw std::runtime_error("Scalar fetch failed: " + response["error"].asString());
    }
    if (!response.isUInt() && !response.isInt()) {
        throw std::runtime_error("Invalid scalar fetch reply type");
    }
    return response.asUInt();
}

uint32_t ZmqServer::readRegister(const std::string& function, uint32_t offset) {
    Json::Value command;
    command["command"] = "read_register";
    command["function"] = function;
    command["offset"] = offset;

    Json::StreamWriterBuilder writer;
    std::string commandStr = Json::writeString(writer, command);

    zmq::message_t request(commandStr.size());
    memcpy(request.data(), commandStr.c_str(), commandStr.size());
    socket.send(request, zmq::send_flags::none);

    zmq::message_t reply;
    auto recvResult = socket.recv(reply);
    if (!recvResult) {
        throw std::runtime_error("ZMQ recv failed in readRegister");
    }
    std::string replyStr(static_cast<char*>(reply.data()), reply.size());

    Json::Value response;
    Json::Reader reader;
    if (!reader.parse(replyStr, response)) {
        throw std::runtime_error("Invalid read_register reply (not JSON): " + replyStr);
    }
    if (response.isObject() && response.isMember("error")) {
        std::string err = response["error"].asString();
        if (response.isMember("function")) {
            err += " function=" + response["function"].asString();
        }
        if (response.isMember("offset")) {
            err += " offset=" + std::to_string(response["offset"].asUInt());
        }
        throw std::runtime_error("read_register failed: " + err);
    }
    if (!response.isUInt() && !response.isInt()) {
        throw std::runtime_error("Invalid read_register reply type");
    }
    return response.asUInt();
}

std::vector<uint8_t> ZmqServer::fetchBuffer(const std::string& name) {
    Json::Value command;
    command["command"] = "fetch";
    command["type"] = "buffer";
    command["name"] = name;

    Json::StreamWriterBuilder writer;
    std::string commandStr = Json::writeString(writer, command);

    zmq::message_t request(commandStr.size());
    memcpy(request.data(), commandStr.c_str(), commandStr.size());
    socket.send(request, zmq::send_flags::none);

    zmq::message_t reply;
    auto recvResult = socket.recv(reply);
    if (!recvResult) {
        throw std::runtime_error("ZMQ recv failed in fetchBuffer");
    }
    std::string replyStr(static_cast<char*>(reply.data()), reply.size());

    Json::Value response;
    Json::Reader reader;
    reader.parse(replyStr, response);

    std::vector<uint8_t> byteArray;
    for (const auto& byte : response) {
        byteArray.push_back(static_cast<uint8_t>(byte.asUInt()));
    }

    return byteArray;
}

void ZmqServer::sendStream(const std::string& name, const std::vector<uint8_t>& buffer) {
    Json::Value command;
    command["command"] = "stream_in";
    command["name"] = name;

    zmq::message_t request(command.toStyledString().size());
    memcpy(request.data(), command.toStyledString().c_str(), command.toStyledString().size());
    socket.send(request, zmq::send_flags::sndmore);

    zmq::message_t data(buffer.size());
    memcpy(data.data(), buffer.data(), buffer.size());
    socket.send(data, zmq::send_flags::none);

    zmq::message_t reply;
    auto recvResult = socket.recv(reply);
    if (!recvResult) {
        throw std::runtime_error("ZMQ recv failed in sendStream");
    }
    std::string replyStr(static_cast<char*>(reply.data()), reply.size());
}

std::vector<uint8_t> ZmqServer::fetchStream(const std::string& name, size_t size) {
    Json::Value command;
    command["command"] = "stream_out";
    command["name"] = name;
    command["size"] = static_cast<Json::UInt64>(size);

    Json::StreamWriterBuilder writer;
    std::string commandStr = Json::writeString(writer, command);

    zmq::message_t request(commandStr.size());
    memcpy(request.data(), commandStr.c_str(), commandStr.size());
    socket.send(request, zmq::send_flags::none);

    zmq::message_t reply;
    auto recvResult = socket.recv(reply);
    if (!recvResult) {
        throw std::runtime_error("ZMQ recv failed in fetchStream");
    }
    std::vector<uint8_t> buffer(reply.size());
    memcpy(buffer.data(), reply.data(), reply.size());
    return buffer;
}

// hw simulation

void ZmqServer::fetchBufferSim(uint64_t addr, uint64_t size, std::vector<uint8_t>& buffer) {
    Json::Value command;
    command["command"] = "fetch";
    command["type"] = "buffer";
    command["addr"] = Json::UInt64(addr);
    command["size"] = Json::UInt64(size);

    Json::StreamWriterBuilder writer;
    std::string commandStr = Json::writeString(writer, command);

    zmq::message_t request(commandStr.size());
    memcpy(request.data(), commandStr.c_str(), commandStr.size());
    socket.send(request, zmq::send_flags::none);

    zmq::message_t reply;
    auto recvResult = socket.recv(reply);
    if (!recvResult) {
        throw std::runtime_error("ZMQ recv failed in fetchBufferSim");
    }
    std::string replyStr(static_cast<char*>(reply.data()), reply.size());

    Json::Value response;
    Json::Reader reader;
    reader.parse(replyStr, response);

    for (const auto& byte : response) {
        buffer.push_back(static_cast<uint8_t>(byte.asUInt()));
    }
}

uint32_t ZmqServer::fetchScalarSim(uint64_t addr) {
    Json::Value command;
    command["command"] = "fetch";
    command["type"] = "scalar";
    command["addr"] = Json::UInt64(addr);

    Json::StreamWriterBuilder writer;
    std::string commandStr = Json::writeString(writer, command);

    zmq::message_t request(commandStr.size());
    memcpy(request.data(), commandStr.c_str(), commandStr.size());
    socket.send(request, zmq::send_flags::none);

    zmq::message_t reply;
    auto recvResult = socket.recv(reply);
    if (!recvResult) {
        throw std::runtime_error("ZMQ recv failed in fetchScalarSim");
    }
    std::string replyStr(static_cast<char*>(reply.data()), reply.size());

    Json::Value response;
    Json::Reader reader;
    reader.parse(replyStr, response);

    return response.asUInt();
}

void ZmqServer::sendBufferSim(uint64_t addr, const std::vector<uint8_t>& buffer) {
    Json::Value command;
    command["command"] = "populate";
    command["addr"] = Json::UInt64(addr);
    command["size"] = Json::UInt64(buffer.size());
    zmq::message_t dataMsg(buffer.data(), buffer.size());
    std::string commandStr = Json::writeString(Json::StreamWriterBuilder(), command);
    zmq::message_t request(commandStr.c_str(), commandStr.size());
    socket.send(request, zmq::send_flags::sndmore);
    socket.send(dataMsg, zmq::send_flags::none);

    zmq::message_t reply;
    auto recvResult = socket.recv(reply);
    if (!recvResult) {
        throw std::runtime_error("ZMQ recv failed in sendBufferSim");
    }
    std::string replyStr(static_cast<char*>(reply.data()), reply.size());
}

void ZmqServer::sendScalar(uint64_t addr, uint32_t value) {
    Json::Value command;
    command["command"] = "reg";
    command["addr"] = Json::UInt64(addr);
    command["val"] = Json::UInt64(value);

    sendCommand(command);
}

}  // namespace vrt
