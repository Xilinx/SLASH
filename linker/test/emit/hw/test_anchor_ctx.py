# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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

"""Tests for the AXI-Lite window anchor (issue 228).

The property under test is the one the hardware actually cares about: the span
of assigned AXI-Lite segments must equal the aperture the static shell
declares, because that span -- not the declared CONFIG.APERTURES -- is what the
NoC compiler sizes the RP's NSU from.
"""

import pytest

from slashkit.emit.hw.user_region.addr_ctx import build_axilite_address_context
from slashkit.emit.hw.user_region.anchor_ctx import (
    ANCHOR_NAME,
    ANCHOR_RANGE,
    WINDOW_BYTES,
    assert_window_spanned,
    build_window_anchor_slaves,
)

BASE = 0x0202_0000_0000

# Fixed offset and range of the debug hub, mirroring debug_ctx so the test does
# not depend on debug being configured.
HUB_SLAVE = {
    "dst_pin": "axi_dbg_hub_0/S_AXI",
    "inst": "axi_dbg_hub_0",
    "busif": "S_AXI_DBG_HUB",
    "segment": "Mem0",
    "range": 0x0020_0000,
    "offset": 0x0202_0060_0000,
}


def _kernel_entry(name, offset, rng=0x1_0000):
    return {
        "inst": name, "busif": "s_axi_control", "segment": "Reg",
        "offset": offset, "range": rng, "addr_space": "S_AXILITE_INI",
    }


def test_anchor_sits_in_the_last_window_of_the_aperture():
    (anchor,) = build_window_anchor_slaves(BASE)
    assert anchor["inst"] == ANCHOR_NAME
    assert anchor["offset"] == BASE + WINDOW_BYTES - ANCHOR_RANGE
    assert anchor["offset"] + anchor["range"] == BASE + WINDOW_BYTES


def test_anchor_offset_tracks_a_different_aperture_base():
    """The service-layer RP sits at 0x203_0000_0000, not 0x202_0000_0000."""
    (anchor,) = build_window_anchor_slaves(0x0203_0000_0000)
    assert anchor["offset"] + anchor["range"] == 0x0203_0000_0000 + WINDOW_BYTES


def test_single_kernel_map_spans_the_full_aperture_with_the_anchor():
    """A one-kernel design is the case that produced a 64 KiB NSU (04_freq)."""
    addr = [_kernel_entry("vadd_0", BASE)] + build_window_anchor_slaves(BASE)
    top = max(a["offset"] + a["range"] for a in addr)
    assert top - BASE == WINDOW_BYTES
    assert_window_spanned(addr, BASE)


def test_assert_rejects_a_map_without_the_anchor():
    """Without the anchor the span is design-sized -- the issue-228 bug."""
    addr = [_kernel_entry("vadd_0", BASE)]
    with pytest.raises(ValueError, match="window anchor"):
        assert_window_spanned(addr, BASE)


def test_assert_rejects_a_map_overflowing_the_aperture():
    addr = [_kernel_entry("big_0", BASE, rng=WINDOW_BYTES * 2)]
    with pytest.raises(ValueError, match="outside the"):
        assert_window_spanned(addr, BASE)


def test_assert_rejects_an_empty_map():
    with pytest.raises(ValueError, match="no AXI-Lite address segments"):
        assert_window_spanned([], BASE)


def test_address_context_places_the_anchor_without_disturbing_kernels():
    """The anchor is an extra slave, so kernels keep the offsets they had."""
    ctx = build_axilite_address_context(
        {},
        base_offset=BASE,
        min_align=0x1_0000,
        extra_slaves=[HUB_SLAVE] + build_window_anchor_slaves(BASE),
    )
    by_inst = {e["inst"]: e for e in ctx["axilite_addr"]}

    # The debug hub keeps its pinned address, which host tooling relies on.
    assert by_inst["axi_dbg_hub_0"]["offset"] == HUB_SLAVE["offset"]
    assert by_inst[ANCHOR_NAME]["offset"] == BASE + WINDOW_BYTES - ANCHOR_RANGE
    assert_window_spanned(ctx["axilite_addr"], BASE)


def test_anchor_and_debug_hub_do_not_overlap():
    """Both are fixed-offset slaves; addr_ctx guards overlap, so prove it passes."""
    ctx = build_axilite_address_context(
        {},
        base_offset=BASE,
        min_align=0x1_0000,
        extra_slaves=[HUB_SLAVE] + build_window_anchor_slaves(BASE),
    )
    windows = sorted((e["offset"], e["offset"] + e["range"])
                     for e in ctx["axilite_addr"])
    for (_, end), (start, _) in zip(windows, windows[1:]):
        assert end <= start


# --------------------------------------------------------------------------
# Service-layer RP. Same defect, different aperture base and a separate
# fan-out: the DCMAC control segments top out at 0x203_0304_0000, so without an
# anchor that RP's NSU is 64 MiB against a declared 128 MiB, and it moves with
# the [network] configuration.
# --------------------------------------------------------------------------

class _Net:
    def __init__(self, enabled):
        self.enabled_eth = set(enabled)


def test_service_layer_anchor_absent_without_networking():
    """No DCMAC means no fan-out, no anchor, and no service-layer RM build."""
    from slashkit.emit.hw.service_region.service_layer_ctx import (
        build_service_axilite_ctx,
    )
    ctx = build_service_axilite_ctx(_Net([]))
    assert ctx["sl_have_xbar"] is False
    assert ctx["sl_window_anchor_enabled"] is False
    assert ctx["sl_num_mi"] == 0


@pytest.mark.parametrize("enabled,dcmacs", [([0], 1), ([0, 2], 2)])
def test_service_layer_anchor_takes_the_trailing_mi_slot(enabled, dcmacs):
    from slashkit.emit.hw.service_region.service_layer_ctx import (
        build_service_axilite_ctx,
        SL_ANCHOR_NAME,
        SL_AXILITE_BASE,
    )
    ctx = build_service_axilite_ctx(_Net(enabled))
    assert ctx["sl_have_xbar"] is True
    assert ctx["sl_window_anchor_enabled"] is True
    # One MI per DCMAC, plus the anchor last.
    assert ctx["sl_num_mi"] == dcmacs + 1
    assert ctx["sl_mi_targets"][-1] == f"{SL_ANCHOR_NAME}/S_AXI"
    # The anchor ends exactly on the aperture boundary.
    offset = int(ctx["sl_window_anchor_offset"], 16)
    rng = int(ctx["sl_window_anchor_range"], 16)
    assert offset + rng == SL_AXILITE_BASE + WINDOW_BYTES


def test_service_layer_anchor_does_not_collide_with_dcmac_windows():
    """DCMAC1's window is the highest fixed one, at 0x203_0300_0000 + 256 KiB."""
    from slashkit.emit.hw.service_region.service_layer_ctx import (
        build_service_axilite_ctx,
    )
    ctx = build_service_axilite_ctx(_Net([0, 2]))
    anchor = int(ctx["sl_window_anchor_offset"], 16)
    assert anchor >= 0x0203_0300_0000 + 0x0004_0000
