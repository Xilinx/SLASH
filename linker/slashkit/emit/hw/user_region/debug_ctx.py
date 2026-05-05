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

import re
from typing import Dict

from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType


_AXIS_ILA_NAME = "axis_ila_debug_0"
_MAX_MONITOR_SLOTS = 16
def _port_norm(s): return re.sub(r"[^a-z0-9]", "", s.lower())


def _resolve_port_name(kernel, requested: str) -> str:
    if requested in kernel.ports:
        return requested

    low = {n.lower(): n for n in kernel.ports.keys()}
    rlow = requested.lower()
    if rlow in low:
        return low[rlow]

    norm_map = {_port_norm(n): n for n in kernel.ports.keys()}
    rnorm = _port_norm(requested)
    if rnorm in norm_map:
        return norm_map[rnorm]

    raise KeyError(
        f"Port '{requested}' not found on kernel '{kernel.name}'. "
        f"Available: {list(kernel.ports.keys())}"
    )


def _axis_ila_slot_meta(ptype: BusType) -> tuple[str, str]:
    if ptype == BusType.AXIS:
        return ("AXIS", "xilinx.com:interface:axis_rtl:1.0")
    if ptype in {BusType.AXILITE, BusType.AXI4FULL}:
        return ("AXI", "xilinx.com:interface:aximm_rtl:1.0")
    raise ValueError(
        "[debug] only AXIS/AXILITE/AXI4FULL ports are supported for axis_ila probes."
    )


def build_system_ila_debug_context(
    instances: Dict[str, KernelInstance],
    debug_spec,
) -> dict:
    """Build context for one multi-slot axis_ila core."""
    debug_nets = list(getattr(debug_spec, "nets", []) or [])
    if len(debug_nets) > _MAX_MONITOR_SLOTS:
        raise ValueError(
            f"[debug] configured {len(debug_nets)} nets, but axis_ila supports at most "
            f"{_MAX_MONITOR_SLOTS} monitor slots."
        )

    slots: list[dict] = []
    for idx, net in enumerate(debug_nets):
        inst_name = getattr(net, "inst", "")
        port_name = getattr(net, "port", "")

        if inst_name not in instances:
            raise KeyError(
                f"[debug] net refers to unknown instance '{inst_name}'.")

        inst = instances[inst_name]
        canon_port = _resolve_port_name(inst.kernel, port_name)
        slot_suffix, intf_type = _axis_ila_slot_meta(
            inst.kernel.port(canon_port).ptype)

        slots.append(
            {
                "idx": idx,
                "src_pin": f"{inst_name}/{canon_port}",
                "slot_pin": f"SLOT_{idx}_{slot_suffix}",
                "intf_type": intf_type,
            }
        )

    return {
        "debug_axis_ila_enabled": bool(slots),
        "debug_axis_ila_name": _AXIS_ILA_NAME,
        "debug_axis_ila_slots": slots,
        "debug_axis_ila_num_slots": len(slots),
    }
