# vrt::graph::FpgaDevice

`FpgaDevice` is the `IDevice` backend that targets the RP1 command
processor on AMD Alveo V80. It takes a per-device `DGraph` produced
by `GraphCompiler` and translates the `CompiledKernelNode`s into the
`rp1_node_t` packet format defined in
`driver/libslash/include/slash/uapi/rp1_protocol.h`.

Source layout:

```
vrt/include/vrt/graph/device/
├── fpga_device.hpp              # FpgaDevice : public IDevice
└── fpga/
    ├── rp1_bar_window.hpp       # typed accessors over BAR4 + 64 MiB
    └── rp1_submitter.hpp        # graph_seq bump + graph_done_seq poll

vrt/src/graph/device/
├── fpga_device.cpp
└── fpga/
    ├── rp1_bar_window.cpp
    └── rp1_submitter.cpp
```

## Architecture

```
            ┌─────────────────────────────┐
   user ──▶ │ vrt::graph::Graph           │
            │  - addNode(FPGA, IOMap,...) │
            │  - compile() + run()        │
            └──────────────┬──────────────┘
                           │ DGraph for "fpga:0"
                           ▼
            ┌─────────────────────────────┐
            │ FpgaDevice::compilePlan     │
            │  - allocate barrier bits    │
            │  - pack scalar args         │
            │  - emit sentinel SIGNAL     │
            └──────────────┬──────────────┘
                           │ Rp1GraphImage
                           ▼
            ┌─────────────────────────────┐
            │ Rp1Submitter                │
            │  - ensureReady              │
            │  - submitAndWait            │
            │  - drainCq                  │
            └──────────────┬──────────────┘
                           │ rp1_node_t / rp1_ctrl_t writes
                           ▼
            ┌─────────────────────────────┐
            │ Rp1BarWindow                │
            │  - readAt / writeAt         │
            │  - readU32 / writeU32       │
            └──────────────┬──────────────┘
                           │ vrtd::BarFile::getPtr<T>(...)
                           ▼
            vrtd  →  libslash  →  BAR4 + 64 MiB
                           │
                           ▼
            RP1 firmware (rp1.elf with RP1_POLLING_BRINGUP=ON)
```

## Public surface

```cpp
#include <vrt/graph/device/fpga_device.hpp>
#include <vrt/graph/device/fpga/rp1_bar_window.hpp>
#include <vrt/graph/graph.hpp>

#include <vrtd/session.hpp>

// 1. Open the BAR window through vrtd.
vrtd::Session session;                                   // /run/vrtd.sock
vrtd::Device  dev   = session.getDeviceByBdf("0000:65:00.0");
vrtd::BarFile bar4  = dev.getBar(4).openBarFile();
auto window = std::make_shared<vrt::graph::fpga::Rp1BarWindow>(std::move(bar4));

// 2. Provide a name -> R5 AXI-Lite base address lookup.
auto lookup = [](const std::string& name) -> vrt::graph::FpgaKernelLocation {
    // r5_addr = xml_addr - 0x0202'0000'0000 + 0x8800'0000
    if (name == "myKernel") return {0x88010000u, /*timeout_cycles*/ 0};
    throw std::runtime_error("unknown kernel '" + name + "'");
};

// 3. Build, compile, run.
auto fpga = std::make_shared<vrt::graph::FpgaDevice>("fpga:0", window, lookup);

vrt::graph::Graph g = vrt::graph::Graph::withDefaults();
g.registerDevice(fpga);

vrt::graph::IOMap io;
io.bindScalar("size", vrt::graph::GraphScalar::constant<std::uint32_t>(0));
g.addNode(vrt::graph::KernelDescriptor{"myKernel", vrt::graph::DeviceType::FPGA},
          std::move(io), "fpga:0");

g.compile();
g.run();  // blocks until graph_done_seq catches up
```

## `FpgaKernelLocationLookup`

A `std::function<FpgaKernelLocation(const std::string&)>` that maps a
`KernelDescriptor::name` to its AXI-Lite base address in R5 address
space. Returning `r5_base_addr == 0` causes `compilePlan()` to throw
(so unmapped names surface as configuration errors rather than
silently submitting bogus dispatches).

The lookup is called once per kernel at `compilePlan()` time, not at
launch. If the user mutates their map between compile and launch
those changes are not picked up — re-compile to apply them.

## What compilePlan emits

For each `CompiledKernelNode k` in DGraph order:

1. **Barrier bit** — one bit in bucket 0, allocated in node order.
   Up to 31 kernels per graph (bit 31 reserved for the sentinel).
2. **Args** — each `IOTypeMap::inputScalars` port resolved against
   `IOMap`, packed into a contiguous argument buffer at the next
   available word offset. Constants are baked in at compile time;
   `GraphScalar::globalVar(...)` bindings are recorded as deferred
   resolutions that the plan re-runs at every `launch()`. Widths
   follow HLS convention: 8/16/32-bit scalars occupy one word
   (sign- or zero-extended); 64-bit scalars occupy two words
   little-endian.
3. **`KERNEL_DISPATCH` packet** — `kernel_base_addr` from the lookup,
   `arg_buffer_offset` + `arg_count` from the packing pass, `await_mask`
   as the OR of every predecessor's allocated bit, `set_mask` set to
   this kernel's own bit.

A trailing `RP1_OP_SIGNAL` "sentinel" node awaits the OR of every
leaf kernel's set-bit and writes `kDefaultSentinelValue`
(`0xD1A1D0DD`) into `kDefaultSentinelSlot` (slot 255). Both are
configurable via `FpgaDevice::setSentinelSlot()` /
`setSentinelValue()`. Hosts that just want to confirm graph
completion can read the sentinel slot after `Graph::wait()` returns.

## Phase 1 limitations

`FpgaDevice::compilePlan()` throws `std::logic_error` with a
descriptive message if it encounters any of:

- `CompiledBridgeOpNode` — cross-device buffer transfers are not yet
  emitted by FpgaDevice (the existing `CpuFpgaBridge` data path is
  also still stubbed). Pre-stage FPGA buffers outside the graph.
- `CompiledBoundaryNode` — graph-region boundaries.
- `CompiledLoopNode` / `CompiledConditionalNode` — RP1 supports
  `LOOP` / `COND` / `RERUN` natively; the lowering lands in a later
  phase.
- More than 31 kernels in a single graph (one bucket).
- Output scalar ports (`IOTypeMap::outputScalars`).

These will be lifted incrementally as the surrounding pieces land
(`DMA_COPY` emission, RP1 LOOP/COND lowering, multi-bucket NOP bridges).

## Diagnostics

`Rp1Submitter` surfaces firmware-side errors directly:

- `Rp1TimeoutError` (a `std::runtime_error` subclass) if `magic`,
  `rp1_state == READY`, or `graph_done_seq` don't reach their
  expected values in time. The exception message includes
  `rp1_state`, `rp1_error_code`, `rp1_current_node`, and
  `cq_write_idx` so the failure mode is unambiguous.
- `std::runtime_error` if the firmware lands in `RP1_STATE_ERROR` or
  `RP1_STATE_HALTED` after a submission.

Per-node completions can be read with
`FpgaDevicePlan::lastCq()` (an internal accessor reachable via a
`dynamic_cast`) or, more commonly, by going through
`FpgaDevice::submitter()->drainCq()` directly. Phase-1 hosts that
only care about whole-graph completion can just check the sentinel
slot after `Graph::wait()` returns.

## Where to go from here

- Run-tested example: `examples/rp1_bringup_vrt/` (canonical
  four-kernel diamond, verifies sentinel + CQ count).
- Wire contract: `driver/libslash/include/slash/uapi/rp1_protocol.h`.
- Firmware: `linker/resources/aved/rp1/`. Build with
  `-DRP1_POLLING_BRINGUP=ON` until the GCQ doorbell is wired.
- Full design: `linker/resources/aved/rp1/ARCHITECTURE.md` (§H lists
  every layered component and which phase introduces each).
