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

"""SLR floorplanning data for the V80 shells and the [floorplan] pblock geometry."""

from __future__ import annotations

import importlib.resources as resources
import json
import re
from typing import Dict, List, Optional, Tuple

# SLRs that hold user-region logic. The service shell's user region has no SLR0.
ALLOWED_SLRS: Dict[str, Tuple[int, ...]] = {
    "compute": (0, 1, 2),
    "service": (1, 2),
}

# Where each shell installs the slash_base block design it was built with.
_SLASH_BASE_BD_PACKAGE: Dict[str, str] = {
    "compute": "slashkit.resources.static_shell_compute.slash_base",
    "service": "slashkit.resources.static_shell.slash_base",
}
_NUM_MEM_PORTS = 8

# Inclusive site-Y span of SLR0, SLR1, SLR2 on xcv80 per site type (None: no such
# site in that SLR), covering every type used in the shells' pblocks. Read from the
# device with Vivado 2025.1: for each SLR, min/max of the Y coordinate of
# [get_sites -of $slr] per type, and of [get_clock_regions -of $slr] for CLOCKREGION.
_Span = Optional[Tuple[int, int]]
_SLR_ROWS: Dict[str, Tuple[_Span, _Span, _Span]] = {
    "BUFGCE":       ((0, 23), None, None),
    "BUFG_FABRIC":  ((0, 95), (96, 167), (168, 239)),
    "BUFG_GT":      ((0, 95), (96, 167), (168, 239)),
    "BUFG_GT_SYNC": ((0, 163), (164, 286), (287, 409)),
    "BUFG_PS":      ((0, 35), (36, 47), (48, 61)),
    "CLOCKREGION":  ((0, 4), (5, 7), (8, 11)),
    "DCMAC":        ((0, 0), (1, 2), None),
    "DPLL":         ((0, 7), (8, 13), (14, 19)),
    "DSP":          ((0, 165), (166, 309), (310, 451)),
    "DSP58_CPLX":   ((0, 165), (166, 309), (310, 451)),
    "GTM_QUAD":     (None, (7, 10), None),
    "GTM_REFCLK":   (None, (14, 21), None),
    "HSC":          ((0, 0), (1, 1), (2, 2)),
    "ILKNF":        (None, (0, 0), None),
    "IRI_QUAD":     ((0, 1355), (1356, 2507), (2508, 3671)),
    "MMCM":         ((0, 0), None, None),
    "MRMAC":        ((0, 1), (2, 3), (4, 4)),
    "NOC_NMU512":   ((0, 6), (7, 12), (13, 18)),
    "NOC_NPS_VNOC": ((0, 13), (14, 25), (26, 37)),
    "NOC_NSU512":   ((0, 6), (7, 12), (13, 18)),
    "RAMB18":       ((0, 167), (168, 311), (312, 455)),
    "RAMB36":       ((0, 83), (84, 155), (156, 227)),
    "SLICE":        ((0, 331), (332, 619), (620, 903)),
    "URAM288":      ((0, 83), (84, 155), (156, 227)),
    "URAM_CAS_DLY": ((0, 3), (4, 6), (7, 9)),
}

_RE_SLASH_RESIZE = re.compile(
    r"resize_pblock\s+\[get_pblocks\s+pblock_slash\]\s+-add\s+\{([^}]*)\}")
_RE_SITE = re.compile(r"^(\w+)_X(\d+)Y(\d+)$")


def reset_replica_name(slr: Optional[int]) -> str:
    """Reset replica register feeding the kernels pinned to slr (None: unpinned)."""
    return "rst_repl_kernels" if slr is None else f"rst_repl_slr{slr}"


def site_slr(site: str) -> Optional[int]:
    """SLR holding a site, or None if the site type or row is unknown."""
    kind, _, y = _parse_site(site)
    for slr, span in enumerate(_SLR_ROWS.get(kind, ())):
        if span is not None and span[0] <= y <= span[1]:
            return slr
    return None


def mem_slr_from_bd(bd_text: str) -> Optional[Tuple[int, ...]]:
    """
    SLR of each MEM port (index = MEM<i>) from a slash_base.bd, read off the
    PHYSICAL_LOC of /hbm_vnoc_0<i>/S00_AXI. The static shell pins those NMUs and
    locks the NoC paths into the user region, so this is where they are in every
    design linked against that shell. None if any port is unpinned.
    """
    components = json.loads(bd_text).get("design", {}).get("components", {})
    slrs = []
    for i in range(_NUM_MEM_PORTS):
        port = (components.get(f"hbm_vnoc_0{i}", {})
                .get("interface_ports", {}).get("S00_AXI", {}))
        site = port.get("parameters", {}).get("PHYSICAL_LOC", {}).get("value", "")
        slr = site_slr(site) if site else None
        if slr is None:
            return None
        slrs.append(slr)
    return tuple(slrs)


def installed_mem_slr(shell: str) -> Optional[Tuple[int, ...]]:
    """mem_slr_from_bd for the installed shell; None if it is not installed."""
    try:
        bd = resources.files(_SLASH_BASE_BD_PACKAGE[shell]).joinpath("slash_base.bd")
        return mem_slr_from_bd(bd.read_text(encoding="utf-8"))
    except (ModuleNotFoundError, FileNotFoundError):
        return None


def _parse_site(site: str) -> Tuple[str, int, int]:
    m = _RE_SITE.match(site)
    if not m:
        raise ValueError(f"Unrecognised pblock site '{site}'.")
    return m.group(1), int(m.group(2)), int(m.group(3))


def _slash_pblock_ranges(shell: str) -> List[str]:
    """Every range added to pblock_slash in the shell's impl.xdc, in file order."""
    xdc = resources.files(
        f"slashkit.resources.base.{shell}.constraints").joinpath("impl.xdc")
    ranges: List[str] = []
    for body in _RE_SLASH_RESIZE.findall(xdc.read_text(encoding="utf-8")):
        ranges.extend(body.split())
    return ranges


def slr_pblock_ranges(shell: str, slr: int) -> List[str]:
    """
    The part of the shell's pblock_slash that lies in one SLR. SLR boundaries are
    horizontal, so clipping each range's Y span to the SLR keeps it a rectangle;
    ranges with nothing in the SLR are dropped.
    """
    clipped: List[str] = []
    for rng in _slash_pblock_ranges(shell):
        lo_site, _, hi_site = rng.partition(":")
        kind, x0, y0 = _parse_site(lo_site)
        kind_hi, x1, y1 = _parse_site(hi_site or lo_site)
        if kind_hi != kind or kind not in _SLR_ROWS:
            raise ValueError(f"No SLR data for pblock_slash range '{rng}'.")
        span = _SLR_ROWS[kind][slr]
        if span is None:
            continue
        x0, x1 = sorted((x0, x1))
        y0, y1 = max(min(y0, y1), span[0]), min(max(y0, y1), span[1])
        if y0 <= y1:
            clipped.append(f"{kind}_X{x0}Y{y0}:{kind}_X{x1}Y{y1}")
    return clipped


def floorplan_xdc(shell: str, slr_by_inst: Dict[str, int]) -> str:
    """
    Implementation constraints for [floorplan]: one child pblock of pblock_slash per
    used SLR, holding the kernel cells pinned to it and the reset replica that
    feeds them. The rest of the linker-generated glue on their ports stays free.
    """
    lines = ["# Generated by slashkit from the [floorplan] section."]
    for slr in sorted(set(slr_by_inst.values())):
        pblock = f"pblock_slash_slr{slr}"
        cells = " ".join(
            [f"top_i/slash/{inst}" for inst, s in slr_by_inst.items() if s == slr]
            + [f"top_i/slash/{reset_replica_name(slr)}"])
        lines += [
            f"create_pblock {pblock}",
            f"resize_pblock [get_pblocks {pblock}] -add "
            f"{{{' '.join(slr_pblock_ranges(shell, slr))}}}",
            f"set_property PARENT pblock_slash [get_pblocks {pblock}]",
            # A child pblock is created soft, which lets the placer move pinned
            # cells across the SLR boundary.
            f"set_property IS_SOFT FALSE [get_pblocks {pblock}]",
            # -quiet: an SLR whose kernels have no reset port has no replica cell.
            f"add_cells_to_pblock [get_pblocks {pblock}] [get_cells -quiet {{{cells}}}]",
        ]
    return "\n".join(lines) + "\n"
