# rp1_mem_latency_gpu

GPU-side latency benchmark for raw reads and writes to the RP1 shared DDR
window through PF2 BAR4.

This example is intentionally lower level than
[`../rp1_bringup_gpu`](../rp1_bringup_gpu/). It does not submit an RP1 graph.
Instead, it imports BAR4 as a dma-buf into HSA, launches one HIP thread, and
uses a scratch range inside the RP1 BAR-visible DDR aperture:

```text
BAR4 + 64 MiB + 0x00200000
```

The measured loop runs entirely on the GPU. The host CPU opens the control
device, imports the BAR dma-buf, launches the HIP kernel, then copies back the
result buffers after the kernel completes.

## What It Measures

The kernel records latency distributions in GPU `clock64()` cycles for:

- `write+fence`: a volatile 32-bit BAR store followed by
  `__threadfence_system()`.
- `read`: a `__threadfence_system()` followed by a volatile 32-bit BAR load.
- `write+read`: a volatile store, system fence, and immediate volatile readback
  of the same scratch word. This is a sanity check for BAR visibility, not an
  FPGA-side acknowledgement timestamp.

The access pattern walks cache-line-spaced words across the scratch span using a
multiplicative permutation. The BAR pointer is declared `volatile`, and protocol
boundaries use `__threadfence_system()` so the compiler and GPU cannot silently
reuse a previous value in the measured code.

The host prints both the raw cycle counts and a derived real-time value. By
default it converts cycles using HIP's reported device clock rate; pass
`--clock-mhz N` if you want to use a known `clock64()` rate instead.

## What It Proves

This test gives evidence that GPU-originated PCIe peer accesses reach the PF2
BAR4 mapping directly:

- The host never opens a CPU mmap of BAR4.
- While the dma-buf import is live, the SLASH driver rejects concurrent CPU BAR4
  mmap users.
- The only data copied through host memory is the final device result buffer
  after the benchmark is complete.

It does not provide a synchronized FPGA or RP1 firmware timestamp. The RP1 CQ
ABI has a timestamp field, but current firmware writes zero there, so this
example times the GPU's view of the BAR path.

## Scratch Range

The default scratch offset is `0x00200000` inside the 64 MiB RP1 shared DDR
aperture. That leaves the default RP1 protocol layout untouched:

- control block: `0x00000000`
- node array: `0x00001000`
- completion queue: `0x00041000`
- argument buffer: `0x00051000`
- signal array: `0x00151000`

Do not run this concurrently with RP1 graph examples that may use the same
scratch range.

## Build

```bash
cd examples/rp1_mem_latency_gpu
cmake -B build -S . \
  -DCMAKE_HIP_COMPILER=/opt/rocm-slash/bin/amdclang++ \
  -DGPU_ARCH=gfx908
cmake --build build
```

Adjust `GPU_ARCH` to match the installed GPU. The default in CMake is `gfx90a`;
the command above matches the MI100 configuration used for
`rp1_bringup_gpu`.

## Run

```bash
sudo LD_LIBRARY_PATH=/opt/rocm-slash/lib ./build/rp1_mem_latency_gpu /dev/slash_ctl0
```

Useful variants:

```bash
# Read-only sampling.
sudo LD_LIBRARY_PATH=/opt/rocm-slash/lib ./build/rp1_mem_latency_gpu \
  /dev/slash_ctl0 --mode read

# More samples across a larger scratch window.
sudo LD_LIBRARY_PATH=/opt/rocm-slash/lib ./build/rp1_mem_latency_gpu \
  /dev/slash_ctl0 --iterations 100000 --warmup 5000 --scratch-bytes 0x400000
```

Options:

```text
--iterations N       measured iterations (default: 10000)
--warmup N           unreported warmup iterations (default: 1000)
--scratch-offset N   offset inside RP1 shared DDR aperture (default: 0x200000)
--scratch-bytes N    scratch span in bytes (default: 0x100000)
--stride N           byte stride between sampled words (default: 64)
--mode read|write|rw measured operations (default: rw)
--clock-mhz N        clock64() conversion rate override in MHz
```

Expected output shape:

```text
SLASH device: 0000:c1:00.2  vendor=0x10ee  device=0x50c2
BAR4  addr=0x...  len=0x...  in_use=0
BAR dma-buf fd=3  len=536870912
GPU agent: gfx908
BAR4 mapped into GPU VA: 0x...  mapped_size=536870912
scratch BAR offset=0x4200000  aperture offset=0x200000  bytes=0x100000
iterations=10000  warmup=1000  stride=64  mode=rw
clock rate=1500.0 MHz (HIP device attribute)
Launching GPU kernel: rp1_mem_latency_gpu_kernel<<<1, 1>>>
status          = 1 (PASS)
magic_seen      = 0x53515231 (SQR1)
sample_count    = 10000
checksum        = 0x...

Latency distributions (GPU clock cycles and derived time):
write+fence       min=... p50=... p90=... p99=... max=... mean=... cycles
  time            min=... p50=... p90=... p99=... max=... mean=...
read              min=... p50=... p90=... p99=... max=... mean=... cycles
  time            min=... p50=... p90=... p99=... max=... mean=...
write+read        min=... p50=... p90=... p99=... max=... mean=... cycles
  time            min=... p50=... p90=... p99=... max=... mean=...
PASSED
```

`magic_seen` is diagnostic only. The benchmark writes and reads scratch memory
directly and does not require an RP1 graph submission, but `SQR1` is a useful
confirmation that the standard RP1 firmware control block is visible at the
expected BAR4 offset.

## Caveats

- The time values are derived from `clock64()` cycle deltas. They are only as
  accurate as the clock rate used for conversion. Use `--clock-mhz` if HIP's
  reported device clock is not the rate you want to assume.
- `write+fence` includes the cost of `__threadfence_system()`. That fence is
  deliberate; it is the operation that forces posted GPU writes toward system
  visibility.
- `write+read` confirms that a later GPU load observes the written value through
  the mapped BAR path. It is not proof that RP1 firmware consumed the value.
- A CPU shadow copy would defeat the purpose of this example. The dma-buf import
  path maps the BAR pages into the GPU; do not add host-side BAR mmap access to
  the benchmark loop.
