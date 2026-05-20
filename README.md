# SLASH — Platform for AMD Alveo V80

SLASH is an open-source platform for AMD Alveo V80 FPGA boards. It provides a
complete runtime and development ecosystem for executing FPGA kernels, managing
devices, and transferring data between host and device memory.

Key components:

- **VRT** (V80 RunTime) — C++17 API for kernel execution, buffer management, and device control
- **v80-smi** — command-line tool for board management, programming, and diagnostics
- **slashkit** — Python-based linker that packages HLS kernels into deployable *vrtbin* archives
- **slash** — Linux kernel module and driver stack

## Architecture

SLASH is organized as a layered stack. Each layer has a single responsibility
and communicates with adjacent layers through well-defined interfaces.

```
┌─────────────────────────────────────────────┐
│              User Application               │  C++17
├─────────────────────────────────────────────┤
│            VRT  (libvrt)                    │  C++17  ─ MIT
├─────────────────────────────────────────────┤
│          libvrtd++  (C++ RAII wrapper)      │  C++20  ─ MIT
├─────────────────────────────────────────────┤
│          libvrtd    (C wire-protocol)       │  C11    ─ MIT
├──────────────── AF_UNIX ────────────────────┤
│          vrtd       (daemon)                │  C11    ─ MIT
├─────────────────────────────────────────────┤
│          libslash   (driver wrapper)        │  C      ─ MIT
├─────────────────────────────────────────────┤
│       Linux kernel module  (slash)          │  C      ─ GPLv2
├─────────────────────────────────────────────┤
│          AMD Alveo V80 Hardware             │
└─────────────────────────────────────────────┘
```

Two additional components sit alongside the stack:

- **v80-smi** — CLI for listing, programming, resetting, and validating V80 boards.
- **slashkit** — links HLS kernels into *vrtbin* archives for deployment.

## Repository Layout

| Directory | Component | Description |
|-----------|-----------|-------------|
| [`vrt/`](vrt/) | VRT | C++17 runtime library — [README](vrt/README.md) |
| [`driver/`](driver/) | Kernel module + libslash | Linux driver and C wrapper — [README](driver/libslash/README.md) |
| [`smi/`](smi/) | v80-smi | CLI management tool — [README](smi/README.md) |
| [`linker/`](linker/) | slashkit | Python-based kernel linker |
| [`cmake/`](cmake/) | CMake modules | Build system integration — [README](cmake/README.md) |
| [`examples/`](examples/) | Examples | Demo projects — [README](examples/README.md) |
| [`docs/`](docs/) | Documentation | Sphinx / ReadTheDocs site |
| [`packaging/`](packaging/) | Packages | Debian and RPM packaging |
| [`scripts/`](scripts/) | Scripts | Build, package, and test helpers |

## Platform Modes

VRT supports three execution platforms. The same application source code runs
on all three — the platform is determined by the vrtbin file, not by the
application.

| Platform | Transport | Build Target | Use Case |
|----------|-----------|-------------|----------|
| **Hardware** | PCIe BAR + QDMA | `hw` | Production runs on a physical V80 board |
| **Emulation** | ZeroMQ IPC to C-model | `emu` | Functional verification without FPGA hardware |
| **Simulation** | Verilog register map | `sim` | Cycle-accurate RTL simulation |

Each example provides three vrtbin targets via CMake:

```cmake
add_vbin(TARGET "axilite_hw"  PLATFORM "hw"  CFG "${CFG_FILE}" KERNELS ${_KERNELS})
add_vbin(TARGET "axilite_emu" PLATFORM "emu" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
add_vbin(TARGET "axilite_sim" PLATFORM "sim" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
```

## Prerequisites

**System requirements:**

- Ubuntu LTS 22.04+; RHEL 9+ or compatible (other distributions may work as well but have not been tested)
- AMD Vivado & Vitis HLS 2025.1 — source the environment before building or
  running against emulation/simulation:

  ```bash
  source <path-to-vivado>/settings64.sh
  source <path-to-vitis-hls>/settings64.sh
  ```

  For `csh`/`tcsh` shells, use `settings64.csh` instead. Using versions other
  than 2025.1 may cause breakage.

**Library dependencies:**

```bash
sudo apt install cmake pkg-config ninja-build \
  libxml2-dev libzmq3-dev libjsoncpp-dev zlib1g-dev \
  libsystemd-dev libinih-dev libcli11-dev \
  linux-headers-$(uname -r)
```

**Submodules:**

SLASH depends on [AVED](https://github.com/Xilinx/AVED) and [QDMA](https://github.com/Xilinx/dma_ip_drivers):

```bash
git submodule update --init --recursive
```

## Quick Start

### 1. Build the stack

Components must be built in dependency order:

```bash
# Kernel module
cd driver && make && sudo insmod slash.ko && cd ..

# libslash (kernel module client library)
cd driver/libslash && cmake -S . -B build -G Ninja && cmake --build build && sudo cmake --install build && cd ../..

# vrtd (daemon + client libraries)
cd vrt/vrtd && cmake -S . -B build -G Ninja && cmake --build build && sudo cmake --install build && cd ../..

# VRT (runtime library)
cd vrt && cmake -S . -B build -G Ninja && cmake --build build && sudo cmake --install build && cd ..

# v80-smi (CLI tool)
cd smi && cmake -S . -B build -G Ninja && cmake --build build && sudo cmake --install build && cd ..
```

### 2. Start the daemon

```bash
sudo vrtd                              # manual
sudo systemctl enable --now vrtd       # production (systemd)
```

### 3. Verify

```bash
v80-smi list
```

All four readiness checks (PF0, PF1, PF2, VRTD) should pass for each board.

### 4. Build and run an example

```bash
cd examples/00_axilite
cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON
cmake --build build

# Build FPGA artefacts (requires Vivado/Vitis)
cmake --build build --target hls              # compile HLS kernels
cmake --build build --target axilite_hw       # link into a hardware vrtbin

# Run
./build/00_axilite <BDF> build/axilite_hw.vbin
```

Set these environment variables before running:

```bash
source <path-to-vivado>/settings64.sh
source <path-to-vitis>/settings64.sh
```

## Code Example

A minimal VRT application:

```cpp
#include <vrt/device.hpp>
#include <vrt/kernel.hpp>
#include <vrt/buffer.hpp>

int main() {
    // Open device and program FPGA
    vrt::Device device("03:00", "design.vrtbin");

    // Get kernel handle
    vrt::Kernel increment(device, "increment_0");

    // Allocate device buffer using the kernel's port configuration
    vrt::Buffer<float> buffer(device, 1024, increment.argMemoryConfig("in"));

    // Fill host-side data
    for (size_t i = 0; i < 1024; ++i)
        buffer[i] = static_cast<float>(i);

    // Transfer host → device
    buffer.sync(vrt::SyncType::HOST_TO_DEVICE);

    // Launch kernel
    increment.setArg(0, 1024);
    increment.setArg(1, buffer);
    increment.start();
    increment.wait();

    // Transfer device → host
    buffer.sync(vrt::SyncType::DEVICE_TO_HOST);

    // Read result register
    uint32_t result = increment.read(0x18);

    device.cleanup();
    return 0;
}
```

## v80-smi Commands

| Command | Description |
|---------|-------------|
| `v80-smi version` | Print build version |
| `v80-smi list` | Enumerate V80 boards with readiness checks (`-l` long, `-s` sensors, `-j` JSON) |
| `v80-smi inspect <vrtbin>` | Display vrtbin metadata (platform, clock, kernels, memory map) |
| `v80-smi query -d <BDF>` | Display metadata of the currently loaded design on a device |
| `v80-smi program <vrtbin> -d <BDF>` | Program a V80 device with a vrtbin file |
| `v80-smi reset -d <BDF>` | Hardware-reset a board (PCIe secondary bus reset) |
| `v80-smi validate -d <BDF>` | Run memory integrity and bandwidth tests (HBM and DDR) |

See the full [v80-smi reference](smi/README.md) for details and examples.

## Memory Model

The V80 board has two memory subsystems:

| Memory | Selection | Capacity | Notes |
|--------|-----------|----------|-------|
| **DDR** | `MemoryRangeType::DDR` | Large, single address space | Bulk storage; referenced as `DDR0` in linker config |
| **HBM** (port) | `MemoryRangeType::HBM` + port | 64 pseudo-channels (HBM0–HBM63) | Explicit channel; high aggregate bandwidth |
| **HBM** (VNOC) | `MemoryRangeType::HBM_VNOC` | Auto-distributed across channels | No manual channel management |

The recommended approach is to derive memory configuration from the kernel metadata
rather than hardcoding types:

```cpp
vrt::Buffer<float> buf(device, size, kernel.argMemoryConfig("in"));
```

This ensures the buffer allocation always matches the linker configuration.

## Examples

| ID | Feature | Notes |
|----|---------|-------|
| 0 | Linking, AXI-Lite control | |
| 1 | Kernels with AXI-MM interfaces | |
| 2 | Freerunning streaming kernels | |
| 3 | Controlling multiple V80s | Uses vrtbin from example 00 |
| 4 | Frequency targets | |
| 5 | Memory performance test | Instantiates maximum number of kernels |
| 6 | Network interface test | Drives two network interfaces |

See the [examples README](examples/README.md) for build and run instructions.

## Component Documentation

Each component has its own README with detailed information:

- **[VRT Runtime](vrt/README.md)** — API overview, classes, building, and platform support
- **[libslash](driver/libslash/README.md)** — driver wrapper, device node API, mock mode
- **[v80-smi](smi/README.md)** — all commands with usage examples
- **[CMake Modules](cmake/README.md)** — BuildHLS, FindVivado, FindVitis, SlashTools reference
- **[VRT API Docs](vrt/doc/README.md)** — Doxygen generation instructions
- **[vrtd Daemon](vrt/vrtd/README.md)** — daemon coding guidelines and standards
- **[Examples](examples/README.md)** — build recipes and run instructions for all examples

## Full Documentation

The complete documentation is published at **[slash-fpga.readthedocs.io](https://slash-fpga.readthedocs.io/)** and covers:

- **Tutorials** — getting started, writing kernels, buffers and memory, emulation/simulation, platform setup, device management, vrtd configuration
- **How-To Guides** — multiple boards, clock frequency, streaming chains, memory benchmarking, building from source, CMake modules, vrtbin inspection, mock mode
- **API Reference** — VRT, libslash, libvrtd, libvrtdpp, vrtd, v80-smi, CMake modules
- **Architecture** — stack overview, memory model, PCIe topology, platform modes, vrtbin format

## Known Limitations

- HLS arguments should not be Verilog or VHDL keywords (e.g. `in`, `out`). Some issues may appear in the linker with this configuration.
- In emulation, HLS kernels must include at least one AXI4-Lite interface to work.
- A maximum of 15 kernels can be instantiated in the current version of the linker. This will be fixed in future versions.
- Freerunning streaming kernel chains are not supported in emulation.

## Contributing

We welcome contributions. Please see [CONTRIBUTING.md](CONTRIBUTING.md) for:

- Issue reporting guidelines
- Pull request process (target the `dev` branch)
- Developer Certificate of Origin (DCO) requirements

## License

| Component | License |
|-----------|---------|
| Linux kernel driver | GPLv2 |
| All user-space code | MIT |

See [LICENSE](LICENSE) for the full text.
