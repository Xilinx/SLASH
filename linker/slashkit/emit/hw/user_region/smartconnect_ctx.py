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
from collections import OrderedDict
from typing import Dict, List
from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType


def build_axilite_smartconnect_context(
    instances: Dict[str, KernelInstance],
    *,
    si_bd_port: str = "axi_noc_0/M00_AXI",
    max_mi: int = 16,
    chain_slot: int = 15,
    base_name: str = "smartconnect",
) -> dict:
    """
    SmartConnects to fan out AXI-Lite control to all kernel AXI-Lite ports.

    Returns:
      {"smartconnects": [
         {
           "index": 0,
           "name": "smartconnect_0",
           "num_mi": 64 or N,
           "chain_slot": 63,
           "si_from": {"type":"bd_port","name":"s_axilite"} or {"type":"smartconnect","prev":"smartconnect_0"},
           "mi": [{"slot": 0, "dst_pin": "inst/AXILITE_PIN"}, ...]
         },
         ...
      ]}
    """
    # 1) Collect all AXI-Lite endpoints in a stable order
    ordered = OrderedDict((name, instances[name])
                          for name in sorted(instances.keys()))
    endpoints: List[str] = []
    for inst in ordered.values():
        for p in sorted((pp for pp in inst.kernel.ports.values() if pp.ptype == BusType.AXILITE), key=lambda x: x.name):
            endpoints.append(f"{inst.name}/{p.name}")

    N = len(endpoints)
    if N == 0:
        return {"smartconnects": []}

    # 2) Pack endpoints into one-or-more SCs
    smartconnects = []
    remaining = endpoints[:]
    idx = 0
    prev_name = None

    while remaining:
        sc_name = f"{base_name}_{idx}"
        is_last = len(remaining) <= max_mi
        if is_last:
            payload = remaining[:max_mi]
            remaining = []
            num_mi = len(payload)
        else:
            payload = remaining[: (max_mi - 1)]
            remaining = remaining[(max_mi - 1):]
            num_mi = max_mi

        sc = {
            "index": idx,
            "name": sc_name,
            "num_mi": num_mi,
            "chain_slot": chain_slot,
            "si_from": (
                {"type": "bd_port", "name": si_bd_port} if prev_name is None
                else {"type": "smartconnect", "prev": prev_name}
            ),
            "mi": [{"slot": slot, "dst_pin": dst} for slot, dst in enumerate(payload)],
        }
        smartconnects.append(sc)
        prev_name = sc_name
        idx += 1

    return {"smartconnects": smartconnects}
