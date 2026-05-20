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
 * @file device.hpp
 * @brief Device class — entry point for V80 hardware interaction.
 */

#ifndef VRT_DEVICE_HPP
#define VRT_DEVICE_HPP

#include <fcntl.h>
#include <json/json.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <sys/file.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#include <vrtd/session.hpp>

#include <vrt/allocator/allocator.hpp>
#include <vrt/kernel.hpp>
#include <vrt/vrt_version.hpp>
#include <vrt/vrtbin.hpp>
#include <vrt/driver/qdma_logic.hpp>
#include <vrt/parser/xml_parser.hpp>
#include <vrt/qdma/qdma_connection.hpp>
#include <vrt/qdma/qdma_intf.hpp>
#include <vrt/utils/logger.hpp>
#include <vrt/utils/platform.hpp>
#include <vrt/utils/zmq_server.hpp>

namespace vrt {

/**
 * @brief Enumeration for device programming types.
 *
 * This enum represents the different methods that can be used to program a device.
 */
enum class ProgramType {
    FLASH,  ///< Program the device using flash memory
    JTAG    ///< Program the device using JTAG interface
};

/**
 * @brief Path to the JTAG programming script.
 *
 * This macro defines the path to the shell script used for programming devices via JTAG.
 */
#define JTAG_PROGRAM_PATH "/usr/local/vrt/jtag_program.sh "

/**
 * @brief Path to the QDMA queue setup script.
 *
 * This macro defines the path to the shell script used for setting up QDMA queues.
 */

/**
 * @brief Delay in microseconds for partial boot process.
 *
 * This constant defines the delay time in microseconds that the system
 * will wait during the partial boot process (4 seconds).
 */
#define DELAY_PARTIAL_BOOT (4 * 1000 * 1000)

namespace impl {
/**
 * @brief Class representing a device.
 */
class Device {
    static constexpr uint64_t QDMA_LOGIC_BASE = 0x20100020000;  ///< Base address for QDMA logic
    static constexpr uint32_t QDMA_LOGIC_OFFSET = 0x1000;       /// Offset for QDMA logic
    static constexpr uint32_t CLOCK_MAX_FREQ = 333333333;
    uint8_t bar = 0;                                            ///< Base Address Register (BAR)
    uint64_t offset = 0;                                        ///< Offset for memory operations
    uint16_t pci_bdf = 0;                                       ///< PCI Bus:Device.Function identifier
    std::string systemMap;                                      ///< Path to the system map file
    std::string bdf;                                            ///< Bus:Device.Function identifier
    std::string bdfFull;                                        ///< Domain:Bus:Device.Function identifier
    std::string pdiPath;                                        ///< Path to the PDI file
    std::vector<std::string> pdiPaths;                          ///< Paths to PDI files discovered in archive
    Vrtbin vrtbin;                                              ///< Vrtbin object for handling VRTBIN operations
    uint64_t clockFreq = 0;                                     ///< Clock frequency
    ProgramType programType{};                                  ///< Type of programming
    std::map<std::string, Kernel> kernels;                      ///< Map of kernel names to Kernel objects
    Allocator* allocator = nullptr;                             ///< Allocator object
    Platform platform{};                                        ///< Platform information
    std::shared_ptr<ZmqServer> zmqServer;                       ///< ZeroMQ server object
    std::vector<QdmaConnection> qdmaConnections;                ///< Vector of QDMA connections
    std::vector<QdmaIntf*> qdmaIntfs;                           ///< Vector of QDMA interfaces for streaming
    std::shared_ptr<vrtd::Session> vrtdSession;                 ///< vrtd session for hardware access
    std::optional<vrtd::Device> vrtdDevice;                     ///< vrtd device handle (requires session)
    std::thread runtimeThread;                                  ///< sw_emu/sim runtime launcher thread
    bool cleanupDone = false;                                   ///< Guard to make cleanup idempotent
   public:
    QdmaIntf qdmaIntf;  ///< QDMA interface object

    /**
     * @brief Constructor for Device.
     * @param bdf The Bus:Device.Function identifier.
     * @param vrtbinPath The path to the VRTBIN file.
     * @param program Flag indicating whether to program the device.
     */
    Device(const std::string& bdf, const std::string& vrtbinPath, bool program = true,
           ProgramType programType = ProgramType::FLASH);

    Device() = delete;
    Device(Device&) = delete;
    Device(Device&&) = delete;

    /**
     * @brief Gets a kernel by name.
     * @param name The name of the kernel.
     * @return The Kernel object.
     */
    vrt::Kernel getKernel(const std::string& name);

    /**
     * @brief Gets the Bus:Device.Function identifier.
     * @return The Bus:Device.Function identifier.
     */
    std::string getBdf();

    /**
     * @brief Programs the device.
     */
    void programDevice();

    /**
     * @brief Destructor for Device.
     */
    ~Device();

    /**
     * @brief Parses the system map file.
     */
    void parseSystemMap();

    /**
     * @brief Cleans up the device.
     */
    void cleanup();
    /**
     * @brief Sets device clock frequency.
     */
    void setFrequency(uint64_t freq);

    /**
     * @brief Gets the clock frequency.
     */
    uint64_t getFrequency();

    /**
     * @brief Gets the maximum frequency.
     */
    uint64_t getMaxFrequency();

    /**
     * @brief Finds the VRTBIN type from system map.
     */
    void findVrtbinType();

    /**
     * @brief Finds the platform from system map.
     */
    void findPlatform();

    /**
     * @brief Gets the platform.
     */
    Platform getPlatform();

    /**
     * @brief Gets the ZMQ server.
     */
    std::shared_ptr<ZmqServer> getZmqServer();

    /**
     * @brief Gets the Allocator instance.
     */
    Allocator* getAllocator();

    /**
     * @brief Gets the underlying vrtd device handle (hardware only).
     */
    vrtd::Device& getVrtdDevice();

    /**
     * @brief Gets the underlying vrtd device handle (hardware only).
     */
    const vrtd::Device& getVrtdDevice() const;

    /**
     * @brief Gets the QDMA connections.
     */
    std::vector<QdmaConnection> getQdmaConnections();

    // /**
    //  * @brief Gets the QDMA logic instance.
    //  */
    // QdmaLogic* getQdmaLogic();

    /**
     * @brief Gets the QDMA streaming interfaces.
     */
    std::vector<QdmaIntf*> getQdmaInterfaces();
};
}  // namespace impl

/**
 * @brief Public handle to a V80 device with move semantics.
 *
 * Thin wrapper around impl::Device providing the user-facing API for
 * device initialization, kernel retrieval, frequency control, and cleanup.
 */
class Device {
    std::shared_ptr<impl::Device> handle;

   public:
    /**
     * @brief Default constructor for an empty device.
     */
    Device() = default;

    /**
     * @brief Constructor for Device.
     * @param bdf The Bus:Device.Function identifier.
     * @param vrtbinPath The path to the VRTBIN file.
     * @param program Flag indicating whether to program the device.
     */
    Device(const std::string& bdf, const std::string& vrtbinPath, bool program = true,
           ProgramType programType = ProgramType::FLASH) : handle(new impl::Device(bdf, vrtbinPath, program, programType)) {}

    /**
     * @brief Constructor from a device handle.
     * @param handle The handle device handle
     */
    Device(std::shared_ptr<impl::Device> handle) : handle(handle) {}

    /**
     * @brief Default copy constructor
     */
    Device(const Device&) = default;

    /**
     * @brief Default copy assignment
     */
    Device& operator=(const Device&) = default;

    /**
     * @brief Default move constructor
     */
    Device(Device&&) = default;

    /**
     * @brief Default move assignment
     */
    Device& operator=(Device&&) = default;

    /**
     * @brief Gets a kernel by name.
     * @param name The name of the kernel.
     * @return The Kernel object.
     */
    vrt::Kernel getKernel(const std::string& name) {
        return handle->getKernel(name);
    }

    /**
     * @brief Gets the Bus:Device.Function identifier.
     * @return The Bus:Device.Function identifier.
     */
    std::string
    getBdf() {
        return handle->getBdf();
    }

    /**
     * @brief Sets device clock frequency.
     */
    void setFrequency(uint64_t freq) { handle->setFrequency(freq); }

    /**
     * @brief Cleans up device-side resources (simulation/emulation/hardware helpers).
     */
    void cleanup() {
        if (handle) {
            handle->cleanup();
        }
    }

    /**
     * @brief Gets the clock frequency.
     */
    uint64_t getFrequency() { return handle->getFrequency(); }

    /**
     * @brief Gets the maximum frequency.
     */
    uint64_t getMaxFrequency() { return handle->getMaxFrequency(); }

    /**
     * @brief Gets the platform.
     */
    Platform getPlatform() { return handle->getPlatform(); }

    /**
     * @brief Return the internal device handle.
     */
    std::shared_ptr<impl::Device> getHandle() const { return handle; }
};

}  // namespace vrt

#endif  // VRT_DEVICE_HPP
