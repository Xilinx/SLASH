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

# AXI Debug Hub: paired with the axis_ila cores so the Versal debug packet
# network has a hub to bind to. Exposed as an AXI-Lite slave on the control
# SmartConnect fan-out at a FIXED address that must match slash_base.tcl, so
# host debug tooling can reach it at one known location across all RM configs.
_DBG_HUB_NAME = "axi_dbg_hub_0"
_DBG_HUB_DST_PIN = f"{_DBG_HUB_NAME}/S_AXI"      # SmartConnect MI target
# address-segment bus interface
_DBG_HUB_BUSIF = "S_AXI_DBG_HUB"
_DBG_HUB_SEGMENT = "Mem0"                        # address-segment name
_DBG_HUB_RANGE = 0x0020_0000                     # 2 MiB
# FIXED - must match slash_base.tcl
_DBG_HUB_OFFSET = 0x0202_0060_0000


def _port_norm(s): return re.sub(r"[^a-z0-9]", "", s.lower())


def build_debug_hub_slaves(debug_spec) -> list[dict]:
    """! @brief AXI-Lite slave descriptor(s) for the debug hub.

    Returns a single-element list describing the debug hub's S_AXI as an extra
    AXI-Lite slave (SmartConnect MI target + fixed address window), or an empty
    list when no debug nets are configured. Consumed by the SmartConnect and
    address context builders so the hub joins the existing control fan-out.
    """
    if not (getattr(debug_spec, "nets", None) or []):
        return []
    return [{
        "dst_pin": _DBG_HUB_DST_PIN,
        "inst": _DBG_HUB_NAME,
        "busif": _DBG_HUB_BUSIF,
        "segment": _DBG_HUB_SEGMENT,
        "range": _DBG_HUB_RANGE,
        "offset": _DBG_HUB_OFFSET,     # explicit fixed offset
    }]


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
        # Debug hub is created whenever there are ILA slots (i.e. debug nets).
        "debug_hub_enabled": bool(slots),
        "debug_hub_name": _DBG_HUB_NAME,
    }
