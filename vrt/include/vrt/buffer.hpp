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

/**
 * @file buffer.hpp
 * @brief Buffer<T> — typed host-accessible memory with device synchronization.
 */

#ifndef VRT_BUFFER_HPP
#define VRT_BUFFER_HPP

#include <atomic>
#include <cstring>
#include <memory>

#include <vrt/allocator/allocator.hpp>
#include <vrt/device.hpp>
#include <vrt/qdma/qdma_intf.hpp>
#include <vrt/utils/platform.hpp>
#include <vrt/utils/zmq_server.hpp>

namespace vrt {

namespace detail {
inline uint64_t reserveFakePhysAddr(uint64_t sizeBytes, MemoryRangeType rangeType) {
    // Match linker simulation address windows from run_pre.tcl:
    //   HBM/HBM_VNOC: 0x4000_0000_00
    //   DDR:          0x6000_0000_000
    static std::atomic<uint64_t> nextHbm{0x4000000000ull};
    static std::atomic<uint64_t> nextDdr{0x60000000000ull};
    const uint64_t aligned = (sizeBytes + 0xfff) & ~0xfffull;
    if (rangeType == MemoryRangeType::DDR) {
        return nextDdr.fetch_add(aligned, std::memory_order_relaxed);
    }
    return nextHbm.fetch_add(aligned, std::memory_order_relaxed);
}
}  // namespace detail

/**
 * @brief Enum class representing the type of synchronization.
 */
enum class SyncType {
    HOST_TO_DEVICE,  ///< Synchronize from host to device
    DEVICE_TO_HOST,  ///< Synchronize from device to host
};

/**
 * @brief Class representing a buffer.
 *
 * This class provides an interface for managing a buffer in a device.
 * It supports memory mapped QDMA connections.
 *
 * @tparam T The type of the elements in the buffer.
 */
template <typename T>
class Buffer {
   public:
    /**
     * @brief Constructor for Buffer.
     * @param device VRT Device of the buffer.
     * @param size The size of the buffer.
     * @param type The type of memory range.
     */
    Buffer(Device device, size_t size, MemoryRangeType type);

    /**
     * @brief Constructor for Buffer.
     * @param device VRT Device of the buffer.
     * @param size The size of the buffer.
     * @param type The type of memory range.
     * @param port The HBM port number. Only valid when type is MemoryRangeType::HBM.
     */
    Buffer(Device device, size_t size, MemoryRangeType type, uint8_t port);

    /**
     * @brief Constructor for Buffer from a MemoryConfig.
     * @param device VRT Device of the buffer.
     * @param size The size of the buffer.
     * @param config Memory configuration, typically obtained via Kernel::portMemoryConfig()
     *               or Kernel::argMemoryConfig().
     */
    Buffer(Device device, size_t size, MemoryConfig config);

    /**
     * @brief Destructor for Buffer.
     */
    ~Buffer();

    /**
     * @brief Gets a pointer to the buffer.
     * @return A pointer to the buffer.
     */
    T* get() const;

    /**
     * @brief Overloads the subscript operator to access buffer elements.
     * @param index The index of the element to access.
     * @return A reference to the element at the specified index.
     */
    T& operator[](size_t index);

    /**
     * @brief Overloads the subscript operator to access buffer elements (const version).
     * @param index The index of the element to access.
     * @return A const reference to the element at the specified index.
     */
    const T& operator[](size_t index) const;

    /**
     * @brief Gets the memory range type of the buffer.
     * @return The memory range type.
     */
    MemoryRangeType getMemoryRangeType() const;

    /**
     * @brief Gets the HBM port number of the buffer.
     * @return The HBM port number, or 0 if no specific port was set.
     */
    uint8_t getHBMPort() const;

    /**
     * @brief Gets the physical address of the buffer.
     * @return The physical address of the buffer.
     */
    uint64_t getPhysAddr() const;

    /**
     * @brief Gets the lower 32 bits of the physical address of the buffer.
     * @return The lower 32 bits of the physical address of the buffer.
     */
    uint32_t getPhysAddrLow() const;

    /**
     * @brief Gets the upper 32 bits of the physical address of the buffer.
     * @return The upper 32 bits of the physical address of the buffer.
     */
    uint32_t getPhysAddrHigh() const;

    /**
     * @brief Synchronizes the buffer.
     * @param syncType The type of synchronization.
     */
    void sync(SyncType syncType);

    std::string getName();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

   private:
    static BufferAllocType resolveAllocType(MemoryRangeType type, bool hasPort);
    static HBMRegion resolveRegion(MemoryRangeType type, bool hasPort, uint8_t port);
    void initAllocate();

    uint64_t startAddress;           ///< The starting address of the buffer
    T* localBuffer;                  ///< Pointer to the local buffer
    size_t size;                     ///< The size of the buffer
    MemoryRangeType type;            ///< The type of memory range
    uint8_t hbmPort = 0;            ///< HBM port number
    bool hasPort = false;            ///< Whether an explicit HBM port was specified
    Device device;                   ///< The device associated with the buffer
    std::unique_ptr<Block> block;    ///< Allocator block (hardware only)
    UntypedBuffer* view;             ///< Cached view into the allocator block
    bool ownsLocalBuffer;            ///< Whether localBuffer should be deleted
    std::size_t index;               // Member variable to store the index of the buffer
    static std::size_t bufferIndex;  // Static variable to track the buffer index
};

template <typename T>
size_t Buffer<T>::bufferIndex = 0;

template <typename T>
Buffer<T>::Buffer(Device device, size_t size, MemoryRangeType type)
    : startAddress(0),
      localBuffer(nullptr),
      size(size),
      type(type),
      device(device),
      block(nullptr),
      view(nullptr),
      ownsLocalBuffer(false),
      index(bufferIndex++) {
    if (type == MemoryRangeType::HBM) {
        throw std::invalid_argument("HBM buffers require an explicit port. Use Buffer(device, size, MemoryRangeType::HBM, port)");
    }
    initAllocate();
}

template <typename T>
Buffer<T>::Buffer(Device device, size_t size, MemoryRangeType type, uint8_t port)
    : startAddress(0),
      localBuffer(nullptr),
      size(size),
      type(type),
      hbmPort(port),
      hasPort(true),
      device(device),
      block(nullptr),
      view(nullptr),
      ownsLocalBuffer(false),
      index(bufferIndex++) {
    if (type != MemoryRangeType::HBM) {
        throw std::invalid_argument("The port argument is only valid for HBM buffers. Use Buffer(device, size, type) for DDR or HBM_VNOC");
    }
    initAllocate();
}

template <typename T>
Buffer<T>::Buffer(Device device, size_t size, MemoryConfig config)
    : startAddress(0),
      localBuffer(nullptr),
      size(size),
      type(config.type),
      hbmPort(config.hbmPort.value_or(0)),
      hasPort(config.hbmPort.has_value()),
      device(device),
      block(nullptr),
      view(nullptr),
      ownsLocalBuffer(false),
      index(bufferIndex++) {
    initAllocate();
}

template <typename T>
void Buffer<T>::initAllocate() {
    Platform platform = this->device.getPlatform();
    if (platform == Platform::HARDWARE) {
        BufferAllocType allocType = resolveAllocType(type, hasPort);
        HBMRegion region = resolveRegion(type, hasPort, hbmPort);
        block = this->device.getHandle()->getAllocator()->allocate(this->device.getHandle()->getVrtdDevice(), allocType,
                                                                   BufferAllocDir::Bidirectional,
                                                                   size * sizeof(T), region);
        if (!block) {
            throw std::bad_alloc();
        }
        view = block->getUntypedBuffer();
        startAddress = view->getPhysAddr();
        localBuffer = static_cast<T*>(view->data());
        utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__,
                           "Allocated buffer final_space_bytes={} phys_addr={x}",
                           view->getSize(), startAddress);
    } else {
        startAddress = detail::reserveFakePhysAddr(size * sizeof(T), type);
        localBuffer = new T[size];
        ownsLocalBuffer = true;
        if (platform == Platform::EMULATION) {
            // send initial buffer so it is populated in the emulation environment
            std::shared_ptr<ZmqServer> server = this->device.getHandle()->getZmqServer();
            std::vector<uint8_t> sendData;
            std::size_t dataSize = size * sizeof(T);
            sendData.resize(dataSize);
            std::memcpy(sendData.data(), localBuffer, dataSize);
            server->sendBuffer(std::to_string(getPhysAddr()), sendData);
        }
    }
}

template <typename T>
Buffer<T>::~Buffer() {
    if (block) {
        device.getHandle()->getAllocator()->deallocate(std::move(block));
    }
    if (ownsLocalBuffer && localBuffer != nullptr) {
        delete[] localBuffer;
    }
}

template <typename T>
T* Buffer<T>::get() const {
    return localBuffer;
}

template <typename T>
T& Buffer<T>::operator[](size_t index) {
    if (index >= size) {
        throw std::out_of_range("Index out of range");
    }
    return localBuffer[index];
}

template <typename T>
const T& Buffer<T>::operator[](size_t index) const {
    if (index >= size) {
        throw std::out_of_range("Index out of range");
    }
    return localBuffer[index];
}

template <typename T>
uint64_t Buffer<T>::getPhysAddr() const {
    return startAddress;
}

template <typename T>
uint32_t Buffer<T>::getPhysAddrLow() const {
    return startAddress & 0xFFFFFFFF;
}

template <typename T>
uint32_t Buffer<T>::getPhysAddrHigh() const {
    return (startAddress >> 32) & 0xFFFFFFFF;
}

template <typename T>
MemoryRangeType Buffer<T>::getMemoryRangeType() const {
    return type;
}

template <typename T>
uint8_t Buffer<T>::getHBMPort() const {
    return hbmPort;
}

template <typename T>
std::string Buffer<T>::getName() {
    return "buffer_" + std::to_string(index);
}

template <typename T>
void Buffer<T>::sync(SyncType syncType) {
    Platform platform = device.getPlatform();
    if (platform == Platform::HARDWARE) {
        if (view == nullptr) {
            throw std::runtime_error("Buffer view unavailable for hardware sync");
        }
        uint64_t totalSize = size * sizeof(T);
        if (syncType == SyncType::HOST_TO_DEVICE) {
            view->syncToDevice(0, totalSize);
        } else if (syncType == SyncType::DEVICE_TO_HOST) {
            view->syncToHost(0, totalSize);
        } else {
            throw std::invalid_argument("Invalid sync type");
        }
    } else if (platform == Platform::EMULATION) {
        std::shared_ptr<ZmqServer> server = device.getHandle()->getZmqServer();
        if (syncType == SyncType::HOST_TO_DEVICE) {
            std::vector<uint8_t> sendData;
            std::size_t dataSize = size * sizeof(T);
            sendData.resize(dataSize);
            std::memcpy(sendData.data(), localBuffer, dataSize);
            server->sendBuffer(std::to_string(getPhysAddr()), sendData);
        } else if (syncType == SyncType::DEVICE_TO_HOST) {
            std::vector<uint8_t> recvData = server->fetchBuffer(std::to_string(getPhysAddr()));
            if ((recvData.size() % sizeof(T)) != 0) {
                throw std::runtime_error("Received emulation buffer size is not aligned to element size");
            }
            const size_t newSize = recvData.size() / sizeof(T);
            if (newSize != size) {
                T* resized = new T[newSize];
                std::memcpy(resized, recvData.data(), recvData.size());
                if (ownsLocalBuffer && localBuffer != nullptr) {
                    delete[] localBuffer;
                }
                localBuffer = resized;
                ownsLocalBuffer = true;
                size = newSize;
            } else {
                std::memcpy(localBuffer, recvData.data(), recvData.size());
            }

        } else {
            throw std::invalid_argument("Invalid sync type");
        }

    } else if (platform == Platform::SIMULATION) {
        std::shared_ptr<ZmqServer> server = device.getHandle()->getZmqServer();
        if (syncType == SyncType::HOST_TO_DEVICE) {
            std::vector<uint8_t> sendData;
            std::size_t dataSize = size * sizeof(T);
            sendData.resize(dataSize);
            std::memcpy(sendData.data(), localBuffer, dataSize);
            server->sendBufferSim(getPhysAddr(), sendData);
        } else if (syncType == SyncType::DEVICE_TO_HOST) {
            std::vector<uint8_t> recvData;
            server->fetchBufferSim(getPhysAddr(), size * sizeof(T), recvData);
            if ((recvData.size() % sizeof(T)) != 0) {
                throw std::runtime_error("Received simulation buffer size is not aligned to element size");
            }
            const size_t newSize = recvData.size() / sizeof(T);
            if (newSize != size) {
                T* resized = new T[newSize];
                std::memcpy(resized, recvData.data(), recvData.size());
                if (ownsLocalBuffer && localBuffer != nullptr) {
                    delete[] localBuffer;
                }
                localBuffer = resized;
                ownsLocalBuffer = true;
                size = newSize;
            } else {
                std::memcpy(localBuffer, recvData.data(), recvData.size());
            }
        } else {
            throw std::invalid_argument("Invalid sync type");
        }
    }
}
template <typename T>
Buffer<T>::Buffer(Buffer&& other) noexcept
    : startAddress(other.startAddress),
      localBuffer(other.localBuffer),
      size(other.size),
      type(other.type),
      hbmPort(other.hbmPort),
      hasPort(other.hasPort),
      device(other.device),
      block(std::move(other.block)),
      view(other.view),
      ownsLocalBuffer(other.ownsLocalBuffer),
      index(other.index) {
    if (block) {
        view = block->getUntypedBuffer();
        localBuffer = static_cast<T*>(view->data());
    }
    other.startAddress = 0;
    other.localBuffer = nullptr;
    other.size = 0;
    other.device = Device{};
    other.view = nullptr;
    other.ownsLocalBuffer = false;
}

template <typename T>
Buffer<T>& Buffer<T>::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        if (ownsLocalBuffer && localBuffer) {
            delete[] localBuffer;
        }

        if (block) {
            device.getHandle()->getAllocator()->deallocate(std::move(block));
        }

        device = other.device;
        size = other.size;
        type = other.type;
        hbmPort = other.hbmPort;
        hasPort = other.hasPort;
        index = other.index;
        startAddress = other.startAddress;
        localBuffer = other.localBuffer;
        block = std::move(other.block);
        view = other.view;
        ownsLocalBuffer = other.ownsLocalBuffer;

        if (block) {
            view = block->getUntypedBuffer();
            localBuffer = static_cast<T*>(view->data());
        }
        other.startAddress = 0;
        other.localBuffer = nullptr;
        other.size = 0;
        other.device = Device{};
        other.view = nullptr;
        other.ownsLocalBuffer = false;
    }
    return *this;
}

template <typename T>
BufferAllocType Buffer<T>::resolveAllocType(MemoryRangeType type, bool hasPort) {
    switch (type) {
        case MemoryRangeType::DDR:
            return BufferAllocType::Ddr;
        case MemoryRangeType::HBM:
            return hasPort ? BufferAllocType::Hbm : BufferAllocType::HbmVnoc;
        case MemoryRangeType::HBM_VNOC:
            return BufferAllocType::HbmVnoc;
        default:
            return BufferAllocType::Ddr;
    }
}

template <typename T>
HBMRegion Buffer<T>::resolveRegion(MemoryRangeType type, bool hasPort, uint8_t port) {
    if (type == MemoryRangeType::HBM && hasPort) {
        if (port > static_cast<uint8_t>(HBMRegion::HBM63)) {
            throw std::out_of_range("HBM port out of range");
        }
        return static_cast<HBMRegion>(port);
    }
    return HBMRegion::NON_HBM;
}

}  // namespace vrt

#endif  // BUFFER_HPP
