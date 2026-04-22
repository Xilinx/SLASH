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
from typing import Set, Dict, Any, List
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class NetworkSpecView:
    enabled_eth: Set[int]


def build_service_layer_context(net) -> dict:
    """
    Map enabled eth_* to DCMAC enables:
      qsfp_0_n_1 -> DCMAC0 ⇔ eth_0
      qsfp_2_n_3 -> DCMAC1 ⇔ eth_2
    Dual-QSFP knobs remain 0 for now.
    """
    enabled = getattr(net, "enabled_eth", set())
    dc0 = 1 if 0 in enabled else 0
    dc1 = 1 if 2 in enabled else 0
    return {
        "needs_dcmac": (dc0 == 1 or dc1 == 1),
        "dc_enable_0": dc0,
        "dc_enable_1": dc1,
        "dual_qsfp_0": 0,
        "dual_qsfp_1": 0,
    }


def dcmac_paths(dcmac_dir: Path) -> Dict[str, Any]:
    """
    Resolve absolute paths for service-layer assets regardless of CWD.
    """
    dcmac_tcl = dcmac_dir / "tcl" / "dcmac.tcl"
    dcmac_hdl = dcmac_dir / "hdl"

    # add/remove files as needed
    hdl_files = [
        "axis_seg_to_unseg_converter.v",
        "clock_to_clock_bus.v",
        "dcmac200g_ctl_port.v",
        "serdes_clock.v",
        "syncer_reset.v",
    ]

    return {
        "dcmac_tcl": str(dcmac_tcl),
        "dcmac_hdl_dir": str(dcmac_hdl),
        "dcmac_hdl_files": [str(dcmac_hdl / f) for f in hdl_files],
    }


def build_service_axilite_ctx(net) -> Dict[str, Any]:
    """
    Build SmartConnect context for service_layer:
      - NUM_CLKS: 2 (aclk0, aclk1)
      - NUM_SI:   1 (drives from top 'S_AXILITE')
      - NUM_MI:   # of enabled DCMAC hier blocks (qsfp_0_n_1, qsfp_2_n_3)
      - MI targets: <qsfp_x>/s_axi

    We map eth_0 -> qsfp_0_n_1, eth_2 -> qsfp_2_n_3 (as per your convention).
    """
    enabled = getattr(net, "enabled_eth", set())

    qsfp_blocks: list[str] = []
    mi_targets: list[str] = []

    if 0 in enabled:
        qsfp_blocks.append("qsfp_0_n_1")
        mi_targets.append("qsfp_0_n_1/s_axi")
    if 2 in enabled:
        qsfp_blocks.append("qsfp_2_n_3")
        mi_targets.append("qsfp_2_n_3/s_axi")

    num_mi = len(mi_targets)

    return {
        # smartconnect presence
        "sl_have_xbar": num_mi > 0,

        # properties
        "sl_num_clks": 1,
        "sl_num_si": 1,
        "sl_num_mi": num_mi,

        # wiring
        # top-level service_layer AXI-Lite interface
        "sl_si_src_if": "axi_noc_0/M00_AXI",
        "sl_clk0": "service_clk",            # service_layer clock pins
        "sl_rstn": "ilreduced_logic_0/Res",

        # MI endpoints and qsfp blocks for clk/rst tie-off
        # e.g. ["qsfp_0_n_1/s_axi", "qsfp_2_n_3/s_axi"]
        "sl_mi_targets": mi_targets,
        "sl_qsfp_blocks": qsfp_blocks,     # e.g. ["qsfp_0_n_1", "qsfp_2_n_3"]

        # preferred instance names
        "sl_smartconnect_path": "smartconnect_0",
        "sl_smartconnect_name": "sl_xbar",
    }


# emit/service_layer_ctx.py


def build_service_noc_axis_ctx(net) -> Dict[str, Any]:
    """
    Build AXIS links between qsfp_* and dummy NoC endpoints inside 'service_layer'.

    Mapping (even indices only):
      - eth_0 -> qsfp_0_n_1 uses X = 0
      - eth_2 -> qsfp_2_n_3 uses X = 4

    Connections:
      Fabric -> MAC:  dummy_noc_X/M00_AXIS  -> qsfp_*/S_AXIS_0
      MAC -> Fabric:  qsfp_*/M_AXIS_0       -> dummy_noc_m_X/S00_AXIS
    """
    enabled = getattr(net, "enabled_eth", set())
    links: List[dict] = []

    if 0 in enabled:
        links.append({  # fabric -> MAC
            "src_pin": "dummy_noc_0/M00_AXIS",
            "dst_pin": "qsfp_0_n_1/S_AXIS_0",
        })
        links.append({  # MAC -> fabric
            "src_pin": "qsfp_0_n_1/M_AXIS_0",
            "dst_pin": "dummy_noc_m_0/S00_AXIS",
        })

    if 2 in enabled:
        links.append({
            "src_pin": "dummy_noc_4/M00_AXIS",
            "dst_pin": "qsfp_2_n_3/S_AXIS_0",
        })
        links.append({
            "src_pin": "qsfp_2_n_3/M_AXIS_0",
            "dst_pin": "dummy_noc_m_4/S00_AXIS",
        })

    return {"sl_axis_noc_links": links}
