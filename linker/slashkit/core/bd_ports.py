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

from __future__ import annotations
from dataclasses import dataclass, field
from typing import Dict, Iterable, Optional, List, Tuple
import re

from slashkit.core.port import BusType


# -----------------------------
# Data classes
# -----------------------------

@dataclass(frozen=True)
class BdPort:
    """
    A top-level Block Design port / shell endpoint.

    Attributes:
        name:      Logical name used by your tool (e.g., "HBM0", "DDR0", "VIRT0", "MEM", "clock", "reset").
        ptype:     BusType (AXI4FULL, AXILITE, AXIS, CLOCK, RESET, INTERRUPT).
        rtl_name:  Actual BD interface/pin name in Vivado (e.g., "HBM_AXI_00", "M00_INI", "VIRT_AXI_00", "ap_clk").
        width:     Optional data width (AXI/AXIS). For CLOCK/RESET/INTERRUPT, this is forced to 1.
        domain:    Optional grouping, inferred from logical name for memory ports ("HBM", "DDR", "VIRT", "MEM").
        index:     Optional index for memory groups (e.g., HBM0..63 → 0..63, DDR0..3 → 0..3, VIRT0..3 → 0..3).
                   For MEM lines, usually None (MEM acts as a multi-entry wildcard).
    """
    name: str
    ptype: BusType
    rtl_name: Optional[str] = None
    width: Optional[int] = None
    domain: Optional[str] = None
    index: Optional[int] = None


@dataclass
class BlockDesignPorts:
    """
    Registry of BD ports with support for multiple RTL endpoints per logical name
    (e.g., a single logical 'MEM' mapping to multiple vNOC INI ports).
    """
    ports: Dict[str, List[BdPort]] = field(default_factory=dict)

    # ---- registration ----

    def add(self, port: BdPort) -> None:
        lst = self.ports.setdefault(port.name, [])
        # Avoid exact duplicate RTL entry under the same logical name & type
        if any(p.rtl_name == port.rtl_name and p.ptype == port.ptype for p in lst):
            raise ValueError(
                f"BD port '{port.name}' already has RTL '{port.rtl_name}' of type {port.ptype.name}."
            )
        lst.append(port)

    def add_many(self, ports: Iterable[BdPort]) -> None:
        for p in ports:
            self.add(p)

    # ---- lookups ----

    def get(self, name: str) -> BdPort:
        """Return a single port for 'name'. Error if zero or more than one exist."""
        lst = self.ports.get(name, [])
        if not lst:
            raise KeyError(f"BD port '{name}' not found.")
        if len(lst) > 1:
            raise ValueError(
                f"Multiple BD ports registered for '{name}'. Use get_all('{name}').")
        return lst[0]

    def get_all(self, name: str) -> List[BdPort]:
        """Return all ports for 'name' (useful for 'MEM')."""
        lst = self.ports.get(name, [])
        if not lst:
            raise KeyError(f"BD port '{name}' not found.")
        return lst

    def iter_type(self, ptype: BusType):
        """Iterate all BdPort entries of a given type across all logical names."""
        for lst in self.ports.values():
            for p in lst:
                if p.ptype == ptype:
                    yield p

    # ---- memory resolution ----

    def mem_targets(self, domain: str, index: Optional[int] = None) -> List[BdPort]:
        """
        Resolve memory target(s) to BdPort(s):
          - ('HBM', i)  -> [ 'HBM{i}'  ]
          - ('DDR', i)  -> [ 'DDR{i}'  ]
          - ('VIRT', i) -> [ 'VIRT{i}' ]
          - ('MEM', None) -> all 'MEM' entries (file order)
          - ('MEM', i)  -> the i-th 'MEM' entry (by file order)
        """
        d = domain.upper()
        if d in ("HBM", "DDR", "VIRT"):
            if index is None:
                raise ValueError(f"{d} requires an index.")
            return [self.get(f"{d}{index}")]
        if d == "MEM":
            mems = self.get_all("MEM")
            if index is None:
                return mems
            if not (0 <= index < len(mems)):
                raise IndexError(
                    f"MEM index {index} out of range (0..{len(mems)-1}).")
            return [mems[index]]
        if d == "HOST":
            # single logical endpoint named 'HOST' in bd_ports.txt
            return [self.get("HOST")]
        raise ValueError(f"Unknown memory domain '{domain}'.")

    def mem(self, domain: str, index: Optional[int]) -> BdPort:
        """
        Back-compat convenience: return a single BdPort.
        For MEM, you must pass an index; otherwise use mem_targets('MEM').
        """
        targets = self.mem_targets(domain, index)
        if len(targets) != 1:
            raise ValueError(
                f"mem('{domain}', index={index}) resolved to {len(targets)} ports. "
                f"Use mem_targets('{domain}', index) instead."
            )
        return targets[0]


# -----------------------------
# Loader from text file
# -----------------------------

_TYPE_MAP = {
    "AXI4FULL": BusType.AXI4FULL,
    "AXILITE":  BusType.AXILITE,
    "AXIS":     BusType.AXIS,
    "CLOCK":    BusType.CLOCK,
    "RESET":    BusType.RESET,
    "INTERRUPT": BusType.INTERRUPT,
}

# HBM / DDR / VIRT with trailing index, e.g. HBM12, DDR3, VIRT2
_RE_LOGICAL_MEM = re.compile(r"^(HBM|DDR|VIRT)(\d+)$", re.IGNORECASE)


def _parse_ptype(s: str) -> BusType:
    try:
        return _TYPE_MAP[s.strip().upper()]
    except KeyError:
        raise ValueError(
            f"Unknown port type '{s}'. Expected one of {list(_TYPE_MAP)}.")


def _infer_domain_index(logical_name: str) -> Tuple[Optional[str], Optional[int]]:
    """
    Best-effort inference from logical name:
       HBM0..HBM63  -> ('HBM',  0..63)
       DDR0..DDR3   -> ('DDR',  0..3)
       VIRT0..VIRT3 -> ('VIRT', 0..3)
       MEM          -> ('MEM', None)
       HOST         -> ('HOST', None)
    """
    ln = logical_name.strip()
    if ln.upper() == "MEM":
        return "MEM", None
    if ln.upper() == "HOST":
        return "HOST", None
    m = _RE_LOGICAL_MEM.match(ln)
    if m:
        return m.group(1).upper(), int(m.group(2))
    return None, None


def _parse_width(s: Optional[str]) -> Optional[int]:
    if not s:
        return None
    try:
        return int(s, 0)  # supports "32", "0x20"
    except ValueError:
        return None


def load_bd_ports_from_file(path: str) -> BlockDesignPorts:
    """
    File format (one entry per line; comments with # or ; are ignored):

        <logicalName>:<rtlName> <type> [width]

    Examples:
        HBM0:HBM_AXI_00 AXI4FULL
        DDR0:M00_INI AXI4FULL
        VIRT0:VIRT_AXI_00 AXI4FULL
        S_AXI_CTRL:S_AXI_CTRL AXILITE 32
        clock:ap_clk CLOCK
        reset:ap_rst_n RESET

        # 'MEM' repeated N times (single logical, many RTL endpoints)
        MEM:HBM_VNOC_INI_00 AXI4FULL
        MEM:HBM_VNOC_INI_01 AXI4FULL
        ...
        MEM:HBM_VNOC_INI_07 AXI4FULL
    """
    bd = BlockDesignPorts()
    with open(path, "r", encoding="utf-8") as f:
        for ln, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith(";"):
                continue

            # Split "<logical>:<rtl>" and "<type> [width]"
            try:
                lhs, rhs = line.split(None, 1)
            except ValueError:
                raise ValueError(
                    f"{path}:{ln}: Expected '<logical>:<rtl> <type> [width]'. Got: {line!r}")

            if ":" not in lhs:
                raise ValueError(
                    f"{path}:{ln}: Missing ':' in '{lhs}'. Expected '<logical>:<rtl>'.")

            logical, rtl = [t.strip() for t in lhs.split(":", 1)]
            parts = rhs.split()
            if len(parts) not in (1, 2):
                raise ValueError(
                    f"{path}:{ln}: Invalid RHS. Expected '<type> [width]'. Got: {rhs!r}")

            ptype = _parse_ptype(parts[0])
            width = _parse_width(parts[1]) if len(parts) == 2 else None

            # Force scalar width for these types
            if ptype in (BusType.CLOCK, BusType.RESET, BusType.INTERRUPT):
                width = 1

            domain, index = _infer_domain_index(logical)
            bd.add(BdPort(
                name=logical,
                ptype=ptype,
                rtl_name=rtl,
                width=width,
                domain=domain,
                index=index
            ))

    return bd


# -----------------------------
# Optional helpers
# -----------------------------

def generate_bd_port_lines(
    num_hbm: int = 64,
    num_ddr: int = 4,
    num_mem_vnoc: int = 8,
    num_virt: int = 4,
) -> List[str]:
    """
    Utility to prefill a mapping file for common shells.

    Returns a list of lines ready to write to a file. Note that 'MEM' is
    repeated num_mem_vnoc times with different RTL names, by design.
    """
    lines: List[str] = []
    # HBM
    for i in range(num_hbm):
        lines.append(f"HBM{i}:HBM_AXI_{i:02d} AXI4FULL")
    # DDR
    for i in range(num_ddr):
        lines.append(f"DDR{i}:M{i:02d}_INI AXI4FULL")
    # VIRT
    for i in range(num_virt):
        lines.append(f"VIRT{i}:VIRT_AXI_{i:02d} AXI4FULL")
    # Control + clocks/resets
    lines += [
        "S_AXI_CTRL:S_AXI_CTRL AXILITE 32",
        "clock:ap_clk CLOCK",
        "reset:ap_rst_n RESET",
    ]
    # MEM (vNOC INI)
    for i in range(num_mem_vnoc):
        lines.append(f"MEM:HBM_VNOC_INI_{i:02d} AXI4FULL")
    return lines
