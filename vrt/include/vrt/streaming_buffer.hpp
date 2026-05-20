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

#ifndef VRT_STREAMING_BUFFER_HPP
#define VRT_STREAMING_BUFFER_HPP

#include <regex>

#include "device.hpp"
#include "qdma/qdma_connection.hpp"
#include "qdma/qdma_intf.hpp"
#include "utils/platform.hpp"
#include "utils/zmq_server.hpp"

namespace vrt {

/**
 * @brief Class representing a streaming buffer.
 *
 * This class provides an interface for managing a streaming buffer in a device.
 * It supports streaming QDMA connections.
 *
 * @tparam T The type of the elements in the buffer.
 */
template <typename T>
class StreamingBuffer {
   public:
    /**
     * @brief Constructs a StreamingBuffer object.
     *
     * @param device The device associated with the buffer.
     * @param kernel The kernel associated with the buffer.
     * @param portName The name of the port associated with the buffer.
     * @param size The size of the buffer.
     */
    StreamingBuffer(Device device, Kernel kernel, const std::string& portName, size_t size);

    /**
     * @brief Destructs the StreamingBuffer object.
     */
    ~StreamingBuffer();

    /**
     * @brief Gets a pointer to the buffer.
     *
     * @return A pointer to the buffer.
     */
    T* get() const;

    /**
     * @brief Accesses an element in the buffer.
     *
     * @param index The index of the element to access.
     * @return A reference to the element at the specified index.
     * @throws std::out_of_range If the index is out of range.
     */
    T& operator[](size_t index);

    /**
     * @brief Accesses an element in the buffer (const version).
     *
     * @param index The index of the element to access.
     * @return A const reference to the element at the specified index.
     * @throws std::out_of_range If the index is out of range.
     */
    const T& operator[](size_t index) const;

    /**
     * @brief Gets the name of the buffer.
     *
     * @return The name of the buffer.
     */
    std::string getName() const;

    /**
     * @brief Synchronizes the buffer with the device.
     */
    void sync();

   private:
    T* localBuffer;                     ///< Pointer to the local buffer.
    size_t size;                        ///< Size of the buffer.
    StreamDirection syncType;           ///< Synchronization type (direction).
    Device device;                      ///< Device associated with the buffer.
    Kernel kernel;                      ///< Kernel associated with the buffer.
    std::size_t index;                  ///< Index of the buffer.
    std::string name;                   ///< Name of the buffer.
    std::string portName;               ///< Name of the port associated with the buffer.
    QdmaIntf* qdmaInterface = nullptr;  ///< Pointer to the QDMA interface.
};

template <typename T>
StreamingBuffer<T>::StreamingBuffer(Device device, Kernel kernel, const std::string& portName,
                                    size_t size)
    : device(device), size(size), kernel(kernel), portName(portName) {
    std::vector<QdmaConnection> qdmaConnections = device.getHandle()->getQdmaConnections();
    bool gotQdma = false;
    for (const auto& con : qdmaConnections) {
        if (con.getKernel() == kernel.getName() && portName == con.getInterface()) {
            index = con.getQid();
            syncType = con.getDirection();
            gotQdma = true;
        }
    }
    if (!gotQdma) {
        throw std::runtime_error("No QDMA connection found for kernel " + kernel.getName() +
                                 " and port " + portName);
    }
    name = (syncType == StreamDirection::HOST_TO_DEVICE)
               ? ("streamingBuffer_" + std::to_string(index))
               : ("outputStreamingBuffer_" + std::to_string(index));
    localBuffer = new T[size];
    Platform platform = device.getPlatform();
    if (platform == Platform::HARDWARE) {
        for (auto& qdmaIntf : device.getHandle()->getQdmaInterfaces()) {
            if (qdmaIntf->getQueueIdx() == index) {
                qdmaInterface = qdmaIntf;
            }
        }
    }
}

template <typename T>
StreamingBuffer<T>::~StreamingBuffer() {
    delete[] localBuffer;
}

template <typename T>
T& StreamingBuffer<T>::operator[](size_t index) {
    if (index >= size) {
        throw std::out_of_range("Index out of range");
    }
    return localBuffer[index];
}

template <typename T>
const T& StreamingBuffer<T>::operator[](size_t index) const {
    if (index >= size) {
        throw std::out_of_range("Index out of range");
    }
    return localBuffer[index];
}

template <typename T>
void StreamingBuffer<T>::sync() {
    Platform platform = device.getPlatform();
    if (platform == Platform::EMULATION) {
        auto server = device.getHandle()->getZmqServer();
        if (syncType == StreamDirection::HOST_TO_DEVICE) {
            std::vector<uint8_t> sendData;
            std::size_t dataSize = size * sizeof(T);
            sendData.resize(dataSize);
            std::memcpy(sendData.data(), localBuffer, dataSize);
            server->sendStream(name, sendData);
        } else {
            std::vector<uint8_t> recvData = server->fetchStream(name, size * sizeof(T));
            size = recvData.size() / sizeof(T);
            localBuffer = reinterpret_cast<T*>(realloc(localBuffer, recvData.size()));
            std::memcpy(localBuffer, recvData.data(), recvData.size());
        }
    } else if (platform == Platform::HARDWARE) {
        if (qdmaInterface == nullptr) {
            throw std::runtime_error("QDMA interface not initialized for streaming buffer");
        }
        if (syncType == StreamDirection::HOST_TO_DEVICE) {
            qdmaInterface->write_buff(reinterpret_cast<char*>(localBuffer), 0, size * sizeof(T));
        } else {
            qdmaInterface->read_buff(reinterpret_cast<char*>(localBuffer), 0, size * sizeof(T));
        }
    } else {
        throw std::runtime_error("Streaming buffer not implemented for this platform.");
    }
}

template <typename T>
std::string StreamingBuffer<T>::getName() const {
    return name;
}
}  // namespace vrt

#endif  // VRT_STREAMING_BUFFER_HPP
