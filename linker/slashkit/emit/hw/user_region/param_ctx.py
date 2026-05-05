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


def _param_name_for_busif(busif: str) -> str:
    # HLS/packager convention: C_<IFNAME>_DATA_WIDTH
    # e.g., M_AXI_GMEM0  -> C_M_AXI_GMEM0_DATA_WIDTH
    return f"C_{busif.upper()}_DATA_WIDTH"


def build_data_width_param_context(
    instances: Dict[str, KernelInstance],
    *,
    domains_of_interest=("HBM", "VIRT"),
    default_width_by_domain={"HBM": 256, "VIRT": 512}
) -> dict:
    """
    For every instance and each AXI4FULL port that is mapped (via cfg.sps/defaults)
    to a memory domain in 'domains_of_interest', emit a param set:
        set_property CONFIG.C_<BUSIF>_DATA_WIDTH {<width>} [get_bd_cells <inst>]

    Width resolution order:
      1) Use the port width parsed from component.xml if present (Port.width).
      2) Fallback to default_width_by_domain[domain].
    """
    out: List[dict] = []

    for inst in instances.values():
        # mem_sp filled earlier by apply_config_to_instances()
        mem_map = inst.params.get("mem_sp", {}) or {}
        for busif, tgt in mem_map.items():
            dom = str(tgt.get("domain", "")).upper()
            if dom not in domains_of_interest:
                continue
            # Only for AXI4FULL ports
            try:
                p = inst.kernel.port(busif)
            except KeyError:
                continue
            if p.ptype != BusType.AXI4FULL:
                continue

            # Decide width
            width = p.width if p.width else default_width_by_domain.get(dom)
            if not width:
                # If still unknown, skip silently (or raise if you prefer)
                continue

            out.append({
                "inst": inst.name,
                "param": f"CONFIG.{_param_name_for_busif(busif)}",
                "value": int(width),
            })

    # Optional de-dup if multiple entries set the same param for an inst
    dedup = {}
    for e in out:
        key = (e["inst"], e["param"])
        dedup[key] = e  # last wins
    return {"data_width_params": list(dedup.values())}
