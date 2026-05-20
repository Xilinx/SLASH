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

#ifndef VRT_QDMA_INTF_HPP
#define VRT_QDMA_INTF_HPP

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <optional>
#include <string>

#include <vrt/utils/logger.hpp>
#include <vrt/qdma/qdma_connection.hpp>

#include <vrtd/qdma_qpair.hpp>

#define RW_MAX_SIZE 0x7ffff000  ///< Maximum size for read/write operations
#define GB_DIV 1000000000       ///< Divider for gigabytes
#define MB_DIV 1000000          ///< Divider for megabytes
#define KB_DIV 1000             ///< Divider for kilobytes
#define NSEC_DIV 1000000000     ///< Divider for nanoseconds

namespace vrtd {
class Device;
}

namespace vrt {
/**
 * @brief Class for interfacing with QDMA.
 */
class QdmaIntf {
    uint8_t queueIdx;       ///< Queue index
    std::string bdf;        ///< Bus:Device.Function identifier
    std::optional<vrtd::QdmaQpair> qpair;  ///< vrtd qpair (streaming)
    int qpairFd = -1;                     ///< Cached qpair fd

    /**
     * @brief Writes data from a buffer to a device.
     * @param dev The device to write to.
     * @param buffer The buffer to write from.
     * @param size The size of the buffer.
     * @param base The base address to write to.
     * @return The number of bytes written.
     */
    ssize_t write_from_buffer(const char* dev, char* buffer, uint64_t size, uint64_t base);

    /**
     * @brief Reads data from a file into a buffer.
     * @param fname The file to read from.
     * @param buffer The buffer to read into.
     * @param size The size of the buffer.
     * @param base The base address to read from.
     * @return The number of bytes read.
     */
    ssize_t read_to_buffer(const char* fname, char* buffer, uint64_t size, uint64_t base);

    /**
     * @brief Strips the bus part from the BDF.
     * @param bdf The BDF to strip.
     * @return The stripped bus part.
     */

   public:
    /**
     * @brief Constructor of the QdmaIntf class
     * @param bdf The BDF (Bus:Device.Function) of the device.
     */
    /**
     * @brief Constructor of the QdmaIntf class using vrtd qpair (streaming).
     * @param device The vrtd device handle.
     * @param queueIdx The stream queue index (from system map).
     * @param direction Stream direction (H2C or C2H).
     */
    QdmaIntf(const vrtd::Device& device, const uint32_t queueIdx, StreamDirection direction);

    /**
     * @brief Default constructor for QdmaIntf.
     */
    QdmaIntf() = default;

    /**
     * @brief Writes a buffer to the device.
     * @param buffer The buffer to write.
     * @param start_addr The starting address to write to.
     * @param size The size of the buffer.
     */
    void write_buff(char* buffer, uint64_t start_addr, uint64_t size);

    /**
     * @brief Reads a buffer from the device.
     * @param buffer The buffer to read into.
     * @param start_addr The starting address to read from.
     * @param size The size of the buffer.
     */
    void read_buff(char* buffer, uint64_t start_addr, uint64_t size);

    /**
     * @brief Gets the queue index.
     * @return The queue index.
     */
    uint32_t getQueueIdx();
    /**
     * @brief Destructor for QdmaIntf.
     */
    ~QdmaIntf();
};

}  // namespace vrt
#endif  // VRT_QDMA_INTF_HPP
