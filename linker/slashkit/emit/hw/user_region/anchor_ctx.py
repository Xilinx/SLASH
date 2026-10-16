# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

"""! @brief AXI-Lite window anchor for the reconfigurable partition.

The static shell declares a constant aperture for the RP's control interface --
`APERTURES {{0x202_0000_0000 128M}}` on `/slash/S_AXILITE_INI` in `top.tcl`, and
the same at `0x203_0000_0000` for the service-layer RP. The reconfigurable
module's NoC NSU, however, is sized by the NoC compiler from the span of the
address segments `assign_bd_address` actually mapped -- the next power of two
covering them. Declaring `CONFIG.APERTURES` on `axi_noc_0/M00_AXI` does not
change that: the property is set, is retained through `validate_bd_design`, and
is simply not what Vivado 2025.1 sizes the NSU from.

So the span tracks the design: 64 KiB for a one-kernel example, 8 MiB for a
76-kernel one. Every RM then disagrees with the shell's declared 128 MiB, and
`pr_verify` rejects the pair with NOC-Validation-16 -- "the equivalent
reconfigurable NOC instance must have the same address size across
configurations". Two RMs with different spans are likewise incompatible with
each other.

The anchor closes the gap from the only side that can move: it maps a small
always-present AXI-Lite slave into the last 64 KiB of the window, so the span
is exactly the declared aperture in every design, whatever the kernels need.
Kernels keep their existing offsets starting at the aperture base, so the
covered range is [base, base + window) regardless of whether the NSU window is
anchored at the aperture base or at the lowest mapped segment.

Measured on 04_freq against the compute shell: without the anchor the NSU comes
out 65536 and `pr_verify` reports "not compatible"; with it, 128 MiB and
"are compatible" with no other errors.
"""

from __future__ import annotations
from typing import Dict, List

#! Instance name of the anchor in the generated block design.
ANCHOR_NAME = "slash_window_anchor"
#! SmartConnect MI target.
ANCHOR_DST_PIN = f"{ANCHOR_NAME}/S_AXI"
#! Bus interface carrying the address segment.
ANCHOR_BUSIF = "S_AXI"
#! Address-segment name on that bus interface.
ANCHOR_SEGMENT = "Reg"
#! Window occupied by the anchor itself, at the very top of the aperture.
ANCHOR_RANGE = 0x0001_0000          # 64 KiB

#! Control aperture the static shell declares for an RP. MUST match the
#! APERTURES property in resources/base/*/scripts/top.tcl; a change on one side
#! without the other puts NOC-Validation-16 straight back.
WINDOW_BYTES = 0x0800_0000          # 128 MiB


def build_window_anchor_slaves(base_offset: int,
                               window_bytes: int = WINDOW_BYTES) -> List[dict]:
    """! @brief AXI-Lite slave descriptor for the window anchor.

    Shaped like `build_debug_hub_slaves` so it feeds the same two consumers:
    the SmartConnect fan-out builder (via `dst_pin`) and the address-context
    builder (as a fixed-offset extra slave).

    @param base_offset  Base of the RP control aperture.
    @param window_bytes Size of that aperture, matching the static shell.
    @return Single-element list describing the anchor.
    """
    if window_bytes < ANCHOR_RANGE:
        raise ValueError(
            f"control window {window_bytes:#x} is smaller than the anchor "
            f"itself ({ANCHOR_RANGE:#x})"
        )
    return [{
        "dst_pin": ANCHOR_DST_PIN,
        "inst": ANCHOR_NAME,
        "busif": ANCHOR_BUSIF,
        "segment": ANCHOR_SEGMENT,
        "range": ANCHOR_RANGE,
        # Fixed: the last ANCHOR_RANGE bytes of the aperture.
        "offset": base_offset + window_bytes - ANCHOR_RANGE,
    }]


def build_window_anchor_context() -> Dict[str, object]:
    """! @brief Template context that instantiates the anchor cell."""
    return {
        "window_anchor_enabled": True,
        "window_anchor_name": ANCHOR_NAME,
    }


def assert_window_spanned(axilite_addr: List[dict],
                          base_offset: int,
                          window_bytes: int = WINDOW_BYTES) -> None:
    """! @brief Fail the link if the control map does not span the aperture.

    The whole point of the anchor is that this holds for every design, so a
    violation means the anchor was dropped, displaced, or outgrown rather than
    that some design is merely unusual. Catching it here turns an eventual
    NOC-Validation-16 -- which today nothing in the flow reports -- into an
    error at Tcl-generation time, seconds in rather than an hour.

    @param axilite_addr Entries from `build_axilite_address_context`.
    @param base_offset  Base of the RP control aperture.
    @param window_bytes Size of that aperture.
    """
    if not axilite_addr:
        raise ValueError(
            "no AXI-Lite address segments: the window anchor should always "
            "contribute one"
        )

    low = min(a["offset"] for a in axilite_addr)
    top = max(a["offset"] + a["range"] for a in axilite_addr)
    want_top = base_offset + window_bytes

    if low < base_offset or top > want_top:
        raise ValueError(
            f"AXI-Lite control map [{low:#x}, {top:#x}) falls outside the "
            f"{window_bytes >> 20} MiB aperture "
            f"[{base_offset:#x}, {want_top:#x}) the static shell declares for "
            f"this partition. The design needs more control space than the "
            f"shell exposes."
        )
    if top != want_top:
        raise ValueError(
            f"AXI-Lite control map tops out at {top:#x}, not {want_top:#x}. "
            f"The window anchor at {want_top - ANCHOR_RANGE:#x} is missing, so "
            f"the NoC NSU would be sized to this design rather than to the "
            f"aperture and pr_verify would reject the image "
            f"(NOC-Validation-16)."
        )
