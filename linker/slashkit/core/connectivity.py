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
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# -----------------------------
# Data structures
# -----------------------------

# The NoC lanes of each RDMA core, by the cores in the design: the linker
# picks them, the configuration does not. The static shell fixes the NoC sites
# of every lane.
#  - eth_0 alone: 4 and 6, 2 x 102 Gb/s. eth_0's own lanes, 0 and 1, share
#    NoC links and carry about 130 Gb/s together.
#  - eth_2: 2 and 6, whose sites lie closest to DCMAC1 on the left of the
#    service layer; with its own lanes the core misses timing.
#  - both: eth_0 takes its own lanes, next to DCMAC0 on the right.
# A lane of a port enabled for Ethernet stays that port's, and the core then
# takes its own two lanes.
RDMA_LANES: Dict[Tuple[int, ...], Dict[int, Tuple[int, int]]] = {
    (0,): {0: (4, 6)},
    (2,): {2: (2, 6)},
    (0, 2): {0: (0, 1), 2: (2, 6)},
}


@dataclass
class NetworkSpec:
    enabled_eth: set[int]
    # Ethernet ports (eth_<n> indices) that carry an RDMA core, ascending;
    # empty: no RDMA
    rdma: Tuple[int, ...] = ()
    # READ requests the stack keeps in flight per queue pair (1..32), for
    # every core; None: the stack's default. A peer NIC takes only as many as
    # its responder resources (max_dest_rd_atomic, 16 on many NICs)
    rdma_read_window: Optional[int] = None

    def rdma_lane_pair(self, eth: int) -> Tuple[int, int]:
        """The NoC lanes (0..7, lane k belongs to eth_<k // 2>) of
        rdma_<eth>.tx / rdma_<eth>.rx."""
        own = (2 * eth, 2 * eth + 1)
        lanes = RDMA_LANES.get(tuple(self.rdma), {}).get(eth, own)
        # a lane of a port enabled for Ethernet stays that port's
        for lane in lanes:
            port = lane // 2
            if port not in self.rdma and port in self.enabled_eth:
                return own
        return lanes


@dataclass
class UserRegionSpec:
    pre_synth_tcls: list[str]


@dataclass(frozen=True)
class DebugNetSpec:
    inst: str
    port: str


@dataclass
class DebugSpec:
    nets: list[DebugNetSpec]


@dataclass(frozen=True)
class NKSpec:
    kernel_type: str
    count: int
    instance_names: List[str]


@dataclass(frozen=True)
class StreamConnect:
    src_inst: str
    src_port: str
    dst_inst: str
    dst_port: str


@dataclass(frozen=True)
class MemoryTarget:
    domain: str
    index: int


@dataclass(frozen=True)
class SpMapping:
    inst: str
    port: str
    target: MemoryTarget


@dataclass(frozen=True)
class ClockSpec:
    inst: str
    freq_hz: int


@dataclass(frozen=True)
class SlrSpec:
    inst: str
    slr: int


@dataclass
class ConnectivityConfig:
    nk: List[NKSpec] = field(default_factory=list)
    streams: List[StreamConnect] = field(default_factory=list)
    sps: List[SpMapping] = field(default_factory=list)
    clocks: List[ClockSpec] = field(default_factory=list)
    net: NetworkSpec = field(
        default_factory=lambda: NetworkSpec(enabled_eth=set()))
    user_region: UserRegionSpec = field(
        default_factory=lambda: UserRegionSpec(pre_synth_tcls=[])
    )
    debug: DebugSpec = field(default_factory=lambda: DebugSpec(nets=[]))
    floorplan: List[SlrSpec] = field(default_factory=list)
    shell: str = "compute"
