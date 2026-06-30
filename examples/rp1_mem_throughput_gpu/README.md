# rp1_mem_throughput_gpu

SDMA / `hipMemcpyAsync` throughput benchmark for bulk transfers between GPU
VRAM and the RP1 shared DDR window exposed through PF2 BAR4.

This example intentionally tests a different path from
[`../rp1_mem_latency_gpu`](../rp1_mem_latency_gpu/). The latency benchmark proves
that HIP kernels can access the imported BAR mapping with shader loads/stores.
This benchmark asks whether ROCm's copy engine path accepts the same foreign
dma-buf BAR mapping as a `hipMemcpyDeviceToDevice` source or destination.

## What It Measures

The host imports BAR4 with the same path used by the latency example:

```text
SLASH_CTLDEV_IOCTL_GET_BAR_FD -> hsa_amd_interop_map_buffer
```

It then allocates a GPU VRAM staging buffer and times:

- `write`: `GPU VRAM -> BAR4 scratch`
- `read`: `BAR4 scratch -> GPU VRAM`

The timed copy uses:

```cpp
hipMemcpyAsync(dst, src, bytes, hipMemcpyDeviceToDevice, stream)
```

Timing uses HIP events around each copy. Throughput is reported as GiB/s from
the transfer size and elapsed milliseconds.

## Important Caveat

This is an SDMA capability test. It may fail even when `rp1_mem_latency_gpu`
passes, because shader loads/stores and ROCm copy engines do not necessarily
accept every external mapping in the same way.

If `hipMemcpyAsync` fails, that means ROCm SDMA rejected the imported BAR mapping
for this operation. It does not invalidate the shader P2P results from the
latency benchmark.

## Scratch Range

The default scratch offset is `0x00200000` inside the 64 MiB RP1 shared DDR
aperture. The host BAR address is:

```text
BAR4 + 64 MiB + 0x00200000
```

The default requested transfer size is `64 MiB`, but the tool clamps the
effective transfer size to the available scratch span when the BAR/shared
aperture is smaller. The effective byte count is printed before the benchmark
runs.

Do not run this concurrently with RP1 graph examples that use the same scratch
range.

## Build

```bash
cd examples/rp1_mem_throughput_gpu
cmake -B build -S . \
  -DCMAKE_HIP_COMPILER=/opt/rocm-slash/bin/amdclang++ \
  -DGPU_ARCH=gfx908
cmake --build build
```

Adjust `GPU_ARCH` for your GPU.

## Run

```bash
sudo LD_LIBRARY_PATH=/opt/rocm-slash/lib \
  ./build/rp1_mem_throughput_gpu /dev/slash_ctl0 --bytes 67108864 --iters 10
```

Useful variants:

```bash
# Write-only SDMA test.
sudo LD_LIBRARY_PATH=/opt/rocm-slash/lib \
  ./build/rp1_mem_throughput_gpu /dev/slash_ctl0 --direction write

# Read-only SDMA test.
sudo LD_LIBRARY_PATH=/opt/rocm-slash/lib \
  ./build/rp1_mem_throughput_gpu /dev/slash_ctl0 --direction read

# Raw copy throughput without prefix/suffix verification.
sudo LD_LIBRARY_PATH=/opt/rocm-slash/lib \
  ./build/rp1_mem_throughput_gpu /dev/slash_ctl0 --no-verify
```

Options:

```text
--bytes N              transfer size in bytes (default: 67108864, bounded)
--iters N              timed repetitions (default: 10)
--warmup N             untimed repetitions (default: 2)
--direction read|write|both
                       transfer direction (default: both)
--scratch-offset N     offset inside RP1 shared DDR aperture (default: 0x200000)
--verify               enable prefix/suffix verification (default)
--no-verify            disable verification
```

Expected output shape:

```text
SLASH device: 0000:21:00.2  vendor=0x10ee  device=0x50c2
BAR4  addr=0x...  len=0x...  in_use=0
BAR dma-buf fd=4  len=134217728
GPU agent: gfx908
BAR4 mapped into GPU VA: 0x...  mapped_size=134217728
scratch BAR offset=0x4200000  aperture offset=0x200000  bytes=0x...
mode=sdma direction=both bytes=... iters=10 warmup=2 verify=on
write    median=... ms mean=... ms min=... ms max=... ms throughput_median=... GiB/s throughput_mean=... GiB/s
read     median=... ms mean=... ms min=... ms max=... ms throughput_median=... GiB/s throughput_mean=... GiB/s
PASSED
```

## Prerequisites

Use the same hardware/software stack as `rp1_bringup_gpu` and
`rp1_mem_latency_gpu`:

- PF2 BAR4 must be prefetchable and registered with Linux P2PDMA.
- Kernel must support foreign dma-buf import through amdgpu/KFD.
- ROCm must include the local foreign-dma-buf fallback patch.
- No CPU BAR4 mmap user can be active while the GPU mapping is live.

## Interpreting Results

This benchmark measures bulk copy-engine behavior over the imported BAR mapping.
It is not a replacement for `rp1_mem_latency_gpu`; the latency benchmark remains
the cleaner way to reason about individual non-posted BAR reads.

For SDMA throughput:

- `write` is usually expected to be faster than `read` because PCIe writes are
  posted.
- `read` depends on completions from the FPGA BAR path and may be much lower.
- Very high numbers should be treated skeptically until verification is enabled
  and passing.
- A failure from `hipMemcpyAsync` is a useful result: shader P2P works, but SDMA
  does not support this imported mapping on that ROCm/kernel stack.
