# SLASH CMake Modules

Build-system support for HLS kernel compilation, FPGA vbin linking,
and AMD tool discovery.

| Module                     | Purpose                                              |
|----------------------------|------------------------------------------------------|
| `SlashTools.cmake`         | SLASH linker integration (`add_vbin`)                |
| `BuildHLS.cmake`           | HLS kernel compilation (`build_hls`, `build_hls_dir`, `build_hls_clean`) |
| `FindVivado.cmake`         | Locate AMD Vivado installation                       |
| `FindVitis.cmake`          | Locate AMD Vitis HLS installation                    |
| `CheckSlashInstall.cmake`  | Validate SLASH hardware files are installed          |

## Using the modules

**Installed mode** (after `cmake --install`):

```cmake
find_package(SlashTools REQUIRED)
```

Modules are installed to `<prefix>/lib/cmake/SlashTools/`.
`find_package(SlashTools)` automatically brings in `BuildHLS`,
`FindVivado`, and `FindVitis`.

**Source-tree mode** (development against local repo):

```cmake
list(APPEND CMAKE_MODULE_PATH "${SLASH_REPO_ROOT}/cmake")
include(SlashTools)
```

## Module reference

### SlashTools &mdash; `add_vbin()`

Links HLS kernel IP into a vbin file using the SLASH linker.

```cmake
add_vbin(
    TARGET   <target-name>
    PLATFORM <hw|sim|emu>
    CFG      <path/to/config.cfg>
    KERNELS  <kernel1.xml> [kernel2.xml ...]
)
```

| Parameter  | Required | Description                                |
|------------|----------|--------------------------------------------|
| `TARGET`   | yes      | Name of the CMake target to create         |
| `PLATFORM` | yes      | Target platform: `hw`, `sim`, or `emu`     |
| `CFG`      | yes      | Path to the linker configuration file      |
| `KERNELS`  | yes      | List of kernel component.xml files to link |

**Output:** `${CMAKE_CURRENT_BINARY_DIR}/<TARGET>.vbin`

**Linker detection** (two modes, tried in order):

1. **Installed** &mdash; finds `slashkit` on PATH
2. **Source tree** &mdash; uses `SLASH_REPO_ROOT` (or auto-detects from
   `../linker/src/main.py` relative to this directory).  Requires
   Python 3.

Variables set after loading:

| Variable              | Description                               |
|-----------------------|-------------------------------------------|
| `SLASH_FOUND`         | `TRUE` when SlashTools is ready           |
| `SLASHKIT_EXECUTABLE` | Path to `slashkit` (installed mode)       |
| `SLASH_REPO_ROOT`     | Path to SLASH repo root (source mode)     |
| `VIVADO_BINARY`       | Path to `vivado` (from FindVivado)        |
| `VITIS_ROOT_DIR`      | Path to Vitis root (from FindVitis)       |

### BuildHLS &mdash; `build_hls()`, `build_hls_dir()`, `build_hls_clean()`

Compiles HLS kernels using `v++` and `vitis-run`.  Both executables
must be on PATH.

#### `build_hls()` &mdash; single kernel

```cmake
build_hls(
    TARGET  <target-name>
    CPP     <source.cpp>
    CFG     <config.cfg>
    DEVICE  <part-name>
    [OUT_DIR <output-directory>]
)
```

| Parameter | Required | Description                                          |
|-----------|----------|------------------------------------------------------|
| `TARGET`  | yes      | CMake target name                                    |
| `CPP`     | yes      | HLS C++ source file                                  |
| `CFG`     | yes      | Vitis HLS configuration file                         |
| `DEVICE`  | yes      | FPGA part (e.g. `xcv80-lsva4737-2MHP-e-S`)          |
| `OUT_DIR` | no       | Output directory (default: `CMAKE_CURRENT_BINARY_DIR`) |

Sets target properties `HLS_BUILD_DIR` and `HLS_COMPONENT_XML`, and
parent-scope variables `${TARGET}_BUILD_DIR` and
`${TARGET}_COMPONENT_XML`.

#### `build_hls_dir()` &mdash; batch build

```cmake
build_hls_dir(
    TARGET   <target-name>
    ROOT     <source-directory>
    DEVICE   <part-name>
    KERNELS  <kernel1> [kernel2 ...]
    [OUT_DIR     <output-directory>]
    [OUT_IP_REPO <variable>]
    [OUT_KERNELS <variable>]
)
```

For each kernel name `K`, expects `ROOT/K.cpp` and `ROOT/K.cfg`.
Creates individual build targets and a parent target that depends on
all of them.

| Parameter    | Required | Description                                      |
|--------------|----------|--------------------------------------------------|
| `TARGET`     | yes      | Parent CMake target name                         |
| `ROOT`       | yes      | Directory containing `<kernel>.cpp` and `.cfg`   |
| `DEVICE`     | yes      | FPGA part name                                   |
| `KERNELS`    | yes      | List of kernel names                             |
| `OUT_DIR`    | no       | Output directory                                 |
| `OUT_IP_REPO`| no       | Variable to receive the IP repository path       |
| `OUT_KERNELS`| no       | Variable to receive the list of component.xml files |

#### `build_hls_clean()` &mdash; remove build artifacts

```cmake
build_hls_clean(
    TARGET  <target-name>
    DEVICE  <part-name>
    [ROOT   <directory>]
    [EXTRA_GLOBS <pattern> ...]
)
```

Creates a target that removes `build_*.DEVICE` directories, log files,
`.Xil`, and CMake cache artifacts.

### FindVivado

Locates the AMD Vivado installation.

```cmake
find_package(Vivado REQUIRED)
```

Search order:

1. `VIVADO_ROOT_DIR` CMake variable
2. `XILINX_VIVADO` environment variable
3. System `PATH`

| Variable          | Description                      |
|-------------------|----------------------------------|
| `VIVADO_FOUND`    | `TRUE` if Vivado was found       |
| `VIVADO_PATH`     | Directory containing `vivado`    |
| `VIVADO_ROOT_DIR` | Vivado installation root         |
| `VIVADO_BINARY`   | Full path to `vivado` executable |

### FindVitis

Locates the AMD Vitis HLS installation.

```cmake
find_package(Vitis REQUIRED)
```

Search order:

1. `VITIS_ROOT_DIR` CMake variable
2. `XILINX_VITIS` environment variable
3. `VITIS_HOME` environment variable
4. `VITIS` environment variable
5. System `PATH`

| Variable            | Description                                 |
|---------------------|---------------------------------------------|
| `VITIS_FOUND`       | `TRUE` if Vitis was found                   |
| `VITIS_BINARY`      | Full path to `vitis` executable             |
| `VITIS_ROOT_DIR`    | Vitis installation root                     |
| `VITIS_INCLUDE_DIR` | `${VITIS_ROOT_DIR}/include` (validated)     |

### CheckSlashInstall

Validates that SLASH hardware files are installed.

```cmake
include(CheckSlashInstall)
```

Checks for four required files in `INSTALL_DIR` (default
`/opt/amd/slash`):

- `static_shell_service_layer.dcp`
- `static_shell_slash.dcp`
- `amd_v80_gen5x8_25.1.pdi`
- `top_wrapper_routed_bb.dcp`

Override the install path:

```cmake
set(INSTALL_DIR "/custom/path")
include(CheckSlashInstall)
```

## Typical integration

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_v80_project)

option(SLASH_USE_REPO "Build against local SLASH repo" OFF)

if(SLASH_USE_REPO)
    set(SLASH_REPO_ROOT "/path/to/SLASH")
    list(APPEND CMAKE_MODULE_PATH "${SLASH_REPO_ROOT}/cmake")
    include(SlashTools)
else()
    find_package(vrt REQUIRED CONFIG)
    find_package(SlashTools REQUIRED)
endif()

set(DEVICE "xcv80-lsva4737-2MHP-e-S")

# Build HLS kernels
build_hls_dir(
    TARGET  hls
    ROOT    "${CMAKE_CURRENT_SOURCE_DIR}/hls"
    DEVICE  "${DEVICE}"
    KERNELS increment accumulate
    OUT_KERNELS _KERNEL_XMLS
)

# Link into a vbin
add_vbin(
    TARGET   my_design_hw
    PLATFORM hw
    CFG      "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg"
    KERNELS  ${_KERNEL_XMLS}
)
```

## File listing

```
cmake/
  SlashTools.cmake              Linker integration (add_vbin)
  BuildHLS.cmake                HLS kernel build functions
  FindVivado.cmake              Vivado tool finder
  FindVitis.cmake               Vitis HLS tool finder
  CheckSlashInstall.cmake       Installation validator
  SlashToolsConfig.cmake.in     Package config template
  CMakeLists.txt                Module installation
```

## License

MIT.  See the license header in each `.cmake` file for the full text.
