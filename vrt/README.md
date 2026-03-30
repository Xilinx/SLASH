# VRT (V80 RunTime)

C++17 runtime library for executing FPGA kernels on the AMD Alveo V80.
VRT provides a unified API for hardware, emulation, and simulation
platforms, handling device management, memory allocation, kernel
dispatch, and streaming DMA.

| Class              | Header                    | Purpose                                  |
|--------------------|---------------------------|------------------------------------------|
| `Device`           | `vrt/device.hpp`          | Open a V80 board and load a vrtbin       |
| `Kernel`           | `vrt/kernel.hpp`          | Set arguments, launch, and wait          |
| `Buffer<T>`        | `vrt/buffer.hpp`          | Typed device memory with host sync       |
| `StreamingBuffer<T>` | `vrt/streaming_buffer.hpp` | QDMA streaming I/O for kernel ports    |
| `Vrtbin`           | `vrt/vrtbin.hpp`          | Archive extraction and metadata lookup   |

## Building

```sh
cmake -B build -S . -G Ninja
cmake --build build
```

CMake options:

| Option               | Default | Description                                      |
|----------------------|---------|--------------------------------------------------|
| `VRT_INCLUDE_VRTD`   | `OFF`   | Build the bundled vrtd daemon instead of using the system package |
| `ENABLE_SANITIZERS`  | `OFF`   | Build with AddressSanitizer and UBSan            |

## Installing

```sh
sudo cmake --install build --prefix /usr/local
```

This installs:

- Headers to `<prefix>/include/vrt/`
- Library to `<prefix>/lib/libvrt.so`
- CMake package config to `<prefix>/lib/cmake/vrt/`

Downstream projects can then use:

```cmake
find_package(vrt REQUIRED CONFIG)
target_link_libraries(myapp PRIVATE vrt::vrt)
```

## Dependencies

| Library    | Package (apt)        | Purpose                              |
|------------|----------------------|--------------------------------------|
| libxml2    | `libxml2-dev`        | system_map.xml parsing               |
| ZeroMQ     | `libzmq3-dev`        | Emulation and simulation IPC         |
| JsonCpp    | `libjsoncpp-dev`     | Emulation manifest and JSON commands |
| ZLIB       | `zlib1g-dev`         | vrtbin archive decompression         |
| vrtd       | (bundled or system)  | Low-level device access daemon       |

```sh
sudo apt install libxml2-dev libzmq3-dev libjsoncpp-dev zlib1g-dev
```

## API overview

### Open a device and run kernels

```cpp
#include <vrt/device.hpp>
#include <vrt/kernel.hpp>
#include <vrt/buffer.hpp>

vrt::Device device(bdf, vrtbinPath);

vrt::Kernel kernel(device, "my_kernel_0");

/* Allocate a buffer on the memory bank the kernel argument is connected to */
vrt::Buffer<float> buf(device, 1024, kernel.argMemoryConfig("in"));

/* Fill host side, then sync to device */
for (uint32_t i = 0; i < 1024; i++)
    buf[i] = static_cast<float>(i);
buf.sync(vrt::SyncType::HOST_TO_DEVICE);

/* Launch the kernel and wait for completion */
kernel.setArg(0, 1024);   /* scalar argument */
kernel.setArg(1, buf);    /* buffer argument (auto-resolves to physical address) */
kernel.start();
kernel.wait();

/* Read a result register */
uint32_t result = kernel.read(0x18);

device.cleanup();
```

### Buffer memory types

```cpp
/* DDR */
vrt::Buffer<int> ddr(device, size, vrt::MemoryRangeType::DDR);

/* HBM via virtual network-on-chip (auto-placed) */
vrt::Buffer<int> hbm(device, size, vrt::MemoryRangeType::HBM_VNOC);

/* HBM on a specific port (kernel metadata) */
vrt::Buffer<int> hbm(device, size, kernel.argMemoryConfig("in"));
```

### Streaming buffers (QDMA)

```cpp
#include <vrt/streaming_buffer.hpp>

vrt::StreamingBuffer<uint32_t> sbuf(device, kernel, "s_axis_data", size);
for (uint32_t i = 0; i < size; i++)
    sbuf[i] = i;
sbuf.sync();   /* direction is inferred from port configuration */
```

### Kernel launch styles

#### Argument style

Arguments can be passed inline or staged with `setArg` before the launch call:

```cpp
/* Inline: pass arguments directly */
kernel.call(size, buf);
kernel.start(size, buf);

/* Staged: set arguments by index or name, then launch */
kernel.setArg(0, size);
kernel.setArg("buf", buf);
kernel.call();   /* or kernel.start() */
```

#### Call vs. start (blocking vs. non-blocking)

`call` launches and waits; `start` launches and returns immediately so other work can proceed while the kernel runs:

```cpp
/* Blocking */
kernel.call(size, buf);

/* Non-blocking */
kernel.start(size, buf);
/* ... do other work ... */
kernel.wait();
```

## Platform support

VRT transparently supports three execution platforms, selected by the
vrtbin contents:

| Platform     | Enum                      | Description                              |
|--------------|---------------------------|------------------------------------------|
| Hardware     | `vrt::Platform::HARDWARE`   | Real FPGA via PCIe BAR and QDMA        |
| Emulation    | `vrt::Platform::EMULATION`  | C-model software emulation via ZeroMQ  |
| Simulation   | `vrt::Platform::SIMULATION` | Verilog simulation via register map    |

Kernel and buffer code paths adapt automatically.  Check the current
platform with `device.getPlatform()`.

## API documentation

Doxygen HTML and PDF documentation can be generated from the
[doc/](doc/) directory.  See [doc/README.md](doc/README.md) for
instructions.

## Project layout

```
vrt/
  include/vrt/
    device.hpp                Public API - device management
    kernel.hpp                Public API - kernel execution
    buffer.hpp                Public API - typed device memory
    streaming_buffer.hpp      Public API - QDMA streaming buffers
    vrtbin.hpp                Public API - archive handling
    allocator/
      allocator.hpp           Memory allocator (buddy system)
    driver/
      qdma_logic.hpp          QDMA driver logic
    parser/
      xml_parser.hpp          system_map.xml parser
      utilization_parser.hpp  Resource utilization parser
      utilization_data.hpp    Utilization report data structures
    qdma/
      pcie_driver_handler.hpp PCIe driver handler
      qdma_connection.hpp     QDMA streaming connection metadata
      qdma_intf.hpp           QDMA interface abstraction
    register/
      register.hpp            Hardware register abstraction
    utils/
      filesystem_cache.hpp    Filesystem cache utility
      logger.hpp              Logging facility
      platform.hpp            Platform enum
      zmq_server.hpp          ZeroMQ IPC server
  src/
    device.cpp                Device implementation
    kernel.cpp                Kernel implementation
    vrtbin.cpp                Vrtbin archive extraction
    allocator/                Memory allocator implementation
    driver/                   Driver interface implementation
    parser/                   XML/utilization parser implementation
    qdma/                     QDMA subsystem implementation
    register/                 Register access implementation
    utils/                    Utility implementations
  doc/
    Doxyfile                  Doxygen configuration
    Makefile                  Documentation build
  vrtd/                       V80 runtime daemon (see vrtd/README.md)
```

## License

MIT — see [LICENSE](../LICENSE).
