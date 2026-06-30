# 07_rp1_memcheck

Minimal software-only RP1 shared-memory verification flow.

This example is the host-side checker for the standalone `rp1_memtest.elf`
firmware image built from `linker/resources/aved/rp1`. The RP1 firmware writes
an incrementing 32-bit word pattern into the first 1MB of the RP1-visible
shared DDR window at `0x30000000`. On the host side, the same memory is
expected to appear at BAR4 offset `64MB`.

Pattern contract:

- total size: `1MB`
- word size: `32-bit`
- seed: `0x13579BDF`
- expected value at word `i`: `seed + i`

Build:

```bash
cd examples/07_rp1_memcheck
cmake -B build -S . -DSLASH_USE_REPO=ON
cmake --build build
```

Run:

```bash
./build/07_rp1_memcheck /dev/slash_ctl0
```

Expected workflow on experimental hardware:

1. Build `rp1_memtest.elf` from `linker/resources/aved/rp1`.
2. Load and run it via xsdb.
3. Run this checker against the correct `slash_ctl` device node.

The checker validates BAR4 usability and size before reading, then reports the
first mismatch if the pattern does not match.
