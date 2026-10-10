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
from typing import Dict, Optional
from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType
from slashkit.core.bd_ports import BlockDesignPorts


def _to_int_or_none(v: Optional[object]) -> Optional[int]:
    if v is None:
        return None
    if isinstance(v, int):
        return v
    s = str(v).strip()
    if s == "":
        return None
    try:
        return int(s, 0)
    except ValueError:
        return None


def build_hbm_smartconnect_context(
    instances: Dict[str, KernelInstance],
    bd: BlockDesignPorts,
    *,
    max_si: int = 16,
    base_name: str = "hbm_sc"
) -> dict:
    """Plan a wide asynchronous CDC followed by static-clock width conversion.

    Single full-width sources use one SmartConnect for CDC and conversion,
    followed by a 256-bit register slice in the static clock domain.
    Arbitration/upsizing paths retain an explicit wide boundary so upstream
    SmartConnect propagation cannot narrow the shared CDC input.
    """
    if not 2 <= max_si <= 16:
        raise ValueError("HBM max_si must be between 2 and 16")
    by_hbm = defaultdict(list)
    supported_widths = {32, 64, 128, 256, 512, 1024}
    for inst in sorted(instances.values(), key=lambda i: i.name):
        for k_port, tgt in sorted((inst.params.get("mem_sp", {}) or {}).items()):
            if str(tgt.get("domain", "")).upper() != "HBM":
                continue
            port = inst.kernel.port(k_port)
            if port.ptype != BusType.AXI4FULL:
                continue
            idx = _to_int_or_none(tgt.get("index"))
            if idx is None or idx < 0:
                raise ValueError(
                    f"{inst.name}.{k_port}: HBM requires a valid channel index")
            if port.width not in supported_widths:
                raise ValueError(
                    f"{inst.name}.{k_port}: unsupported or unknown HBM AXI width "
                    f"{port.width!r}; supply the actual DATA_WIDTH in component.xml"
                )
            by_hbm[idx].append(
                {"src": f"{inst.name}/{k_port}", "width": port.width})

    nodes, roots, inputs, outputs = [], [], [], []
    for idx, sources in sorted(by_hbm.items()):
        dst = bd.mem("HBM", idx)
        # The existing static shell contract is 256 bits. Do not silently
        # reinterpret an incompatible future boundary as the current one.
        if dst.width not in (None, 256):
            raise ValueError(
                f"HBM{idx}: expected a 256-bit static shell boundary")
        width = max(256, *(s["width"] for s in sources))
        name = f"{base_name}_{idx:02d}"
        merged = len(sources) == 1 and sources[0]["width"] >= 256
        roots.append({
            "name": name, "idx": idx, "width": width,
            "merged": merged,
            "dst_port": dst.rtl_name or dst.name, "sources": sources,
            "dwidth_name": f"{name}_dwidth" if not merged and width != 256 else None,
        })
        current = sources
        level = 0
        # A single narrow source also needs a user-clock upsizer before CDC.
        while len(current) > 1 or (level == 0 and sources[0]["width"] < width):
            next_level = []
            for group_idx, start in enumerate(range(0, len(current), max_si)):
                group = current[start:start + max_si]
                # Do not add redundant 1x1 stages to an arbitration tree.
                if len(group) == 1 and len(current) > 1:
                    next_level.extend(group)
                    continue
                sc = f"{name}_L{level}_{group_idx}"
                nodes.append({
                    "name": sc, "num_si": len(group),
                    "si": [{"slot": i, "src": s["src"]} for i, s in enumerate(group)],
                })
                next_level.append({"src": f"{sc}/M00_AXI", "width": width})
            current = next_level
            level += 1
        inputs.append(
            {"src_pin": current[0]["src"], "dst_pin": f"{name}/S00_AXI"})
        outputs.append({
            "src_pin": (f"{name}_out/M_AXI" if merged else
                        f"{name}_dwidth/M00_AXI" if width != 256 else f"{name}_wide/M_AXI"),
            "dst_port": dst.rtl_name or dst.name,
        })
    return {
        "hbm_reduce_nodes": nodes, "hbm_root_create": roots,
        "hbm_root_in": inputs, "hbm_root_out": outputs,
    }
