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
from typing import Dict, List, Optional, Tuple, Any
from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType


def _coerce_optional_int(v: Any) -> Optional[int]:
    """Return int(v) if v is not None and looks like an int (decimal or 0x..), else None."""
    if v is None:
        return None
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        s = v.strip()
        try:
            return int(s, 0)
        except ValueError:
            return None
    return None


def build_mem_smartconnect_context(
    instances: Dict[str, KernelInstance],
    *,
    num_mem_ports: int = 8,
    max_si: int = 16,
    base_name: str = "sc_mem",
    noc_pin_fmt: str = "/hbm_vnoc_0{index}/S00_AXI"
) -> dict:
    """
    Map all AXI4FULL masters targeting MEM to 8 VNOC slaves using round-robin,
    unless a MEM index is explicitly specified in the config (sp=...:MEM<i>).
    If a VNOC bucket has >1 masters, build a SmartConnect reduction tree with
    <= max_si SIs per node (NUM_CLKS=1, NUM_MI=1).

    Returns:
      {
        "mem_direct":      [{src_pin, dst_pin}],
        "mem_smart_nodes": [{name, num_si, si:[{slot, src}], ...}],
        "mem_smart_roots": [{sc_name, dst_pin}],
      }
    """
    # 1) Collect all AXI4FULL kernel pins that target MEM
    #    Also capture an optional explicit index (if provided by config).
    # (src_pin, explicit_index or None)
    mem_sources: List[Tuple[str, Optional[int]]] = []

    for inst in instances.values():
        mem_sp = inst.params.get("mem_sp", {})
        for k_port, tgt in mem_sp.items():
            if tgt.get("domain") == "MEM":
                # Only AXI4FULL should be in mem_sp for memory mapping
                if inst.kernel.port(k_port).ptype == BusType.AXI4FULL:
                    src_pin = f"{inst.name}/{k_port}"
                    # Some configs may carry an explicit MEM index; usually None.
                    idx = _coerce_optional_int(tgt.get("index"))
                    if idx is not None and not (0 <= idx < num_mem_ports):
                        raise ValueError(
                            f"MEM index {idx} out of range (0..{num_mem_ports-1}) for {src_pin}")
                    mem_sources.append((src_pin, idx))

    if not mem_sources:
        return {"mem_direct": [], "mem_smart_nodes": [], "mem_smart_roots": []}

    # 2) Round-robin assign to MEM buckets (respect explicit indices when present)
    buckets: Dict[int, List[str]] = {i: [] for i in range(num_mem_ports)}
    rr = 0
    for src_pin, explicit in mem_sources:
        if explicit is not None:
            buckets[explicit].append(src_pin)
        else:
            buckets[rr % num_mem_ports].append(src_pin)
            rr += 1

    mem_direct: List[dict] = []
    mem_smart_nodes: List[dict] = []
    mem_smart_roots: List[dict] = []

    # 3) For each MEM bucket, either direct-connect or reduce via SmartConnect tree
    for m_idx in range(num_mem_ports):
        dst_pin = noc_pin_fmt.format(index=m_idx)
        sources = buckets[m_idx]
        if len(sources) == 0:
            continue
        if len(sources) == 1:
            mem_direct.append({"src_pin": sources[0], "dst_pin": dst_pin})
            continue

        # Reduction tree (≤ max_si SIs per node)
        level = 0
        current = [{"src": s} for s in sources]
        root_sc_name = None

        while len(current) > 1:
            groups = [current[i:i + max_si]
                      for i in range(0, len(current), max_si)]
            next_level: List[dict] = []

            for g_idx, group in enumerate(groups):
                sc_name = f"{base_name}_{m_idx}_{level}_{g_idx}"
                node = {
                    "name": sc_name,
                    "num_si": len(group),
                    "si": [{"slot": i, "src": g["src"]} for i, g in enumerate(group)],
                }
                mem_smart_nodes.append(node)
                # Output of this SC is single MI: M00_AXI
                next_level.append({"src": f"{sc_name}/M00_AXI"})
                root_sc_name = sc_name

            current = next_level
            level += 1

        if root_sc_name:
            mem_smart_roots.append(
                {"sc_name": root_sc_name, "dst_pin": dst_pin})

    return {
        "mem_direct": mem_direct,
        "mem_smart_nodes": mem_smart_nodes,
        "mem_smart_roots": mem_smart_roots,
    }
