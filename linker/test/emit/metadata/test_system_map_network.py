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
"""The [network] section reaches the <ServiceLayer> element of system_map.xml."""

import xml.etree.ElementTree as ET
from types import SimpleNamespace

from slashkit.core.connectivity import ConnectivityConfig, NetworkSpec
from slashkit.emit.emu.tcl_gen import generate_emu_tcl


def _emu_config(tmp_path, enabled_eth):
    cfg = ConnectivityConfig()
    cfg.net = NetworkSpec(enabled_eth=set(enabled_eth))
    return SimpleNamespace(
        configuration=cfg,
        kernel_instances=[],
        kernels=[],
        _project_name="net",
        build_dir=tmp_path,
        clock_hz=300_000_000,
        shell_type=SimpleNamespace(value="service"),
    )


def _ethernet(tmp_path):
    root = ET.parse(tmp_path / "system_map.xml").getroot()
    eth = root.find("ServiceLayer/Ethernet")
    return eth.get("enabled"), [int(e.get("index")) for e in eth.findall("eth")]


def test_enabled_ports_are_in_the_system_map(tmp_path):
    generate_emu_tcl(_emu_config(tmp_path, {2, 0}))
    assert _ethernet(tmp_path) == ("true", [0, 2])


def test_no_ports_means_ethernet_disabled(tmp_path):
    generate_emu_tcl(_emu_config(tmp_path, set()))
    assert _ethernet(tmp_path) == ("false", [])


def test_default_network_spec_is_an_empty_set():
    assert ConnectivityConfig().net.enabled_eth == set()
    assert isinstance(ConnectivityConfig().net.enabled_eth, set)
