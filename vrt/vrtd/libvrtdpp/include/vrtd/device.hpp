/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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

#ifndef VRTD_DEVICE_HPP
#define VRTD_DEVICE_HPP

#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <stddef.h>
#include <stdint.h>

#include <vrtd/bar.hpp>
#include <vrtd/buffer.hpp>
#include <vrtd/qdma_qpair.hpp>
#include <vrtd/vrtd.h>

namespace vrtd {

namespace detail {
class Connection;
}

/** @brief Device clock domain selected by clock-control operations. */
enum class ClockRegion : uint32_t {
    Service = VRTD_CLOCK_REGION_SERVICE, ///< Static service-region clock.
    User = VRTD_CLOCK_REGION_USER,       ///< Reconfigurable user-region clock.
};

/** @brief PCIe hotplug/reset operation applied by Device::hotplugOp(). */
enum class HotplugOp : uint8_t {
    Rescan = VRTD_DEVICE_HOTPLUG_OP_RESCAN, ///< Rescan the PCI bus.
    Remove = VRTD_DEVICE_HOTPLUG_OP_REMOVE, ///< Remove selected PCI functions.
    ToggleSbr = VRTD_DEVICE_HOTPLUG_OP_TOGGLE_SBR, ///< Toggle secondary-bus reset.
    Hotplug = VRTD_DEVICE_HOTPLUG_OP_HOTPLUG, ///< Remove then rescan.
    ResetSequence = VRTD_DEVICE_HOTPLUG_OP_RESET_SEQUENCE, ///< Run managed reset.
};

/** Select all V80 physical functions where a hotplug operation permits it. */
inline constexpr uint8_t HotplugFunctionAll = VRTD_DEVICE_HOTPLUG_FUNCTION_ALL;

/**
 * @brief A single sensor reading returned by Device::getSensorInfo().
 */
struct SensorEntry {
    std::string name;   ///< Sensor name (e.g., "vccint").
    uint8_t type;       ///< Sensor type bitmask (1=temp, 2=current, 4=voltage, 8=power).
    uint8_t status;     ///< Sensor status code (0x01 = OK).
    int8_t unitMod;     ///< Unit modifier exponent (e.g., -3 for milli-).
    int32_t value;      ///< Sensor reading (apply 10^unitMod to get base unit value).
};

/**
 * @brief Value-type handle describing a vrtd device.
 *
 * A @c Device carries its device number, name, and PCI metadata and retains
 * the shared vrtd connection used for device operations.
 *
 * @par Lifetime
 * Moving or destroying the originating @c Session does not invalidate a
 * Device. Explicitly closing the shared connection does.
 *
 * @par Thread safety
 * Methods are thread-safe and may be called concurrently; they synchronize
 * on the shared connection.
 */
class Device {
public:
    /** Release this metadata handle and its shared connection reference. */
    ~Device() = default;

    /** Device handles are copyable views of the same shared connection. */
    Device(const Device&)                = default;
    Device& operator=(const Device&)     = default;

    /** Moving transfers the shared connection and cached metadata. */
    Device(Device&&) noexcept            = default;
    Device& operator=(Device&&) noexcept = default;

    /**
     * @brief Zero-based device index as seen by vrtd.
     */
    uint32_t getNum() const noexcept;

    /**
     * @brief Human-readable device name.
     *
     * Stable for the lifetime of the @c Device object.
     */
    const std::string& getName() const noexcept;

    /**
     * @brief PCI BDF string for this device.
     */
    const std::string& getBdf() const noexcept;

    /**
     * @brief PCI vendor ID.
     */
    uint16_t getVendorId() const noexcept;

    /**
     * @brief PCI device ID.
     */
    uint16_t getDeviceId() const noexcept;

    /**
     * @brief PCI subsystem vendor ID.
     */
    uint16_t getSubsystemVendorId() const noexcept;

    /**
     * @brief PCI subsystem device ID.
     */
    uint16_t getSubsystemDeviceId() const noexcept;

    /**
     * @brief Access a device BAR by index.
     *
     * @param num BAR index.
     * @return Metadata handle for the requested BAR.
     * @throws vrtd::Error on error (e.g., invalid index or unusable session).
     *
     * @par Notes
     * The returned @c Bar retains the same shared connection.
     */
    Bar getBar(uint8_t num) const;

    /**
     * @brief Create a QDMA qpair on this device.
     *
     * Returns an owning @c QdmaQpair that will automatically delete
     * the qpair on destruction.
     *
     * @param cfg Qpair configuration parameters. The returned qpair
     *            exposes @c getQid().
     * @return An owning @c QdmaQpair.
     * @throws vrtd::Error on error.
     *
     * @par Notes
     * The returned @c QdmaQpair retains the same shared connection.
     */
    QdmaQpair createQdmaQpair(const struct slash_qdma_qpair_add& cfg) const;

    /**
     * @brief Open a buffer (allocation + QDMA qpair) on this device.
     *
     * Returns an owning @c Buffer that closes the returned FD on destruction.
     *
     * @param allocType Allocation type for the buffer.
     * @param size      Requested size in bytes.
     * @param allocArg  Allocation argument (HBM region index for HBM).
     * @param allocDir  QDMA transfer direction.
     * @param mmChannel AXI-MM/NoC channel selection (defaults to auto).
     * @return An owning @c Buffer.
     * @throws vrtd::Error on error.
     */
    Buffer openBuffer(BufferAllocType allocType,
                      uint64_t size,
                      uint64_t allocArg = 0,
                      BufferAllocDir allocDir = BufferAllocDir::Bidirectional,
                      MmChannel mmChannel = MmChannel::Auto) const;

    /**
     * @brief Allocate a DDR buffer through openBuffer().
     *
     * @param size Requested byte count.
     * @param allocDir Permitted transfer direction.
     * @param mmChannel AXI-MM/NoC channel selection.
     * @return Owning DDR buffer.
     * @throws vrtd::Error if allocation fails.
     */
    Buffer openDdrBuffer(uint64_t size, BufferAllocDir allocDir = BufferAllocDir::Bidirectional,
                         MmChannel mmChannel = MmChannel::Auto) const {
        return openBuffer(BufferAllocType::Ddr, size, 0, allocDir, mmChannel);
    }

    /**
     * @brief Allocate a buffer from one fixed HBM region.
     *
     * @param region HBM region index passed as the allocation argument.
     * @param size Requested byte count.
     * @param allocDir Permitted transfer direction.
     * @param mmChannel AXI-MM/NoC channel selection.
     * @return Owning HBM buffer.
     * @throws vrtd::Error if the region or allocation is invalid.
     */
    Buffer openHbmBuffer(uint32_t region,
                         uint64_t size,
                         BufferAllocDir allocDir = BufferAllocDir::Bidirectional,
                         MmChannel mmChannel = MmChannel::Auto) const {
        return openBuffer(BufferAllocType::Hbm, size, region, allocDir, mmChannel);
    }

    /**
     * @brief Allocate an HBM buffer using daemon VNOC placement.
     *
     * @param size Requested byte count.
     * @param allocDir Permitted transfer direction.
     * @param mmChannel AXI-MM/NoC channel selection.
     * @return Owning HBM VNOC buffer.
     * @throws vrtd::Error if allocation fails.
     */
    Buffer openHbmVnocBuffer(uint64_t size,
                             BufferAllocDir allocDir = BufferAllocDir::Bidirectional,
                             MmChannel mmChannel = MmChannel::Auto) const {
        return openBuffer(BufferAllocType::HbmVnoc, size, 0, allocDir, mmChannel);
    }

    /**
     * @brief Open a raw buffer (QDMA qpair at caller-specified device address, bypasses allocator).
     *
     * Requires the @c raw-mem-access permission on this device.
     * The caller is responsible for ensuring the address is valid and not in use.
     *
     * @param phys_addr Device physical address.
     * @param size      Size in bytes.
     * @param allocDir  QDMA transfer direction.
     * @param mmChannel AXI-MM/NoC channel selection (defaults to auto).
     * @return An owning @c Buffer.
     * @throws vrtd::Error on error.
     */
    Buffer openRawBuffer(uint64_t phys_addr,
                         uint64_t size,
                         BufferAllocDir allocDir = BufferAllocDir::Bidirectional,
                         MmChannel mmChannel = MmChannel::Auto) const;

    /**
     * @brief Perform a PCIe hotplug operation for this device.
     *
     * For ResetSequence, @p function is ignored. For Remove and Hotplug,
     * @p function selects the PCI physical function (0-7) or
     * HotplugFunctionAll for all V80 PFs. ToggleSbr requires a single PCI
     * physical function (0-7). Rescan is global and ignores this Device and
     * @p function; prefer Session::hotplugRescan() for that operation.
     *
     * @param op       One of HotplugOp.
     * @param function PCI function number (0-7), or HotplugFunctionAll where allowed.
     * @throws vrtd::Error on error.
     */
    void hotplugOp(HotplugOp op, uint8_t function = 0) const;

    /**
     * @brief Convenience helper for remove.
     *
     * @param function PCI function number (0-7), or HotplugFunctionAll for all PFs.
     * @throws vrtd::Error if removal fails.
     */
    void hotplugRemove(uint8_t function = HotplugFunctionAll) const {
        hotplugOp(HotplugOp::Remove, function);
    }

    /**
     * @brief Convenience helper for SBR toggle.
     *
     * @param function PCI function number (0-7). Required.
     * @throws vrtd::Error if the selector is invalid or the reset fails.
     */
    void hotplugToggleSbr(uint8_t function) const {
        hotplugOp(HotplugOp::ToggleSbr, function);
    }

    /**
     * @brief Convenience helper for a remove+rescan hotplug cycle.
     *
     * @param function PCI function number (0-7), or HotplugFunctionAll for all PFs.
     * @throws vrtd::Error if removal or rescan fails.
     */
    void hotplug(uint8_t function = HotplugFunctionAll) const {
        hotplugOp(HotplugOp::Hotplug, function);
    }

    /**
     * @brief Perform a design writer transfer using an input file descriptor.
     *
     * The call blocks until the daemon finishes the transfer. SCM_RIGHTS
     * duplicates the descriptor, so the caller retains ownership of @p input_fd.
     *
     * @param input_fd Readable design image descriptor.
     * @throws vrtd::Error on error.
     */
    void designWrite(int input_fd) const;

    /**
     * @brief Perform a design writer transfer from a file path.
     *
     * Convenience helper that opens the path and passes the FD to the daemon.
     *
     * @param path Design image opened and closed internally by libvrtd.
     * @throws vrtd::Error on error.
     */
    void designWriteFile(std::string_view path) const;

    /**
     * @brief Program a PDI into cfgmem and reset into the programmed partition.
     *
     * The daemon reads @p input_fd, programs @p partition on @p bootDevice
     * through AMI, selects that partition for boot, and performs the vrtd-managed
     * reset sequence.
     *
     * SCM_RIGHTS preserves caller ownership of @p input_fd.
     *
     * @param input_fd Readable PDI descriptor.
     * @param bootDevice AMI boot-device selector.
     * @param partition Flash partition to program and select.
     * @throws vrtd::Error on error.
     */
    void cfgmemProgram(int input_fd, uint8_t bootDevice, uint32_t partition) const;

    /**
     * @brief Program a PDI file into cfgmem.
     *
     * Opens @p path, programs @p partition through AMI, selects it for boot,
     * and performs the vrtd-managed reset sequence.
     *
     * @param path PDI file opened and closed internally by libvrtd.
     * @param bootDevice AMI boot-device selector.
     * @param partition Flash partition to program and select.
     * @throws vrtd::Error on error.
     */
    void cfgmemProgramFile(std::string_view path, uint8_t bootDevice, uint32_t partition) const;

    /**
     * @brief Get the clock rate for a region.
     *
     * @param region Clock region.
     * @return Current rate in Hz.
     * @throws vrtd::Error on error.
     */
    uint32_t getClockRate(ClockRegion region) const;

    /**
     * @brief Set the clock rate for a region.
     *
     * @param region Clock region.
     * @param rate_hz Requested rate in Hz.
     * @return Achieved rate in Hz.
     * @throws vrtd::Error on error.
     */
    uint32_t setClockRate(ClockRegion region, uint32_t rate_hz) const;

    /**
     * @brief Return the service-region clock rate in hertz.
     *
     * @throws vrtd::Error if the query fails.
     */
    uint32_t getServiceClockRate() const {
        return getClockRate(ClockRegion::Service);
    }

    /**
     * @brief Set the service-region clock and return the achieved hertz value.
     *
     * @param rate_hz Requested rate in hertz.
     * @throws vrtd::Error if the rate cannot be applied.
     */
    uint32_t setServiceClockRate(uint32_t rate_hz) const {
        return setClockRate(ClockRegion::Service, rate_hz);
    }

    /**
     * @brief Return the user-region clock rate in hertz.
     *
     * @throws vrtd::Error if the query fails.
     */
    uint32_t getUserClockRate() const {
        return getClockRate(ClockRegion::User);
    }

    /**
     * @brief Set the user-region clock and return the achieved hertz value.
     *
     * @param rate_hz Requested rate in hertz.
     * @throws vrtd::Error if the rate cannot be applied.
     */
    uint32_t setUserClockRate(uint32_t rate_hz) const {
        return setClockRate(ClockRegion::User, rate_hz);
    }

    /**
     * @brief Query all sensor readings for this device.
     *
     * Returns current values and statuses for all sensors (temperature,
     * power, voltage, current) discovered via the AMI interface.
     *
     * @return Vector of sensor entries.
     * @throws vrtd::Error on error.
     */
    std::vector<SensorEntry> getSensorInfo() const;

private:
    friend class Session;

    /**
     * @brief Construct an owned device metadata snapshot.
     *
     * Session supplies a shared connection plus bounded copies of the daemon's
     * name and PCI identity fields. Derived handles retain the same connection.
     */
    Device(std::shared_ptr<detail::Connection> connection,
           uint32_t num,
           std::string_view name,
           std::string_view bdf,
           uint16_t vendorId,
           uint16_t deviceId,
           uint16_t subsystemVendorId,
           uint16_t subsystemDeviceId);

    /** Shared transport used by all connection-dependent operations. */
    std::shared_ptr<detail::Connection> connection;
    uint32_t num; ///< Zero-based daemon device index.
    std::string name; ///< Owned human-readable daemon name.
    std::string bdf; ///< Owned board-level PCI BDF.
    uint16_t vendorId = 0; ///< PCI vendor identifier.
    uint16_t deviceId = 0; ///< PCI device identifier.
    uint16_t subsystemVendorId = 0; ///< PCI subsystem vendor identifier.
    uint16_t subsystemDeviceId = 0; ///< PCI subsystem device identifier.

};

}

#endif // VRTD_DEVICE_HPP
