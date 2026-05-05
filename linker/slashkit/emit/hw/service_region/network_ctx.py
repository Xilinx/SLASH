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
from typing import Dict, List, Tuple
from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType

# eth_<i>.(tx0|tx1|rx0|rx1)
_ETH_EP_RE = re.compile(r"^eth_(\d+)\.(tx0|tx1|rx0|rx1)$", re.IGNORECASE)


def _map_eth_tx_pin(eth_idx: int, lane: int) -> str:
    k = eth_idx * 2 + lane
    return f"/dcmac_axis_noc_{k}/S00_AXIS"


def _map_eth_rx_pin(eth_idx: int, lane: int) -> str:
    k = eth_idx * 2 + lane
    return f"/dcmac_axis_noc_s_{k}/M00_AXIS"


def _is_eth_endpoint(s: str) -> bool:
    return _ETH_EP_RE.match(s or "") is not None


def _parse_eth_endpoint(s: str) -> Tuple[int, str]:
    m = _ETH_EP_RE.match(s)
    if not m:
        raise ValueError(
            f"Invalid eth endpoint '{s}'. Expected eth_<0..3>.(tx0|tx1|rx0|rx1)")
    return int(m.group(1)), m.group(2).lower()


def _port_norm(s): return re.sub(r"[^a-z0-9]", "", s.lower())


def _resolve_port_name(kernel, requested: str) -> str:
    # exact
    if requested in kernel.ports:
        return requested
    # case-insensitive
    low = {n.lower(): n for n in kernel.ports.keys()}
    rlow = requested.lower()
    if rlow in low:
        return low[rlow]
    # underscore/char-insensitive (remove non-alnum)
    norm_map = {_port_norm(n): n for n in kernel.ports.keys()}
    rnorm = _port_norm(requested)
    if rnorm in norm_map:
        return norm_map[rnorm]
    raise KeyError(
        f"Kernel '{kernel.name}' has no port named '{requested}'. "
        f"Available: {list(kernel.ports.keys())}"
    )

# --------------------------------------------------------------------


def build_network_axis_context(
    instances: Dict[str, KernelInstance],
    streams,
    net,                       # cfg.network with .enabled_eth
):
    """
    Returns:
      {
        "axis_to_fabric":   [{ "src_pin": "<inst>/<axis_port>", "dst_pin": "<fabric_tx_pin>"}],
        "axis_from_fabric": [{ "src_pin": "<fabric_rx_pin>",   "dst_pin": "<inst>/<axis_port>"}],
        "streams_leftover": [ non-eth streams ]
      }
    """
    to_fabric:   List[dict] = []
    from_fabric: List[dict] = []
    leftover = []

    for s in streams:
        src_is_eth = _is_eth_endpoint(f"{s.src_inst}.{s.src_port}")
        dst_is_eth = _is_eth_endpoint(f"{s.dst_inst}.{s.dst_port}")

        if not src_is_eth and not dst_is_eth:
            leftover.append(s)
            continue
        if src_is_eth and dst_is_eth:
            raise ValueError(
                f"stream_connect cannot be eth->eth: '{s.src_inst}.{s.src_port} : {s.dst_inst}.{s.dst_port}'")

        if dst_is_eth:
            # inst -> fabric TX
            if s.src_inst not in instances:
                raise KeyError(
                    f"Unknown instance '{s.src_inst}' in stream '{s.src_inst}.{s.src_port} -> {s.dst_inst}.{s.dst_port}'")
            src_inst = instances[s.src_inst]
            # robust port resolution
            src_port = _resolve_port_name(src_inst.kernel, s.src_port)
            src_p = src_inst.kernel.port(src_port)
            if src_p.ptype != BusType.AXIS:
                raise ValueError(
                    f"{s.src_inst}.{src_port} is not AXIS (got {src_p.ptype.name})")

            eth_idx, lane_name = _parse_eth_endpoint(
                f"{s.dst_inst}.{s.dst_port}")
            if eth_idx not in getattr(net, "enabled_eth", set()):
                raise ValueError(
                    f"eth_{eth_idx} is not enabled in [network] but is referenced in stream_connect")

            lane = 0 if lane_name == "tx0" else 1 if lane_name == "tx1" else None
            if lane is None:
                raise ValueError(
                    f"Only tx0/tx1 valid on fabric TX, got '{lane_name}'")

            dst_pin = _map_eth_tx_pin(eth_idx, lane)
            to_fabric.append({
                "src_pin": f"{s.src_inst}/{src_port}",
                "dst_pin": dst_pin,
            })

        else:
            # fabric RX -> inst
            if s.dst_inst not in instances:
                raise KeyError(
                    f"Unknown instance '{s.dst_inst}' in stream '{s.src_inst}.{s.src_port} -> {s.dst_inst}.{s.dst_port}'")
            dst_inst = instances[s.dst_inst]
            dst_port = _resolve_port_name(dst_inst.kernel, s.dst_port)
            dst_p = dst_inst.kernel.port(dst_port)
            if dst_p.ptype != BusType.AXIS:
                raise ValueError(
                    f"{s.dst_inst}.{dst_port} is not AXIS (got {dst_p.ptype.name})")

            eth_idx, lane_name = _parse_eth_endpoint(
                f"{s.src_inst}.{s.src_port}")
            if eth_idx not in getattr(net, "enabled_eth", set()):
                raise ValueError(
                    f"eth_{eth_idx} is not enabled in [network] but is referenced in stream_connect")

            lane = 0 if lane_name == "rx0" else 1 if lane_name == "rx1" else None
            if lane is None:
                raise ValueError(
                    f"Only rx0/rx1 valid on fabric RX, got '{lane_name}'")

            src_pin = _map_eth_rx_pin(eth_idx, lane)
            from_fabric.append({
                "src_pin": src_pin,
                "dst_pin": f"{s.dst_inst}/{dst_port}",
            })

    return {
        "axis_to_fabric": to_fabric,
        "axis_from_fabric": from_fabric,
        "streams_leftover": leftover,
    }
