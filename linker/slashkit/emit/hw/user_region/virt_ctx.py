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
from collections import defaultdict
from typing import Dict, List
from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType
from slashkit.core.bd_ports import BlockDesignPorts


def build_virt_smartconnect_context(
    instances: Dict[str, KernelInstance],
    bd: BlockDesignPorts,
    *,
    max_si: int = 16,
    base_name: str = "virt_sc_m",
    num_virt: int = 4,
) -> dict:
    """
    Like HBM, but the sinks are VIRT NoC pins:
      /noc_virt_00/S00_AXI ... /noc_virt_03/S00_AXI

    Returns:
      - virt_direct:      [{src_pin, dst_pin}]
      - virt_smart_nodes: [{name, num_si, si:[{slot, src}], ...}]
      - virt_smart_roots: [{sc_name, dst_pin}]
    """
    # Collect AXI4FULL kernel pins that target VIRT<i>
    by_virt: Dict[int, List[str]] = defaultdict(list)
    for inst in instances.values():
        mem_sp = inst.params.get("mem_sp", {})
        for k_port, tgt in mem_sp.items():
            if (tgt.get("domain") == "VIRT"
                and tgt.get("index") is not None
                    and 0 <= int(tgt["index"]) < num_virt):
                if inst.kernel.port(k_port).ptype == BusType.AXI4FULL:
                    by_virt[int(tgt["index"])].append(f"{inst.name}/{k_port}")

    virt_direct: List[dict] = []
    virt_smart_nodes: List[dict] = []
    virt_smart_roots: List[dict] = []

    for v_idx in sorted(by_virt.keys()):
        # Destination is the NoC pin (not a top-level BD port)
        dst_pin = f"/noc_virt_0{v_idx}/S00_AXI"
        sources = by_virt[v_idx]

        if len(sources) == 1:
            virt_direct.append({"src_pin": sources[0], "dst_pin": dst_pin})
            continue

        # Build reduction tree to respect max_si per SmartConnect
        level = 0
        current = [{"src": s} for s in sources]
        root_sc_name = None

        while len(current) > 1:
            groups = [current[i:i + max_si]
                      for i in range(0, len(current), max_si)]
            next_level = []
            for g_idx, group in enumerate(groups):
                sc_name = f"{base_name}_{v_idx:02d}_{level}_{g_idx}"
                virt_smart_nodes.append({
                    "name": sc_name,
                    "num_si": len(group),
                    "si": [{"slot": i, "src": g["src"]} for i, g in enumerate(group)],
                })
                next_level.append({"src": f"{sc_name}/M00_AXI"})
                root_sc_name = sc_name
            current = next_level
            level += 1

        if root_sc_name:
            virt_smart_roots.append(
                {"sc_name": root_sc_name, "dst_pin": dst_pin})

    return {
        "virt_direct": virt_direct,
        "virt_smart_nodes": virt_smart_nodes,
        "virt_smart_roots": virt_smart_roots,
    }
