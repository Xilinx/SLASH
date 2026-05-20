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
from typing import Dict, List
from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType
from slashkit.core.bd_ports import BlockDesignPorts


def build_host_smartconnect_context(
    instances: Dict[str, KernelInstance],
    bd: BlockDesignPorts,
    *,
    max_si: int = 16,
    base_name: str = "host_sc_m",
) -> dict:
    """
    Plan connections for kernels that target HOST (QDMA slave bridge).
    Sink is the NoC pin: /qdma_slave_bridge_noc/S00_AXI

    Returns:
      - host_direct:      [{src_pin, dst_pin}]
      - host_smart_nodes: [{name, num_si, si:[{slot, src}], ...}]
      - host_smart_roots: [{sc_name, dst_pin}]
    """
    # Gather AXI4FULL sources that map to HOST
    host_sources: List[str] = []
    for inst in instances.values():
        mem_sp = inst.params.get("mem_sp", {})
        for k_port, tgt in mem_sp.items():
            if (str(tgt.get("domain", "")).upper() == "HOST"
                    and inst.kernel.port(k_port).ptype == BusType.AXI4FULL):
                host_sources.append(f"{inst.name}/{k_port}")

    host_direct: List[dict] = []
    host_smart_nodes: List[dict] = []
    host_smart_roots: List[dict] = []

    dst_pin = "/qdma_slave_bridge_noc/S00_AXI"

    if len(host_sources) == 1:
        host_direct.append({"src_pin": host_sources[0], "dst_pin": dst_pin})
    elif len(host_sources) > 1:
        # reduction tree to respect max_si
        level = 0
        current = [{"src": s} for s in host_sources]
        root_sc_name = None
        while len(current) > 1:
            groups = [current[i:i + max_si]
                      for i in range(0, len(current), max_si)]
            next_level = []
            for g_idx, group in enumerate(groups):
                sc_name = f"{base_name}_{level}_{g_idx}"
                host_smart_nodes.append({
                    "name": sc_name,
                    "num_si": len(group),
                    "si": [{"slot": i, "src": g["src"]} for i, g in enumerate(group)],
                })
                next_level.append({"src": f"{sc_name}/M00_AXI"})
                root_sc_name = sc_name
            current = next_level
            level += 1
        if root_sc_name:
            host_smart_roots.append(
                {"sc_name": root_sc_name, "dst_pin": dst_pin})

    return {
        "host_direct": host_direct,
        "host_smart_nodes": host_smart_nodes,
        "host_smart_roots": host_smart_roots,
    }
