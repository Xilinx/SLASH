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

#ifndef VRT_ZMQ_SERVER_HPP
#define VRT_ZMQ_SERVER_HPP

#include <json/json.h>

#include <memory>
#include <vector>
#include <zmq.hpp>

#include <vrt/utils/logger.hpp>

namespace vrt {

/**
 * @brief Class for managing ZeroMQ server communication.
 *
 * The ZmqServer class provides functionality for communication between the host application
 * and a simulation/emulation executable using the ZeroMQ messaging library. It supports sending and
 * receiving commands, buffers, and streams, as well as reading and writing scalar values.
 */
class ZmqServer {
   private:
    zmq::context_t context;  ///< ZeroMQ context for managing socket connections.
    zmq::socket_t socket;    ///< ZeroMQ socket for communication.
    std::string address = "tcp://localhost:5555";  ///< Default server address.

   public:
    /**
     * @brief Constructor for ZmqServer.
     *
     * Initializes a new ZeroMQ server connection with the default address.
     */
    ZmqServer();

    /**
     * @brief Sends a named buffer to the server.
     *
     * @param name The name identifier for the buffer.
     * @param buffer The data buffer to send.
     */
    void sendBuffer(const std::string& name, const std::vector<uint8_t>& buffer);

    /**
     * @brief Sends a JSON command to the server.
     *
     * @param command The JSON object representing the command to send.
     */
    void sendCommand(const Json::Value& command);

    /**
     * @brief Fetches a scalar value from the server.
     *
     * @param function The function name associated with the scalar value.
     * @param argIdx The argument index or identifier.
     * @return The requested scalar value.
     */
    uint32_t fetchScalar(const std::string& function, const std::string& argIdx);

    /**
     * @brief Fetches a scalar value from the server with an optional register offset hint.
     *
     * The offset is used by EMU runtimes that support manifest route disambiguation for split
     * registers (e.g., low/high words of 64-bit AXI-Lite values).
     *
     * @param function The function name associated with the scalar value.
     * @param argIdx The argument index or identifier.
     * @param offset The kernel register offset being read.
     * @return The requested scalar value.
     */
    uint32_t fetchScalar(const std::string& function, const std::string& argIdx, uint32_t offset);

    /**
     * @brief Reads an emulated kernel register by function instance and register offset.
     *
     * @param function The kernel instance name.
     * @param offset The AXI-Lite register offset.
     * @return The 32-bit register value.
     */
    uint32_t readRegister(const std::string& function, uint32_t offset);

    /**
     * @brief Fetches a named buffer from the server.
     *
     * @param name The name identifier of the buffer to fetch.
     * @return The requested buffer data.
     */
    std::vector<uint8_t> fetchBuffer(const std::string& name);

    /**
     * @brief Sends a stream to the server.
     *
     * @param name The name identifier for the stream.
     * @param buffer The data buffer to stream.
     */
    void sendStream(const std::string& name, const std::vector<uint8_t>& buffer);

    /**
     * @brief Fetches a stream from the server.
     *
     * @param name The name identifier of the stream to fetch.
     * @param size The size of the data to fetch.
     * @return The requested stream data.
     */
    std::vector<uint8_t> fetchStream(const std::string& name, size_t size);

    /**
     * @brief Fetches a scalar value from a simulation at a specific address.
     *
     * @param addr The memory address to read from.
     * @return The scalar value read from the specified address.
     */
    uint32_t fetchScalarSim(uint64_t addr);

    /**
     * @brief Fetches buffer data from a simulation at a specific address.
     *
     * @param addr The starting memory address to read from.
     * @param size The size of the buffer to read.
     * @param buffer Reference to the buffer where data will be stored.
     */
    void fetchBufferSim(uint64_t addr, uint64_t size, std::vector<uint8_t>& buffer);

    /**
     * @brief Sends buffer data to a simulation at a specific address.
     *
     * @param addr The starting memory address to write to.
     * @param buffer The buffer containing data to write.
     */
    void sendBufferSim(uint64_t addr, const std::vector<uint8_t>& buffer);

    /**
     * @brief Sends a scalar value to a specific memory address.
     *
     * @param addr The memory address to write to.
     * @param value The value to write.
     */
    void sendScalar(uint64_t addr, uint32_t value);

    /**
     * @brief Deleted copy constructor.
     *
     * ZmqServer objects cannot be copied to prevent multiple ownership of socket resources.
     */
    ZmqServer(const ZmqServer&) = delete;

    /**
     * @brief Deleted copy assignment operator.
     *
     * ZmqServer objects cannot be copy-assigned to prevent multiple ownership of socket resources.
     *
     * @return Reference to this ZmqServer.
     */
    ZmqServer& operator=(const ZmqServer&) = delete;

    /**
     * @brief Default move constructor.
     *
     * Enables moving ZmqServer objects, transferring ownership of resources.
     */
    ZmqServer(ZmqServer&&) = default;

    /**
     * @brief Default move assignment operator.
     *
     * Enables move-assigning ZmqServer objects, transferring ownership of resources.
     *
     * @return Reference to this ZmqServer.
     */
    ZmqServer& operator=(ZmqServer&&) = default;
};

}  // namespace vrt

#endif  // VRT_ZMQ_SERVER_HPP
