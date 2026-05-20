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

#ifndef VRT_QDMA_CONNECTION_HPP
#define VRT_QDMA_CONNECTION_HPP

#include <string>
#include <cstdint>

namespace vrt {

/**
 * @brief Enumeration for stream data direction.
 *
 * This enum represents the different directions for data streaming between
 * host and device.
 */
enum class StreamDirection {
    HOST_TO_DEVICE,  ///< Data flow from host to device (H2C)
    DEVICE_TO_HOST   ///< Data flow from device to host (C2H)
};

/**
 * @brief Class for managing QDMA connections.
 *
 * The QdmaConnection class provides functionality to manage connections
 * between the host and device using QDMA (Queue DMA) for data transfers.
 */
class QdmaConnection {
   public:
    /**
     * @brief Constructor for QdmaConnection.
     *
     * @param kernel The name of the kernel associated with this connection.
     * @param qid Queue ID for the QDMA operation.
     * @param interface The interface name for the connection.
     * @param direction String representation of the stream direction ("h2c" or "c2h").
     *
     * Initializes a new QDMA connection with the specified parameters.
     */
    QdmaConnection(const std::string& kernel, uint32_t qid, const std::string& interface,
                   const std::string& direction);

    /**
     * @brief Gets the kernel name.
     *
     * @return The name of the kernel associated with this connection.
     */
    std::string getKernel() const;

    /**
     * @brief Gets the queue ID.
     *
     * @return The queue ID for this QDMA connection.
     */
    uint32_t getQid() const;

    /**
     * @brief Gets the interface name.
     *
     * @return The interface name for this connection.
     */
    std::string getInterface() const;

    /**
     * @brief Gets the stream direction.
     *
     * @return The direction of data flow for this connection.
     */
    StreamDirection getDirection() const;

   private:
    std::string kernel;         ///< Name of the kernel associated with this connection.
    uint32_t qid;               ///< Queue ID for the QDMA operation.
    std::string interface;      ///< Interface name for the connection.
    StreamDirection direction;  ///< Direction of data flow.
};
}  // namespace vrt

#endif  // VRT_QDMA_CONNECTION_HPP