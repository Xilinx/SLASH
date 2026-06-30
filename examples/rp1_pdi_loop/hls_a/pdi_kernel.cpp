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
 *
 * pdi_kernel — variant A. AXI-Lite-only HLS kernel whose only side
 * effect is writing the magic constant 0xAAAA_AAAA to its `out` output
 * register.  After `ap_done`, the register at AXI-Lite offset +0x10
 * holds the magic; an RP1 `SCALAR_READ` node forwards that into a
 * signal-array slot so the host can confirm which variant just ran.
 *
 * Both `hls_a/` and `hls_b/` define the same symbol with the same
 * AXI-Lite signature; the only difference is the magic constant.  Each
 * is packed into its own vbin (`rp1_pdi_loop_a_hw.vbin` /
 * `rp1_pdi_loop_b_hw.vbin`) by the parent `CMakeLists.txt`.  The
 * linker places the single user-region kernel at host
 * `0x0202_0000_0000` → R5 `0x8800_0000` in both designs, so a single
 * `KERNEL_DISPATCH` opcode in the RP1 graph targets whichever variant
 * is currently loaded.
 */

#include <ap_int.h>

void pdi_kernel(ap_uint<32>* out) {
#pragma HLS interface mode=s_axilite port=out
#pragma HLS interface mode=s_axilite port=return

    *out = 0xAAAAAAAA;
}
