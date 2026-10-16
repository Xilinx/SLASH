# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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

"""Tests for MEM port selection: sp=...:MEM<i> must reach vNoC port i end to end."""

import textwrap
from pathlib import Path

import pytest

from slashkit.core.kernel import Kernel
from slashkit.core.port import BusType, Port
from slashkit.emit.hw.user_region.mem_ctx import build_mem_smartconnect_context
from slashkit.emit.metadata.system_map_ctx import _assign_mem_indices
from slashkit.parser.config_parser import (
    apply_config_to_instances,
    parse_connectivity_file,
)


def _instances(tmp_path, sp_lines):
    """Parse a config with two single-port dma instances and the given sp lines."""
    cfg_path = tmp_path / "connectivity.cfg"
    cfg_path.write_text(textwrap.dedent("""\
        [connectivity]
        nk=dma:2:dma_0.dma_1
    """) + "".join(f"{ln}\n" for ln in sp_lines))
    kernel = Kernel(name="dma", component_xml_path=Path(), ports={
        "m_axi_gmem": Port(name="m_axi_gmem", ptype=BusType.AXI4FULL, width=512)})
    instances = apply_config_to_instances(parse_connectivity_file(cfg_path), [kernel])
    return {inst.name: inst for inst in instances}


def test_indexed_mem_selects_that_vnoc_port(tmp_path):
    instances = _instances(tmp_path, ["sp=dma_0.m_axi_gmem:MEM3"])
    direct = build_mem_smartconnect_context(instances)["mem_direct"]
    assert {"src_pin": "dma_0/m_axi_gmem",
            "dst_pin": "/hbm_vnoc_03/S00_AXI"} in direct
    # The system map must report the same port the hardware is wired to.
    assert _assign_mem_indices(instances)[("dma_0", "m_axi_gmem")] == 3


def test_bare_mem_is_still_assigned_round_robin(tmp_path):
    # dma_0 pins MEM5; dma_1 is left to round-robin, which starts at MEM0 because
    # explicit indices do not advance the round-robin counter.
    instances = _instances(tmp_path, ["sp=dma_0.m_axi_gmem:MEM5",
                                      "sp=dma_1.m_axi_gmem:MEM"])
    direct = build_mem_smartconnect_context(instances)["mem_direct"]
    assert {"src_pin": "dma_1/m_axi_gmem",
            "dst_pin": "/hbm_vnoc_00/S00_AXI"} in direct
    assert _assign_mem_indices(instances)[("dma_1", "m_axi_gmem")] == 0


def test_mem_index_out_of_range_is_rejected(tmp_path):
    instances = _instances(tmp_path, ["sp=dma_0.m_axi_gmem:MEM8"])
    with pytest.raises(ValueError, match="MEM index 8 out of range"):
        build_mem_smartconnect_context(instances)
