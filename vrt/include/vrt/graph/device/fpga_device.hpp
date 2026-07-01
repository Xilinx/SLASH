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
 * @brief FpgaDevice — IDevice backend that lowers DGraphs to RP1 graphs.
 *
 * `compilePlan()` lowers a DGraph into a packed RP1 node program submitted to
 * the R5 command processor:
 *   - `CompiledKernelNode` -> `RP1_OP_KERNEL_DISPATCH`. Kernel arguments are
 *     taken from `IOMap` scalar bindings, packed in the order declared by the
 *     kernel's `IOTypeMap::inputScalars`. Constants are baked in at compile
 *     time; global-variable bindings are resolved at `launch()` time via the
 *     per-graph scalar map.
 *   - `CompiledReprogramNode` -> `RP1_OP_PDI_LOAD` (partial reconfiguration of
 *     the user region), with the PDI staged through QDMA/DDR.
 *   - `CompiledLoopNode` / `CompiledConditionalNode` -> autonomous
 *     `RP1_OP_LOOP` / branch packets when the whole body lowers to the FPGA,
 *     or a split Authority/Follower rendezvous when a peer (e.g. CPU) queue
 *     drives the control decision.
 *   - `CompiledSignalNode` / `CompiledWaitNode` -> cross-queue signal/wait
 *     rendezvous over host-visible BAR signal slots.
 *   - `CompiledBridgeOpNode` -> data movement via the registered bridge.
 *   - Barrier bits are allocated per reset domain; bit 31 of the lifecycle
 *     bucket is reserved for the trailing sentinel `RP1_OP_SIGNAL` that writes
 *     `kDefaultSentinelValue` into `kDefaultSentinelSlot` once every leaf node
 *     completes.
 *
 * Current limitations — these make `compilePlan()` throw a descriptive
 * diagnostic rather than silently misbehave:
 *   - Top-level `CompiledBoundaryNode`s are not yet supported (boundaries are
 *     only handled inside loop/conditional child DGraphs).
 *   - A loop body that mixes CPU and FPGA kernels, or an FPGA loop with no
 *     FPGA body nodes, is not yet supported.
 *   - Control-flow outputs published to a device other than the one that
 *     produced them are not yet executable.
 */

#ifndef VRT_GRAPH_DEVICE_FPGA_DEVICE_HPP
#define VRT_GRAPH_DEVICE_FPGA_DEVICE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <vrt/allocator/allocator.hpp>
#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/device/fpga/control_lowering.hpp>
#include <vrt/graph/device/fpga/rp1_bar_window.hpp>
#include <vrt/graph/device/fpga/rp1_submitter.hpp>

namespace vrt::graph {

class Graph;
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
 * @brief Sentinel slot/value used by the trailing SIGNAL node in every
 *        compiled FPGA plan.  Matches the convention used by
 *        `examples/rp1_bringup/cmd_diamond`.
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
 * first compiled plan calls `launch()`.  Multiple plans built by the
 * same `FpgaDevice` share a single `Rp1Submitter`, so kernel
 * submissions are serialised across the device by construction.
 *
 * Buffer arguments use the RP1-visible DDR window as a staging arena.
 * Kernel arguments are packed as all scalar inputs first, followed by
 * 64-bit DDR addresses for `IOTypeMap::inputs`,
 * `IOTypeMap::outputs`, and then each RW buffer pair's input and
 * output addresses in declaration order.
 */
class FpgaDevice : public IDevice {
   public:
    FpgaDevice(std::string                       id,
               std::shared_ptr<fpga::Rp1BarWindow> window,
               FpgaKernelLocationLookup           lookup,
               std::uint32_t                      cq_size = fpga::kDefaultCqSize);

    FpgaDevice(std::string                       id,
               std::shared_ptr<fpga::Rp1BarWindow> window,
               std::shared_ptr<fpga::FpgaVbinSpec> vbinSpec,
               std::string                       initialImageId = "",
               std::uint32_t                     cq_size = fpga::kDefaultCqSize);

    ~FpgaDevice() override;

    FpgaDevice(const FpgaDevice&)            = delete;
    FpgaDevice& operator=(const FpgaDevice&) = delete;

    // ---- IDevice ----------------------------------------------------

    DeviceType  type() const override { return DeviceType::FPGA; }
    std::string id()   const override { return id_; }

    std::unique_ptr<IDevicePlan> compilePlan(const DGraph& dg) override;

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

    // ---- BAR-backed scalar accessors (also used by scalar bridges) ----

    /// Scalar signal slots are 32-bit RP1 values. Wider FPGA output scalars are
    /// rejected during plan compilation until the protocol grows a multi-slot read.
    void setInputScalar(const std::string& scalarKey, std::uint64_t bits);
    std::uint64_t getOutputScalar(const std::string& scalarKey) const;

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

    struct StagedPdiRecord {
        std::unique_ptr<::vrt::Buffer<std::uint8_t>> buffer;
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
                                   std::size_t sizeBytes);
    std::uint64_t bufferDeviceAddress(const GraphBuffer& buffer, std::size_t sizeBytes);
    /// Walk a DGraph's kernel buffer bindings and record each bound buffer's
    /// m_axi memory region into @ref bufferRegion_ (keyed by scoped buffer
    /// name) so later allocation lands in the region the kernel can reach.
    void populateBufferRegions(const DGraph& dg);
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
    std::uint64_t stagePdiBytes(const std::string& cacheKey,
                                const std::vector<std::uint8_t>& bytes);
    std::uint64_t stagePdiFile(const std::string& pdiPath);
    void setActiveImage(std::string imageId);
    std::string activeImageId() const;

    std::string                            id_;
    std::shared_ptr<fpga::Rp1BarWindow>    window_;
    FpgaKernelLocationLookup               lookup_;
    std::shared_ptr<fpga::FpgaVbinSpec>     vbinSpec_;
    std::shared_ptr<fpga::Rp1Submitter>    submitter_;
    std::uint32_t                          sentinelSlot_  = kDefaultSentinelSlot;
    std::uint32_t                          sentinelValue_ = kDefaultSentinelValue;
    std::chrono::milliseconds              waitTimeout_   = kDefaultFpgaWaitTimeout;

    mutable std::mutex                     bufferMutex_;
    std::map<std::string, BufferRecord>    buffers_;
    std::map<std::string, std::string>      bufferAliases_;
    std::map<std::string, ::vrt::MemoryConfig> bufferRegion_;
    std::uint32_t                          nextBufferOffset_ = 0;
    mutable std::mutex                     scalarMutex_;
    fpga::SignalSlotAllocator              scalarSlotAlloc_;
    std::map<std::string, std::uint32_t>    scalarSlots_;
    mutable std::mutex                     pdiMutex_;
    std::shared_ptr<::vrt::Device>          pdiStagingDevice_;
    std::map<std::string, StagedPdiRecord>  stagedPdis_;
    mutable std::mutex                     imageMutex_;
    std::string                            activeImageId_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_FPGA_DEVICE_HPP
