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
#include <functional>
#include <vector>
#include <stddef.h>
#include <stdint.h>

#include <vrtd/bar.hpp>
#include <vrtd/buffer.hpp>
#include <vrtd/qdma_qpair.hpp>
#include <vrtd/vrtd.h>

namespace vrtd {

enum class ClockRegion : uint32_t {
    Service = VRTD_CLOCK_REGION_SERVICE,
    User = VRTD_CLOCK_REGION_USER,
};

enum class HotplugOp : uint8_t {
    Rescan = VRTD_DEVICE_HOTPLUG_OP_RESCAN,
    Remove = VRTD_DEVICE_HOTPLUG_OP_REMOVE,
    ToggleSbr = VRTD_DEVICE_HOTPLUG_OP_TOGGLE_SBR,
    Hotplug = VRTD_DEVICE_HOTPLUG_OP_HOTPLUG,
    ResetSequence = VRTD_DEVICE_HOTPLUG_OP_RESET_SEQUENCE,
};

inline constexpr uint8_t HotplugFunctionAll = VRTD_DEVICE_HOTPLUG_FUNCTION_ALL;

enum class ShellType : uint8_t {
    Unknown = VRTD_SHELL_UNKNOWN,
    Service = VRTD_SHELL_SERVICE,
    Compute = VRTD_SHELL_COMPUTE,
};

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
 * A @c Device carries its device number, name, and PCI metadata and routes operations back
 * through its originating @c Session.
 *
 * @par Lifetime
 * A @c Device becomes invalid if its originating @c Session is closed or moved.
 * Any subsequent member call will throw.
 *
 * @par Thread safety
 * Methods are thread-safe and may be called concurrently; they synchronize
 * on the originating @c Session.
 */
class Device {
public:
    ~Device() = default;

    Device(const Device&)                = default;
    Device& operator=(const Device&)     = default;
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
     * @brief Shell state reported by vrtd when this Device handle was created.
     */
    ShellType getShellType() const noexcept;

    /**
     * @brief Whether vrtd reported this device as JTAG-booted.
     */
    bool isJtag() const noexcept;

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
     * The returned @c Bar becomes invalid if the originating @c Session is
     * later closed or moved.
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
     * The returned @c QdmaQpair becomes invalid if the originating
     * @c Session is later closed or moved.
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
     * @brief Convenience helper for DDR allocations.
     */
    Buffer openDdrBuffer(uint64_t size, BufferAllocDir allocDir = BufferAllocDir::Bidirectional,
                         MmChannel mmChannel = MmChannel::Auto) const {
        return openBuffer(BufferAllocType::Ddr, size, 0, allocDir, mmChannel);
    }

    /**
     * @brief Convenience helper for HBM allocations (fixed region).
     */
    Buffer openHbmBuffer(uint32_t region,
                         uint64_t size,
                         BufferAllocDir allocDir = BufferAllocDir::Bidirectional,
                         MmChannel mmChannel = MmChannel::Auto) const {
        return openBuffer(BufferAllocType::Hbm, size, region, allocDir, mmChannel);
    }

    /**
     * @brief Convenience helper for HBM VNOC allocations.
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
     * physical function (0-7). Use Session::hotplugRescan() for bus rescan.
     *
     * @param op       One of HotplugOp.
     * @param function PCI function number (0-7), or HotplugFunctionAll where allowed.
     * @throws vrtd::Error on error.
     */
    void hotplugOp(HotplugOp op, uint8_t function = 0) const;

    /**
     * @brief Convenience helper for remove.
     * @param function PCI function number (0-7), or HotplugFunctionAll for all PFs.
     */
    void hotplugRemove(uint8_t function = HotplugFunctionAll) const {
        hotplugOp(HotplugOp::Remove, function);
    }

    /**
     * @brief Convenience helper for SBR toggle.
     * @param function PCI function number (0-7). Required.
     */
    void hotplugToggleSbr(uint8_t function) const {
        hotplugOp(HotplugOp::ToggleSbr, function);
    }

    /**
     * @brief Convenience helper for a remove+rescan hotplug cycle.
     * @param function PCI function number (0-7), or HotplugFunctionAll for all PFs.
     */
    void hotplug(uint8_t function = HotplugFunctionAll) const {
        hotplugOp(HotplugOp::Hotplug, function);
    }

    /**
     * @brief Reset the board and boot the requested shell.
     * @param shellType Hardware shell to boot.
     */
    void resetSequence(ShellType shellType = ShellType::Service) const;

    /**
     * @brief Set vrtd's in-memory shell/JTAG state for this device.
     * @param shellType Hardware shell believed to be booted.
     * @param jtag Whether the device was booted from a JTAG-loaded image.
     */
    void setShellState(ShellType shellType, bool jtag) const;

    /**
     * @brief Perform a design writer transfer using an input file descriptor.
     *
     * The daemon takes ownership of the FD and blocks until the transfer completes.
     *
     * @throws vrtd::Error on error.
     */
    void designWrite(int input_fd, ShellType requiredShell = ShellType::Service) const;

    /**
     * @brief Perform a design writer transfer from a file path.
     *
     * Convenience helper that opens the path and passes the FD to the daemon.
     *
     * @throws vrtd::Error on error.
     */
    void designWriteFile(std::string_view path,
                         ShellType requiredShell = ShellType::Service) const;

    /**
     * @brief Program a PDI into cfgmem and reset into the programmed partition.
     *
     * The daemon reads @p input_fd, programs @p partition on @p bootDevice
     * through AMI, selects that partition for boot, and performs the vrtd-managed
     * reset sequence.
     *
     * @throws vrtd::Error on error.
     */
    void cfgmemProgram(int input_fd, uint8_t bootDevice, uint32_t partition) const;

    /**
     * @brief Program a PDI file into cfgmem.
     *
     * Convenience helper that opens @p path and passes the FD to the daemon.
     *
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

    // TODO: getClockRateMHz()

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
     * @brief Convenience helper for service region get.
     * @see getClockRate
     */
    uint32_t getServiceClockRate() const {
        return getClockRate(ClockRegion::Service);
    }

    /**
     * @brief Convenience helper for service region set.
     * @see setClockRate
     */
    uint32_t setServiceClockRate(uint32_t rate_hz) const {
        return setClockRate(ClockRegion::Service, rate_hz);
    }

    /**
     * @brief Convenience helper for user region get.
     * @see getClockRate
     */
    uint32_t getUserClockRate() const {
        return getClockRate(ClockRegion::User);
    }

    /**
     * @brief Convenience helper for user region set.
     * @see setClockRate
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


    // reconfigureUserRegion()
    // reconfigureServiceRegion()
    // setUserRegionFrequency()
    // setServiceRegionFrequency()


private:
    // Only allow the Session class to generate this class
    friend class Session;
    Device(uint32_t num,
           std::string_view name,
           std::string_view bdf,
           uint16_t vendorId,
           uint16_t deviceId,
           uint16_t subsystemVendorId,
           uint16_t subsystemDeviceId,
           ShellType shellType,
           bool jtag,
           std::function<Bar(const Device&, uint8_t)> fGetBar,
           std::function<QdmaQpair(const Device&, const struct slash_qdma_qpair_add&)> fCreateQdmaQpair,
           std::function<Buffer(const Device&, BufferAllocType, uint64_t, uint64_t, BufferAllocDir, MmChannel)> fOpenBuffer,
           std::function<Buffer(const Device&, uint64_t, uint64_t, BufferAllocDir, MmChannel)> fOpenBufferRaw,
           std::function<void(const Device&, HotplugOp, uint8_t)> fHotplugOp,
           std::function<void(const Device&, ShellType)> fResetSequence,
           std::function<void(const Device&, ShellType, bool)> fSetShellState,
           std::function<void(const Device&, int, ShellType)> fDesignWrite,
           std::function<void(const Device&, std::string_view, ShellType)> fDesignWriteFile,
           std::function<void(const Device&, int, uint8_t, uint32_t)> fCfgmemProgram,
           std::function<void(const Device&, std::string_view, uint8_t, uint32_t)> fCfgmemProgramFile,
           std::function<uint32_t(const Device&, ClockRegion)> fGetClockRate,
           std::function<uint32_t(const Device&, ClockRegion, uint32_t)> fSetClockRate,
           std::function<std::vector<SensorEntry>(const Device&)> fGetSensorInfo);

    uint32_t num;
    std::string name;
    std::string bdf;
    uint16_t vendorId = 0;
    uint16_t deviceId = 0;
    uint16_t subsystemVendorId = 0;
    uint16_t subsystemDeviceId = 0;
    ShellType shellType = ShellType::Unknown;
    bool jtag = false;

    std::function<Bar(const Device&, uint8_t)> fGetBar;
    std::function<QdmaQpair(const Device&, const struct slash_qdma_qpair_add&)> fCreateQdmaQpair;
    std::function<Buffer(const Device&, BufferAllocType, uint64_t, uint64_t, BufferAllocDir, MmChannel)> fOpenBuffer;
    std::function<Buffer(const Device&, uint64_t, uint64_t, BufferAllocDir, MmChannel)> fOpenBufferRaw;
    std::function<void(const Device&, HotplugOp, uint8_t)> fHotplugOp;
    std::function<void(const Device&, ShellType)> fResetSequence;
    std::function<void(const Device&, ShellType, bool)> fSetShellState;
    std::function<void(const Device&, int, ShellType)> fDesignWrite;
    std::function<void(const Device&, std::string_view, ShellType)> fDesignWriteFile;
    std::function<void(const Device&, int, uint8_t, uint32_t)> fCfgmemProgram;
    std::function<void(const Device&, std::string_view, uint8_t, uint32_t)> fCfgmemProgramFile;
    std::function<uint32_t(const Device&, ClockRegion)> fGetClockRate;
    std::function<uint32_t(const Device&, ClockRegion, uint32_t)> fSetClockRate;
    std::function<std::vector<SensorEntry>(const Device&)> fGetSensorInfo;
};

}

#endif // VRTD_DEVICE_HPP
