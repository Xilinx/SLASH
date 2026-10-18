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

"""Tests for core.floorplan — SLR tables and the per-SLR child pblock geometry."""

import importlib.resources as resources
import json
import re

import pytest

from slashkit.core import floorplan
from slashkit.core.floorplan import (
    ALLOWED_SLRS,
    floorplan_xdc,
    mem_slr_from_bd,
    site_slr,
    slr_pblock_ranges,
)

_SHELLS = ("compute", "service")
_TEMPLATE = {"compute": "slash_compute.tcl", "service": "slash.tcl"}
# Agreed MEM layout: more ports where there is no HBM edge, at least one per SLR.
_MEM_SLR = {"compute": (0, 0, 0, 1, 1, 1, 1, 2), "service": (1, 1, 2, 2, 2, 2, 2, 2)}
_RE_VNOC_PIN = re.compile(
    r"PHYSICAL_LOC \{(NOC_NMU512_X\d+Y\d+)\} \\\n(?:.*\n){3}"
    r" \] \[get_bd_intf_pins /hbm_vnoc_0(\d)/S00_AXI\]")


def _rect(rng):
    lo, _, hi = rng.partition(":")
    (kind, x0, y0), (_, x1, y1) = (floorplan._parse_site(s) for s in (lo, hi or lo))
    return kind, min(x0, x1), max(x0, x1), min(y0, y1), max(y0, y1)


def _bd(sites):
    """A minimal slash_base.bd with the given hbm_vnoc_0<i> S00_AXI pins."""
    comps = {}
    for i, site in enumerate(sites):
        params = {"CATEGORY": {"value": "pl"}}
        if site:
            params["PHYSICAL_LOC"] = {"value": site}
        comps[f"hbm_vnoc_0{i}"] = {"interface_ports": {"S00_AXI": {"parameters": params}}}
    return json.dumps({"design": {"components": comps}})


@pytest.mark.parametrize("shell", _SHELLS)
def test_static_shell_pins_mem_nmus_to_the_agreed_slrs(shell):
    # The static shell build locks these sites for every linked design.
    text = resources.files(f"slashkit.resources.base.{shell}.scripts").joinpath(
        "slash_base.tcl").read_text()
    sites = {int(i): site for site, i in _RE_VNOC_PIN.findall(text)}
    assert sorted(sites) == list(range(8))
    assert tuple(site_slr(sites[i]) for i in range(8)) == _MEM_SLR[shell]


@pytest.mark.parametrize("shell", _SHELLS)
def test_link_templates_leave_mem_nmu_sites_to_the_shell(shell):
    # A link-time pin that differs from the shell's lock fails DFX DRC HDPR-122.
    text = resources.files("slashkit.resources.templates").joinpath(
        _TEMPLATE[shell]).read_text()
    assert _RE_VNOC_PIN.findall(text) == []


def test_mem_slr_from_bd_reads_the_pinned_sites():
    sites = ["NOC_NMU512_X0Y6", "NOC_NMU512_X2Y6", "NOC_NMU512_X3Y6", "NOC_NMU512_X0Y9",
             "NOC_NMU512_X1Y9", "NOC_NMU512_X2Y9", "NOC_NMU512_X3Y9", "NOC_NMU512_X1Y14"]
    assert mem_slr_from_bd(_bd(sites)) == (0, 0, 0, 1, 1, 1, 1, 2)


def test_mem_slr_from_bd_is_none_for_a_shell_without_pins():
    sites = ["NOC_NMU512_X0Y6"] * 7 + [""]
    assert mem_slr_from_bd(_bd(sites)) is None


@pytest.mark.parametrize("shell", _SHELLS)
def test_every_allowed_slr_has_mem_ports_and_fabric(shell):
    for slr in ALLOWED_SLRS[shell]:
        assert slr in _MEM_SLR[shell]
        assert any(r.startswith("SLICE_") for r in slr_pblock_ranges(shell, slr))


def test_service_user_region_has_nothing_in_slr0():
    assert slr_pblock_ranges("service", 0) == []


@pytest.mark.parametrize("shell", _SHELLS)
@pytest.mark.parametrize("slr", (0, 1, 2))
def test_clipped_ranges_stay_inside_the_slr_and_the_parent(shell, slr):
    parent = [_rect(r) for r in floorplan._slash_pblock_ranges(shell)]
    for rng in slr_pblock_ranges(shell, slr):
        kind, x0, x1, y0, y1 = _rect(rng)
        lo, hi = floorplan._SLR_ROWS[kind][slr]
        assert lo <= y0 <= y1 <= hi
        assert any(k == kind and px0 <= x0 and x1 <= px1 and py0 <= y0 and y1 <= py1
                   for k, px0, px1, py0, py1 in parent), rng


def test_slr_split_covers_the_whole_parent_slice_area():
    # Splitting pblock_slash by SLR must lose nothing: the SLICE area of the
    # three slices adds up to the parent's.
    def area(ranges):
        cells = set()
        for r in ranges:
            kind, x0, x1, y0, y1 = _rect(r)
            if kind == "SLICE":
                cells |= {(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)}
        return cells
    parent = area(floorplan._slash_pblock_ranges("compute"))
    assert area(r for s in (0, 1, 2) for r in slr_pblock_ranges("compute", s)) == parent


def test_floorplan_xdc_makes_one_child_pblock_per_used_slr():
    xdc = floorplan_xdc("compute", {"k_0": 1, "k_1": 0, "k_2": 1})
    assert xdc.count("create_pblock") == 2
    assert "set_property PARENT pblock_slash [get_pblocks pblock_slash_slr0]" in xdc
    # Child pblocks start soft; pinning must be hard.
    assert xdc.count("set_property IS_SOFT FALSE [get_pblocks pblock_slash_slr") == 2
    # Each SLR pblock also holds the reset replica feeding its kernels.
    assert ("add_cells_to_pblock [get_pblocks pblock_slash_slr1] [get_cells -quiet "
            "{top_i/slash/k_0 top_i/slash/k_2 top_i/slash/rst_repl_slr1}]") in xdc
    assert ("add_cells_to_pblock [get_pblocks pblock_slash_slr0] [get_cells -quiet "
            "{top_i/slash/k_1 top_i/slash/rst_repl_slr0}]") in xdc
