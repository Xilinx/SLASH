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
from typing import List

# -----------------------------
# Data structures
# -----------------------------


@dataclass
class NetworkSpec:
    enabled_eth: set[int]


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


@dataclass
class ConnectivityConfig:
    nk: List[NKSpec] = field(default_factory=list)
    streams: List[StreamConnect] = field(default_factory=list)
    sps: List[SpMapping] = field(default_factory=list)
    clocks: List[ClockSpec] = field(default_factory=list)
    net: NetworkSpec = field(
        default_factory=lambda: NetworkSpec(enabled_eth={}))
    user_region: UserRegionSpec = field(
        default_factory=lambda: UserRegionSpec(pre_synth_tcls=[]))
    debug: DebugSpec = field(default_factory=lambda: DebugSpec(nets=[]))
