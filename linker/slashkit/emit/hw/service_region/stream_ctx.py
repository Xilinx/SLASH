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
from typing import Dict, List
from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType

_ETH_EP_RE = re.compile(r"^eth_(\d+)\.(tx0|tx1|rx0|rx1)$", re.IGNORECASE)
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
        f"Kernel '{kernel.name}' has no port named '{requested}'. "
        f"Available: {list(kernel.ports.keys())}"
    )


def build_stream_connect_context(
    instances: Dict[str, KernelInstance],
    streams: List[object],
) -> dict:
    """
    Convert config 'stream_connect=src_inst.src_port:dst_inst.dst_port'
    into {src_pin, dst_pin}, validating AXIS.
    NOTE: pass ONLY non-eth streams here (use build_network_axis_context first).
    """
    out: List[dict] = []

    for s in streams:
        # prevent accidental eth_* usage here
        if _ETH_EP_RE.match(f"{s.src_inst}.{s.src_port}") or _ETH_EP_RE.match(f"{s.dst_inst}.{s.dst_port}"):
            raise ValueError(
                "eth_* endpoint seen in generic builder. "
                "Call build_network_axis_context() first and pass only its 'streams_leftover' here."
            )

        if s.src_inst not in instances:
            raise KeyError(f"stream_connect: unknown instance '{s.src_inst}'")
        if s.dst_inst not in instances:
            raise KeyError(f"stream_connect: unknown instance '{s.dst_inst}'")

        src_inst = instances[s.src_inst]
        dst_inst = instances[s.dst_inst]

        src_port = _resolve_port_name(src_inst.kernel, s.src_port)
        dst_port = _resolve_port_name(dst_inst.kernel, s.dst_port)

        src_p = src_inst.kernel.port(src_port)
        dst_p = dst_inst.kernel.port(dst_port)

        if src_p.ptype != BusType.AXIS:
            raise ValueError(
                f"stream_connect: {s.src_inst}.{src_port} is not AXIS (got {src_p.ptype.name})")
        if dst_p.ptype != BusType.AXIS:
            raise ValueError(
                f"stream_connect: {s.dst_inst}.{dst_port} is not AXIS (got {dst_p.ptype.name})")

        out.append({
            "src_pin": f"{s.src_inst}/{src_port}",
            "dst_pin": f"{s.dst_inst}/{dst_port}",
        })

    return {"axis_streams": out}
