/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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
 * @file fpga_device.hpp
 * @brief FPGA backend lowering typed scheduled queues to RP1 graph images.
 *
 * Kernel, reprogram, control, rendezvous, transfer, and boundary payloads
 * lower directly from a QueueProgram. Arguments follow backend ABI metadata;
 * physical scalar and signal slots come from execution-plan resource leases.
 * Barrier bit 31 remains reserved for the lifecycle sentinel.
 */

#ifndef VRT_GRAPH_DEVICE_FPGA_DEVICE_HPP
#define VRT_GRAPH_DEVICE_FPGA_DEVICE_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <vrt/allocator/allocator.hpp>
#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/device/fpga/rp1_program.hpp>
#include <vrt/graph/device/fpga/control_lowering.hpp>
#include <vrt/graph/device/fpga/rp1_bar_window.hpp>
#include <vrt/graph/device/fpga/rp1_submitter.hpp>

namespace vrt::graph {

class Graph;
class FpgaRendezvousLease;
namespace fpga {
class FpgaVbinSpec;
}

/**
 * @brief Resolves a kernel's logical name to its AXI-Lite base address
 *        in R5 address space.
 *
 * The R5 address is the AXI-Lite base; the firmware writes `+0x00` for
 * `ap_start` and each argument at its own register byte offset (from the
 * system_map, e.g. `+0x10`, `+0x1c`, `+0x28` — not necessarily contiguous).
 * Convert from the host-view address in `system_map.xml` via:
 *
 *     r5_addr = xml_addr - 0x0202'0000'0000 + 0x8800'0000
 */
struct FpgaKernelLocation {
    std::uint32_t r5_base_addr = 0;

    /// Optional default `timeout_cycles` for KERNEL_DISPATCH; 0 = firmware default.
    std::uint32_t timeout_cycles = 0;
};

using FpgaKernelLocationLookup =
    std::function<FpgaKernelLocation(const std::string& kernel_name)>;

/**
 * @brief Sentinel slot/value used by the trailing SIGNAL packet in every
 *        lowered RP1 image.
 */
constexpr std::uint32_t kDefaultSentinelSlot  = RP1_MAX_SIGNALS - 1u;
constexpr std::uint32_t kDefaultSentinelValue = 0xD1A1D0DDu;

/**
 * @brief Default timeout for `FpgaDevicePlan::wait()`.
 */
constexpr std::chrono::milliseconds kDefaultFpgaWaitTimeout{3000};

class FpgaDevicePlan;

/**
 * @brief IDevice backend that targets the RP1 command processor.
 *
 * Construction is light-weight; nothing is sent over the BAR until the
 * first lowered executable calls `launch()`. Multiple executables built by the
 * same `FpgaDevice` share a single `Rp1Submitter`, so kernel
 * submissions are serialised across the device by construction.
 *
 * Scheduled queues first become typed `Rp1Command` programs. Finalization
 * allocates barriers and chooses a mainline or autonomous-control image;
 * launch patches scalar values, buffer addresses, and staged PDI addresses.
 *
 * Buffer arguments use region-aware HBM/DDR storage when metadata and a
 * staging device are available, with an RP1 BAR-arena fallback. Arguments are
 * packed as input scalars, input/output buffer addresses, then each RW pair's
 * shared pointer, using exact system_map register offsets when available.
 *
 * Execution and signal resources are leased until hardware is known idle.
 * An indeterminate timeout poisons the device and quarantines live allocations
 * rather than allowing firmware-visible addresses to be reused.
 */
class FpgaDevice : public IDevice,
                   public std::enable_shared_from_this<FpgaDevice> {
   public:
    FpgaDevice(std::string                       id,
               std::shared_ptr<fpga::Rp1BarWindow> window,
               FpgaKernelLocationLookup           lookup);

    FpgaDevice(std::string                       id,
               std::shared_ptr<fpga::Rp1BarWindow> window,
               std::shared_ptr<fpga::FpgaVbinSpec> vbinSpec,
               std::string                       initialImageId = "");

    ~FpgaDevice() override;

    FpgaDevice(const FpgaDevice&)            = delete;
    FpgaDevice& operator=(const FpgaDevice&) = delete;

    // ---- IDevice ----------------------------------------------------

    DeviceType  type() const override { return DeviceType::FPGA; }
    std::string id()   const override { return id_; }

    DeviceCapabilities compilerCapabilities() const override;
    CapabilityDecision evaluateControlCapability(
        const ControlCapabilityRequest& request) const override;
    std::unique_ptr<IDeviceExecutionLease> leaseExecution() override;
    std::unique_ptr<IDeviceResourceLease> leaseResources(
        const std::vector<RendezvousId>& rendezvous,
        const std::vector<ScalarResourceId>& scalars) override;
    std::shared_ptr<IDeviceResourceAccess> resourceAccess() const override;

    std::optional<std::string> resolveMemoryRegion(
        const KernelDescriptor& kernel, const std::string& portName) const override;
    std::function<void()> makeDeviceCopyAction(
        const GraphBuffer& source, const GraphBuffer& target,
        BufferType type, const std::string& sourceRegion,
        const std::string& targetRegion) override;

    std::unique_ptr<IBackendExecutable> lowerQueue(
        const BackendLoweringContext& context) override;
    std::unique_ptr<IBackendExecutable> compileProgram(
        const Rp1QueueProgram& program);

    /**
     * @brief Project a direct RP1 queue program into its packet image.
     *
     * Intended for backend characterization and offline inspection; launch
     * uses the same lowering path through compileProgram().
     */
    fpga::Rp1GraphImage projectProgram(
        const Rp1QueueProgram& program);

    // ---- BAR-backed buffer accessors (also used by CPU↔FPGA bridges) --

    /**
     * @brief Supply or preallocate data for a root-scope FPGA buffer.
     *
     * If @p data is null and @p sizeBytes is non-zero, the buffer is
     * zero-filled.  @p bufferName may be either a plain root-scope name
     * or an already-scoped key (`scope:N:name`).
     */
    void setInputBuffer(const std::string& bufferName,
                        const void*        data,
                        std::size_t        sizeBytes);

    /**
     * @brief Read back a BAR-backed FPGA buffer into host memory.
     */
    void getOutputBuffer(const std::string& bufferName,
                         void*              data,
                         std::size_t        sizeBytes) const;

    /**
     * @brief Returns the current logical size of @p bufferName.
     */
    std::size_t bufferSize(const std::string& bufferName) const;

    /**
     * @brief Returns true iff @p bufferName currently has device storage.
     */
    bool hasBuffer(const std::string& bufferName) const;

    // ---- FpgaDevice-specific configuration --------------------------

    /// Index of the signal slot the auto-generated sentinel SIGNAL node
    /// writes to. Defaults to @c kDefaultSentinelSlot.
    void          setSentinelSlot(std::uint32_t slot);
    std::uint32_t sentinelSlot() const noexcept { return sentinelSlot_; }

    /// Value the sentinel writes; defaults to @c kDefaultSentinelValue.
    /// Tests use this to confirm graph completion.
    void          setSentinelValue(std::uint32_t value);
    std::uint32_t sentinelValue() const noexcept { return sentinelValue_; }

    /// Default per-plan wait timeout used by @c FpgaDevicePlan::wait().
    void                       setWaitTimeout(std::chrono::milliseconds t);
    std::chrono::milliseconds  waitTimeout() const noexcept { return waitTimeout_; }

    /**
     * @brief True when an indeterminate RP1 timeout requires device recovery.
     *
     * A poisoned device rejects reuse and retains its execution/resources for
     * process lifetime so live firmware cannot observe freed allocations.
     */
    bool executionPoisoned() const noexcept {
        return executionPoisoned_.load(std::memory_order_acquire) ||
               (submitter_ && submitter_->poisoned());
    }

    /// Stable 1-based numeric id for @p imageId within this device's vbin
    /// spec, used to populate the RP1 expected-image guard fields on
    /// KERNEL_DISPATCH / PDI_LOAD. Returns 0 when there is no vbin spec
    /// (mock/lookup path) or the image is unknown, which disables the guard.
    std::uint32_t imageNumericId(const std::string& imageId) const;

    /**
     * @brief Provide the VRT hardware device used to stage partial PDIs.
     *
     * Reprogram PDIs must be copied into DDR via QDMA, and RP1 receives the
     * resulting DDR physical address in its PDI_LOAD packet.
     */
    void setPdiStagingDevice(::vrt::Device device);

    // ---- Shared backplane (used by tests and FpgaDevicePlan) --------

    /// Shared submitter; FpgaDevicePlan calls into this.
    std::shared_ptr<fpga::Rp1Submitter> submitter() const noexcept { return submitter_; }

    /// Backing BAR window; exposed for diagnostics and tests.
    std::shared_ptr<fpga::Rp1BarWindow> window() const noexcept { return window_; }

   private:
    friend class FpgaDevicePlan;
    friend class FpgaRendezvousLease;

    /**
     * @brief Canonical backing shared by every alias of a graph buffer.
     *
     * `mem` selects region-aware HBM/DDR storage; a null `mem` selects the
     * RP1 BAR arena at `offset`. Capacity may exceed the current logical size.
     */
    struct BufferRecord {
        std::uint32_t offset = 0;     ///< Window-relative byte offset (BAR mode).
        std::size_t   size = 0;       ///< Logical bytes currently valid.
        std::size_t   capacity = 0;   ///< Allocated bytes.
        BufferType    type = BufferType::U8;
        /// Device-memory backing (HBM/DDR) when the kernel's m_axi region is
        /// known and a staging device is configured.  When null the buffer
        /// lives in the BAR window at @ref offset (mock/test fallback).
        std::shared_ptr<::vrt::Buffer<std::uint8_t>> mem;
    };

    /**
     * @brief Cached QDMA staging for a PDI_LOAD physical address.
     *
     * Plans retain an additional launch pin through completion; poisoned
     * launches transfer that pin to process-lifetime quarantine.
     */
    struct StagedPdiRecord {
        std::shared_ptr<::vrt::Buffer<std::uint8_t>> buffer;
        std::uint64_t                                physAddr = 0;
        std::size_t                                  size = 0;
    };

    static std::string normalizeBufferKey(const std::string& bufferName);
    std::string canonicalBufferKey(const std::string& key) const;

    /// Make @p targetName resolve to the same device buffer (offset/size/region
    /// backing) as @p sourceName.  Used to honour loop/conditional carried-buffer
    /// region boundaries as zero-copy aliases when the body runs autonomously on
    /// the FPGA queue.  Throws if @p sourceName has not been allocated yet.
    void aliasBufferKey(const std::string& targetName, const std::string& sourceName);

    BufferRecord ensureBuffer(const GraphBuffer& buffer, std::size_t sizeBytes);
    /// Unified allocation core (caller must hold @ref bufferMutex_).  Allocates
    /// in device memory when @ref bufferRegion_ has @p key and a staging device
    /// is configured; otherwise carves space from the BAR-window arena.
    BufferRecord ensureBufferByKey(const std::string& key, BufferType type,
                                   std::size_t sizeBytes,
                                   const std::shared_ptr<::vrt::Device>&
                                       stagingDevice);
    std::uint64_t bufferDeviceAddress(
        const GraphBuffer& buffer, std::size_t sizeBytes,
        std::shared_ptr<::vrt::Buffer<std::uint8_t>>& pin);
    /// Walk an RP1 queue program's kernel bindings and record each buffer's
    /// m_axi memory region into @ref bufferRegion_ (keyed by scoped buffer
    /// name) so later allocation lands in the region the kernel can reach.
    void populateBufferRegions(const Rp1QueueProgram& program);
    FpgaKernelLocation resolveKernelLocation(const KernelDescriptor& kernel) const;
    /// Maps each functional-arg name to its AXI-Lite register byte offset
    /// (from the system_map of the active/declared image).  Returns an empty
    /// map when no vbin spec is configured (e.g. the mock lookup path), in
    /// which case the argument packer falls back to a contiguous layout from
    /// `0x10`.
    std::map<std::string, std::uint32_t> kernelArgOffsets(const KernelDescriptor& kernel) const;
    /// AXI-Lite register byte offset of an output scalar port (the register the
    /// kernel writes its result to, captured post-run via RP1_OP_SCALAR_READ).
    /// Resolved from the active/declared image's system_map; falls back to a
    /// conventional offset on the mock/lookup path (no vbin spec).
    std::uint32_t outputScalarRegOffset(const KernelDescriptor& kernel,
                                        const std::string& portName) const;
    /// Map a kernel descriptor's (possibly user-renamed) buffer/scalar port
    /// names to the underlying system_map argument names, by positional
    /// (per-category, declaration-order) correspondence between the
    /// descriptor's IOTypeMap and the spec's canonical IOTypeMap.  This lets
    /// graphs bind ports by arbitrary names (e.g. `image_a_out`) while the
    /// register offsets and memory regions still resolve against the real
    /// HLS arg names (e.g. `out_r`).  Empty on the mock/lookup path.
    std::map<std::string, std::string> descriptorPortToArgName(
        const KernelDescriptor& kernel) const;
    /// Resolve the m_axi memory region a kernel buffer port is wired to (from
    /// the active/declared image's system_map connections), applying the same
    /// `<name>_out` alias the argument packer uses.  Returns `std::nullopt`
    /// when no vbin spec/connection is available (mock path or unconnected
    /// port), in which case the caller falls back to the BAR-window arena.
    std::optional<::vrt::MemoryConfig> resolveBufferRegion(
        const KernelDescriptor& kernel, const std::string& portName) const;
    std::uint64_t stagePdiBytes(
        const std::string& cacheKey,
        const std::vector<std::uint8_t>& bytes,
        std::shared_ptr<::vrt::Buffer<std::uint8_t>>& pin);
    std::uint64_t stagePdiFile(
        const std::string& pdiPath,
        std::shared_ptr<::vrt::Buffer<std::uint8_t>>& pin);
    void setActiveImage(std::string imageId);
    void markActiveImageUnknown();
    std::string activeImageId() const;
    std::shared_ptr<FpgaDevice> sharedSelf();
    void requireExecutionUsable(const char* method) const;
    void poisonExecution() noexcept;
    void quarantineLaunchPins(
        std::vector<std::shared_ptr<::vrt::Buffer<std::uint8_t>>> bufferPins,
        std::vector<std::shared_ptr<::vrt::Buffer<std::uint8_t>>> pdiPins);

    // Device identity, packet transport, and lifecycle sentinel.
    std::string                            id_;
    std::shared_ptr<fpga::Rp1BarWindow>    window_;
    FpgaKernelLocationLookup               lookup_;
    std::shared_ptr<fpga::FpgaVbinSpec>     vbinSpec_;
    std::shared_ptr<fpga::Rp1Submitter>    submitter_;
    std::uint32_t                          sentinelSlot_  = kDefaultSentinelSlot;
    std::uint32_t                          sentinelValue_ = kDefaultSentinelValue;
    bool                                   sentinelConfigLocked_ = false;
    std::chrono::milliseconds              waitTimeout_   = kDefaultFpgaWaitTimeout;
    std::atomic_bool                       executionLeased_{false};
    std::atomic_bool                       executionPoisoned_{false};

    // Canonical storage, aliases, and pre-resolved HBM/DDR placement.
    mutable std::mutex                     bufferMutex_;
    std::map<std::string, BufferRecord>    buffers_;
    std::map<std::string, std::string>      bufferAliases_;
    std::map<std::string, ::vrt::MemoryConfig> bufferRegion_;
    std::uint32_t                          nextBufferOffset_ = 0;

    // One RP1 signal namespace shared by rendezvous and scalar resources.
    mutable std::mutex                     scalarMutex_;
    fpga::SignalSlotAllocator              scalarSlotAlloc_;

    // PDI cache and the VRT device used for DDR/QDMA staging.
    mutable std::mutex                     pdiMutex_;
    std::shared_ptr<::vrt::Device>          pdiStagingDevice_;
    std::map<std::string, StagedPdiRecord>  stagedPdis_;

    // Host image state updated only from reconciled PDI_LOAD completions.
    mutable std::mutex                     imageMutex_;
    std::string                            activeImageId_;

    // Pins retained when RP1 may still own an indeterminate submission.
    mutable std::mutex                     quarantineMutex_;
    std::vector<std::shared_ptr<::vrt::Buffer<std::uint8_t>>>
        quarantinedBufferPins_;
    std::vector<std::shared_ptr<::vrt::Buffer<std::uint8_t>>>
        quarantinedPdiPins_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_FPGA_DEVICE_HPP
