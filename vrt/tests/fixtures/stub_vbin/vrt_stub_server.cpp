#include <json/json.h>
#include <zmq.hpp>

#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr uint64_t VaddBase = 0x10000;
constexpr uint64_t VaddIn1Offset = 0x10;
constexpr uint64_t VaddIn2Offset = 0x18;
constexpr uint64_t VaddOutOffset = 0x20;
constexpr uint64_t VaddSizeOffset = 0x28;

using BufferMap = std::map<std::string, std::vector<uint8_t>>;
using RegisterMap = std::map<uint64_t, uint32_t>;

std::string toString(const zmq::message_t& message) {
    return std::string(static_cast<const char*>(message.data()), message.size());
}

std::vector<zmq::message_t> receiveFrames(zmq::socket_t& socket) {
    std::vector<zmq::message_t> frames;
    while (true) {
        zmq::message_t frame;
        auto received = socket.recv(frame);
        if (!received) {
            continue;
        }
        frames.push_back(std::move(frame));
        if (!socket.get(zmq::sockopt::rcvmore)) {
            break;
        }
    }
    return frames;
}

Json::Value parseJson(const zmq::message_t& message) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::string payload = toString(message);
    std::istringstream stream(payload);
    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        return Json::Value(Json::objectValue);
    }
    return root;
}

void sendString(zmq::socket_t& socket, const std::string& value) {
    zmq::message_t reply(value.data(), value.size());
    socket.send(reply, zmq::send_flags::none);
}

void sendOk(zmq::socket_t& socket) { sendString(socket, "OK"); }

std::string writeJson(const Json::Value& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

void sendJson(zmq::socket_t& socket, const Json::Value& value) { sendString(socket, writeJson(value)); }

std::string bufferKey(const Json::Value& command) {
    if (command.isMember("name")) {
        return command["name"].asString();
    }
    if (command.isMember("addr")) {
        return std::to_string(command["addr"].asUInt64());
    }
    return "";
}

uint64_t reconstruct64(const RegisterMap& registers, uint64_t base, uint64_t offset) {
    const auto lo = registers.find(base + offset);
    const auto hi = registers.find(base + offset + 4);
    const uint64_t loValue = lo == registers.end() ? 0 : lo->second;
    const uint64_t hiValue = hi == registers.end() ? 0 : hi->second;
    return (hiValue << 32) | loValue;
}

int32_t readInt32(const std::vector<uint8_t>& buffer, std::size_t index) {
    int32_t value = 0;
    const std::size_t byteIndex = index * sizeof(value);
    if (byteIndex + sizeof(value) <= buffer.size()) {
        std::memcpy(&value, buffer.data() + byteIndex, sizeof(value));
    }
    return value;
}

void writeInt32(std::vector<uint8_t>& buffer, std::size_t index, int32_t value) {
    const std::size_t byteIndex = index * sizeof(value);
    if (byteIndex + sizeof(value) > buffer.size()) {
        buffer.resize(byteIndex + sizeof(value));
    }
    std::memcpy(buffer.data() + byteIndex, &value, sizeof(value));
}

void runVadd(BufferMap& buffers, const std::string& in1Key, const std::string& in2Key,
             const std::string& outKey, uint32_t size) {
    const auto in1It = buffers.find(in1Key);
    const auto in2It = buffers.find(in2Key);
    const std::vector<uint8_t> empty;
    const auto& in1 = in1It == buffers.end() ? empty : in1It->second;
    const auto& in2 = in2It == buffers.end() ? empty : in2It->second;
    std::vector<uint8_t>& out = buffers[outKey];
    out.assign(static_cast<std::size_t>(size) * sizeof(int32_t), 0);

    for (uint32_t index = 0; index < size; ++index) {
        writeInt32(out, index, readInt32(in1, index) + readInt32(in2, index));
    }
}

uint32_t scalarValue(const Json::Value& command, const RegisterMap& registers) {
    if (command.isMember("addr")) {
        const uint64_t address = command["addr"].asUInt64();
        const auto it = registers.find(address);
        return it == registers.end() ? 0 : it->second;
    }
    if (command.get("function", "").asString() == "vadd" &&
        command.get("arg", "").asString() == "size") {
        const auto it = registers.find(VaddBase + VaddSizeOffset);
        return it == registers.end() ? 0 : it->second;
    }
    return 0;
}

bool handleCommand(zmq::socket_t& socket, const std::vector<zmq::message_t>& frames,
                   BufferMap& buffers, BufferMap& streams, RegisterMap& registers) {
    const Json::Value command = parseJson(frames.front());
    const std::string kind = command.get("command", "").asString();

    if (kind == "exit") {
        sendOk(socket);
        return false;
    }

    if (kind == "populate") {
        if (frames.size() > 1) {
            const auto* data = static_cast<const uint8_t*>(frames[1].data());
            buffers[bufferKey(command)] = std::vector<uint8_t>(data, data + frames[1].size());
        }
        sendOk(socket);
        return true;
    }

    if (kind == "stream_in") {
        if (frames.size() > 1) {
            const auto* data = static_cast<const uint8_t*>(frames[1].data());
            streams[command.get("name", "").asString()] =
                std::vector<uint8_t>(data, data + frames[1].size());
        }
        sendOk(socket);
        return true;
    }

    if (kind == "stream_out") {
        const std::string name = command.get("name", "").asString();
        const std::size_t size = command.get("size", 0).asUInt64();
        const auto it = streams.find(name);
        if (it == streams.end()) {
            std::vector<uint8_t> zeros(size, 0);
            zmq::message_t reply(zeros.data(), zeros.size());
            socket.send(reply, zmq::send_flags::none);
        } else {
            zmq::message_t reply(it->second.data(), it->second.size());
            socket.send(reply, zmq::send_flags::none);
        }
        return true;
    }

    if (kind == "fetch") {
        if (command.get("type", "").asString() == "buffer") {
            const std::string key = bufferKey(command);
            const std::size_t size = command.get("size", 0).asUInt64();
            const auto it = buffers.find(key);
            const std::vector<uint8_t> zeros(size, 0);
            const auto& buffer = it == buffers.end() ? zeros : it->second;
            Json::Value data(Json::arrayValue);
            for (uint8_t byte : buffer) {
                data.append(byte);
            }
            sendJson(socket, data);
        } else {
            sendJson(socket, Json::Value(scalarValue(command, registers)));
        }
        return true;
    }

    if (kind == "read_register") {
        sendJson(socket, Json::Value(0));
        return true;
    }

    if (kind == "reg") {
        const uint64_t address = command.get("addr", 0).asUInt64();
        const uint32_t value = command.get("val", 0).asUInt();
        if ((value & 0x1) != 0 && address == VaddBase) {
            const uint64_t in1 = reconstruct64(registers, VaddBase, VaddIn1Offset);
            const uint64_t in2 = reconstruct64(registers, VaddBase, VaddIn2Offset);
            const uint64_t out = reconstruct64(registers, VaddBase, VaddOutOffset);
            const auto sizeIt = registers.find(VaddBase + VaddSizeOffset);
            const uint32_t size = sizeIt == registers.end() ? 0 : sizeIt->second;
            runVadd(buffers, std::to_string(in1), std::to_string(in2), std::to_string(out), size);
            registers[address] = 0x6;
        } else {
            registers[address] = value;
        }
        sendOk(socket);
        return true;
    }

    if (kind == "call") {
        const std::string function = command.get("function", "").asString();
        if (function == "vadd") {
            const Json::Value args = command["args"];
            runVadd(buffers, args["arg0"].get("name", "").asString(),
                    args["arg1"].get("name", "").asString(),
                    args["arg2"].get("name", "").asString(),
                    args["arg3"].get("value", 0).asUInt());
        }
        sendOk(socket);
        return true;
    }

    sendOk(socket);
    return true;
}

}  // namespace

int main() {
    zmq::context_t context(1);
    zmq::socket_t socket(context, ZMQ_REP);
    socket.set(zmq::sockopt::linger, 0);
    socket.bind("tcp://*:5555");

    BufferMap buffers;
    BufferMap streams;
    RegisterMap registers;

    bool keepRunning = true;
    while (keepRunning) {
        keepRunning = handleCommand(socket, receiveFrames(socket), buffers, streams, registers);
    }

    return 0;
}