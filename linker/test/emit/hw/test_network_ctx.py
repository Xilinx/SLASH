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
"""User-region stream endpoints eth_<n>.* and rdma.* map onto the NoC lanes of a port."""

from pathlib import Path

import pytest

from slashkit.core.connectivity import NetworkSpec, StreamConnect
from slashkit.core.kernel import Kernel, KernelInstance
from slashkit.core.port import BusType, Port
from slashkit.emit.hw.service_region.network_ctx import build_network_axis_context
from slashkit.emit.hw.service_region.stream_ctx import build_stream_connect_context


def _instances():
    ports = {"out": Port(name="out", ptype=BusType.AXIS, width=512),
             "in": Port(name="in", ptype=BusType.AXIS, width=512)}
    k = Kernel(name="k", component_xml_path=Path(), ports=ports)
    return {n: KernelInstance(name=n, kernel=k) for n in ("a", "b")}


def _ctx(streams, enabled, rdma=None):
    net = NetworkSpec(enabled_eth=set(enabled), rdma=rdma)
    return build_network_axis_context(
        _instances(), [StreamConnect(*s) for s in streams], net)


@pytest.mark.parametrize("rdma, lane, pin", [
    (0, "tx0", "/dcmac_axis_noc_0/S00_AXIS"),
    (0, "tx1", "/dcmac_axis_noc_1/S00_AXIS"),
    (2, "tx0", "/dcmac_axis_noc_4/S00_AXIS"),
    (2, "tx1", "/dcmac_axis_noc_5/S00_AXIS"),
])
def test_rdma_tx_lanes_are_the_rdma_ports_lanes(rdma, lane, pin):
    ctx = _ctx([("a", "out", "rdma", lane)], {rdma}, rdma)
    assert ctx["axis_to_fabric"] == [{"src_pin": "a/out", "dst_pin": pin}]
    assert ctx["streams_leftover"] == []


@pytest.mark.parametrize("rdma, lane, pin", [
    (0, "rx0", "/dcmac_axis_noc_s_0/M00_AXIS"),
    (2, "rx1", "/dcmac_axis_noc_s_5/M00_AXIS"),
])
def test_rdma_rx_lanes_are_the_rdma_ports_lanes(rdma, lane, pin):
    ctx = _ctx([("rdma", lane, "b", "in")], {rdma}, rdma)
    assert ctx["axis_from_fabric"] == [{"src_pin": pin, "dst_pin": "b/in"}]


def test_rdma_and_plain_ethernet_side_by_side():
    ctx = _ctx([("a", "out", "rdma", "tx0"), ("a", "out", "eth_2", "tx0"),
                ("eth_2", "rx0", "b", "in")], {0, 2}, 0)
    assert [e["dst_pin"] for e in ctx["axis_to_fabric"]] == [
        "/dcmac_axis_noc_0/S00_AXIS", "/dcmac_axis_noc_4/S00_AXIS"]
    assert [e["src_pin"] for e in ctx["axis_from_fabric"]] == [
        "/dcmac_axis_noc_s_4/M00_AXIS"]


def test_rdma_endpoint_without_rdma_port():
    with pytest.raises(ValueError, match="needs an rdma= port"):
        _ctx([("a", "out", "rdma", "tx0")], {0})


def test_rdma_to_rdma_rejected():
    with pytest.raises(ValueError, match="cannot be eth->eth"):
        _ctx([("rdma", "rx0", "rdma", "tx0")], {0}, 0)


def test_generic_stream_builder_rejects_rdma_endpoints():
    with pytest.raises(ValueError, match="rdma"):
        build_stream_connect_context(
            _instances(), [StreamConnect("a", "out", "rdma", "tx0")])
