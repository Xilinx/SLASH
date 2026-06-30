# rp1_bringup_gpu

GPU-driven port of the `signal` subcommand of [`examples/rp1_bringup`](../rp1_bringup/).

A HIP kernel running on the GPU performs the entire RP1 firmware-liveness
exchange over PCIe peer-to-peer DMA: it programs the RP1 control block,
builds a one-node `SIGNAL` graph, bumps `graph_seq`, polls `graph_done_seq`,
and reads back signal slot 0. The host CPU writes nothing to BAR4 in the
hot path — every byte that arrives at RP1 came from the GPU.

This validates, end-to-end:

- PF2 BAR4 is registered with `pci_p2pdma_add_resource()` (i.e. the
  bitstream marks it prefetchable).
- The GPU's IOMMU mapping of BAR4 reaches the FPGA.
- RP1 firmware sees writes that originated on the GPU.
- RP1's responses (`graph_done_seq`, signal slot) are visible back to
  the GPU.

Pass criterion: signal slot 0 reads back `0xDEADBEEF` (same magic the
host-side `rp1_bringup signal` uses).

## How it relates to the rest of the tree

| Example | Driver | What it exercises |
|---|---|---|
| [`examples/07_rp1_memcheck`](../07_rp1_memcheck/) | host CPU + libslash mmap of BAR4 | RP1 firmware writes a 1 MB pattern; host reads it. No graph submission. |
| [`examples/rp1_bringup`](../rp1_bringup/) (`signal`) | host CPU + libslash mmap of BAR4 | Host submits a one-node SIGNAL graph, polls completion. |
| [`examples/08_p2p_gpu`](../08_p2p_gpu/) | GPU + dma-buf import of BAR0 | GPU writes AXI-Lite registers of the `slash_add` HLS kernel. No RP1 involvement. |
| **rp1_bringup_gpu** *(this example)* | **GPU + dma-buf import of BAR4** | **GPU submits a one-node SIGNAL graph to RP1. Combines `rp1_bringup signal` + `08_p2p_gpu` into a single end-to-end test.** |

## Verified working configuration

End-to-end PASS was confirmed on:

| | |
|---|---|
| GPU | AMD Instinct MI100 (`gfx908`) |
| FPGA | Xilinx Alveo V80 (`0000:21:00.2`, PF2 BAR4 prefetchable, 128 MB) |
| Kernel | `6.8.0-90-generic` with `CONFIG_PCI_P2PDMA=y`, `iommu=pt` |
| amdgpu | In-tree mainline (includes Felix Kuehling's 2023 patches routing `AMDKFD_IOC_IMPORT_DMABUF` through `drm_gem_prime_fd_to_handle`, in 6.7+) |
| ROCm | 7.12 built from source via [TheRock](https://github.com/ROCm/TheRock) at tag `therock-7.12`, installed to `/opt/rocm-slash` |
| rocr-runtime | Patched with [`scripts/patch_rocr_foreign_dmabuf.sh`](scripts/patch_rocr_foreign_dmabuf.sh) (foreign-dma-buf fallback in `fmm_register_graphics_handle()`) |

Older configurations that **do not** work and the failure they produce:

- ROCm ≤ 6.4 + any kernel → `AMDKFD_IOC_GET_DMABUF_INFO` returns `-EINVAL`, HSA reports generic `HSA_STATUS_ERROR`. The kernel-side `IMPORT_DMABUF` rejection was only lifted in 6.7+, and the userspace-side `GET_DMABUF_INFO` rejection is still present even in 7.12 preview without the local patch.
- ROCm ≥ 6.5 + kernel < 6.7 → same symptom; the kernel-side gate is still in place.
- Kernel without `CONFIG_PCI_P2PDMA` → slash driver logs `kernel built without CONFIG_PCI_P2PDMA` at probe; example exits with `BAR4 is not usable`.
- Bitstream where BAR4 is non-prefetchable → slash logs `BAR4 P2PDMA registration failed: -EINVAL`; example exits with `BAR4 is not usable`.

## Prerequisites

1. **Bitstream with PF2 BAR4 prefetchable.** The loaded PDI must mark
   BAR4 prefetchable so `pci_p2pdma_add_resource(pdev, 4, 0, 0)` in
   `driver/slash_ctldev.c` succeeds during probe. Verify by checking
   `dmesg` after module load:

   ```text
   slash 0000:21:00.2: ctldev: BAR4 registered with P2PDMA
   ```

   If you see `BAR4 P2PDMA registration failed: -EINVAL` instead, BAR4
   is not prefetchable in the running bitstream and the example will
   exit with `BAR4 is not usable` before launching the GPU kernel.

   Confirm at the PCIe level with:

   ```bash
   lspci -vvs 0000:21:00.2 | grep -iE 'Region 4|prefetchable'
   # Want: "Region 4: Memory at ... (64-bit, prefetchable) [size=...]"
   ```

2. **RP1 firmware loaded.** Build `rp1.elf` with `RP1_POLLING_BRINGUP=ON`
   and load it via xsdb on R5 core 1 — same flow as
   [`examples/rp1_bringup`](../rp1_bringup/) /
   [`examples/07_rp1_memcheck`](../07_rp1_memcheck/):

   ```bash
   cd linker/resources/aved/rp1
   cmake -S . -B build-bringup -DRP1_POLLING_BRINGUP=ON
   cmake --build build-bringup
   # produces build-bringup/rp1.elf — load via xsdb
   ```

   After load the firmware should sit in `RP1_STATE_READY` with
   `heartbeat` advancing. Confirm with `rp1_bringup dump
   /dev/slash_ctl0` from the host CPU before running the GPU test.

3. **Kernel ≥ 6.7 with `CONFIG_PCI_P2PDMA=y` and `iommu=pt`.** Earlier
   kernels lack the mainline amdgpu changes that allow third-party
   dma-buf import through `drm_gem_prime_fd_to_handle()`. Confirm:

   ```bash
   uname -r                                              # >= 6.7
   grep CONFIG_PCI_P2PDMA "/boot/config-$(uname -r)"     # =y
   cat /proc/cmdline | grep iommu=pt
   ```

4. **ROCm ≥ 7.x with the foreign-dma-buf rocr-runtime patch.** The
   example uses `hsa_amd_interop_map_buffer` to import the BAR4 dma-buf
   into the GPU's address space. The HSA runtime in every released
   version through 7.12 gates the import on `AMDKFD_IOC_GET_DMABUF_INFO`,
   which the kernel returns `-EINVAL` for any non-amdgpu source. A small
   patch teaches `libhsakmt`'s `fmm_register_graphics_handle()` to treat
   `EINVAL` as "foreign source, synthesize stub metadata" and proceed
   into `AMDKFD_IOC_IMPORT_DMABUF`, which since 6.7 routes foreign
   dma-bufs through `drm_gem_prime_fd_to_handle()` and accepts them.

   The simplest way to get a patched ROCm is to use the
   [`scripts/build_rocm_with_foreign_dmabuf.sh`](scripts/build_rocm_with_foreign_dmabuf.sh)
   orchestrator. From a fresh box with `git`, `python3`, `sudo`:

   ```bash
   # Defaults: GPU_ARCH=gfx908, INSTALL_PREFIX=/opt/rocm-slash,
   # SOURCE_DIR=<this dir>/rocm-slash-src (gitignored), THEROCK_TAG=therock-7.12.
   # Override any of these as env vars.
   ./scripts/build_rocm_with_foreign_dmabuf.sh
   ```

   It does everything: apt-installs build deps, clones TheRock, runs
   `fetch_sources.py`, applies the patch, configures a trim build
   (compiler + core + HIP runtimes only), builds (~1 hour), installs to
   `$INSTALL_PREFIX`, registers the lib path with `ldconfig`, and
   verifies the patched string is present in the installed
   `libhsa-runtime64.so`. The script is idempotent — re-running it
   short-circuits each phase whose outputs already exist.

   If you already have a TheRock build and only want to apply the patch
   and rebuild, use the inner script directly:

   ```bash
   ./scripts/patch_rocr_foreign_dmabuf.sh
   ```

5. **No concurrent BAR4 user.** Phase-1 driver policy (see
   `driver/slash_dmabuf.c`) serializes CPU mmap and GPU P2P attach.
   While this example has BAR4 attached, any other process trying to
   `slash_bar_file_open(BAR4)` gets `-EBUSY`. Don't run `rp1_bringup`
   or `07_rp1_memcheck` in parallel with this example.

## Build

```bash
cd examples/rp1_bringup_gpu
cmake -B build -S . \
    -DCMAKE_HIP_COMPILER=/opt/rocm-slash/bin/amdclang++ \
    -DGPU_ARCH=gfx908
cmake --build build
```

Adjust `GPU_ARCH` to match your GPU (e.g. `gfx908` for MI100, `gfx90a`
for MI200, `gfx942` for MI300, `gfx1100` for RX 7900). Adjust
`CMAKE_HIP_COMPILER` to wherever TheRock installed `amdclang++`.

If ROCm is not installed, the build still produces a stub binary that
prints an error and exits — useful for syntax-checking the host C++ on
machines without a GPU.

## Run

```bash
sudo LD_LIBRARY_PATH=/opt/rocm-slash/lib ./build/rp1_bringup_gpu /dev/slash_ctl0
```

The `LD_LIBRARY_PATH` is necessary because `sudo` strips it from the
environment by default. Alternatively, register the source-built ROCm
with the dynamic linker once and drop the prefix:

```bash
echo /opt/rocm-slash/lib | sudo tee /etc/ld.so.conf.d/rocm-slash.conf
sudo ldconfig
sudo ./build/rp1_bringup_gpu /dev/slash_ctl0
```

Expected output on success (numbers will vary):

```text
SLASH device: 0000:c1:00.2  vendor=0x10ee  device=0x50c2
BAR4  addr=0x...  len=0x20000000  in_use=0
BAR dma-buf fd=3  len=536870912
GPU agent: gfx90a
BAR4 mapped into GPU VA: 0x...  mapped_size=536870912
Launching GPU kernel: rp1_bringup_signal_gpu<<<1, 1>>>
status         = 1 (PASS)
magic_seen     = 0x53515231 (SQR1)
slot0          = 0xdeadbeef
graph_done_seq = 1
rp1_state      = 1 (READY)
polls          = 4
PASSED
```

## Troubleshooting

The kernel always populates the result struct even on failure, so the
diagnostic block is printed unconditionally. Use it together with the
host-side `rp1_bringup dump /dev/slash_ctl0` (which reads the same
control block from the CPU side).

| Observed | Likely cause | Next step |
|---|---|---|
| `BAR4 is not usable` (host throw, before GPU launch) | Bitstream did not mark PF2 BAR4 prefetchable, or `pci_p2pdma_add_resource()` failed at probe. | `dmesg \| grep "BAR4 P2PDMA"`. Reload a bitstream that has BAR4 prefetchable. |
| `BAR4 length 0x... < required 0x...` | BAR4 is too small to cover the 64 MiB control offset + signal array. | Confirm the bitstream uses the standard 512 MB BAR4 (`CPM_PCIE1_PF2_BAR4_QDMA_SIZE {512}`). |
| `HSA error in hsa_amd_interop_map_buffer` | GPU and FPGA are not P2P-reachable (different root complex without ACS relaxation), or the kernel was built without `CONFIG_PCI_P2PDMA`. | `dmesg \| grep "P2P not supported"`. Re-check IOMMU / ACS settings. |
| `status = 2 (NO_FIRMWARE)` and `magic_seen = 0x00000000` or `0xFFFFFFFF` | RP1 firmware never wrote the control block, or the BAR4 + 64 MiB window doesn't reach RP1's DDR. | Run `rp1_bringup dump /dev/slash_ctl0` from the CPU. If `magic` is also bad there, reload `rp1.elf`. If only the GPU sees `0xFFFFFFFF`, the GPU mapping is the problem (e.g. mapped beyond the BAR's prefetchable region). |
| `status = 4 (TIMEOUT)`, `polls` very large, `graph_done_seq` did not advance | RP1 received `graph_seq` but never advanced `graph_done_seq`. Often the `wfi` problem. | Confirm `rp1.elf` was built with `RP1_POLLING_BRINGUP=ON`. Re-run `rp1_bringup dump`; `heartbeat` should still be advancing. If `rp1_state == 3 (ERROR)`, check `rp1_error_code`. |
| `status = 3 (BAD_SLOT)`, graph completed | Graph executed but slot 0 holds a value other than `0xDEADBEEF`. Either the signal-array offset doesn't match what RP1 expects, or RP1 wrote a different slot. | Compare `slot0` against `RP1_DEFAULT_SIG_ARRAY_OFFSET` (`0x151000` from `rp1_protocol.h`) and re-check the `sig_array_base_lo` value the kernel programmed. |
| Host hangs in `hipDeviceSynchronize` | The GPU's PCIe writes never reached RP1 (write-combine never flushed) or the GPU kernel hit an unrecoverable fault. | Sanity-check `07_rp1_memcheck` reads the expected pattern first — that proves BAR4 + 64 MiB window mapping is sound from the CPU side. |

## Implementation notes

- **Single-thread kernel.** The RP1 protocol is sequential per graph;
  the kernel runs as `<<<1, 1>>>`. A multi-thread version would only
  help once we move to a multi-graph or concurrent-graph workload.
- **Memory ordering.** BAR4 is host-side write-combine (because it is
  prefetchable). The kernel uses `__threadfence_system()` before the
  `graph_seq` bump (so RP1 cannot observe the new sequence number
  before the node + ctrl writes have landed in DDR), after the bump,
  and on each iteration of the polling loop (so each `graph_done_seq`
  read is fresh from DDR rather than the GPU's last-cached value).
- **Layout constants.** All offsets come from
  [`driver/libslash/include/slash/uapi/rp1_protocol.h`](../../driver/libslash/include/slash/uapi/rp1_protocol.h)
  (the single source of truth shared between firmware and host); the
  64 MiB BAR-relative window offset matches
  [`examples/rp1_bringup/rp1_bringup.c`](../rp1_bringup/rp1_bringup.c)
  and [`examples/07_rp1_memcheck/07_rp1_memcheck.c`](../07_rp1_memcheck/07_rp1_memcheck.c).
- **Polling deadline.** The kernel uses `clock64()` against a 6×10⁹
  cycle budget, which is roughly 3–6 s of wall-clock at typical AMD
  GPU reference clocks. Comfortably above the host-side
  `rp1_bringup` `POLL_TIMEOUT_NS = 3 s`.
