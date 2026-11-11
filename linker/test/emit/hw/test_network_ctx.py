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


def _instances(width=1024):
    ports = {"out": Port(name="out", ptype=BusType.AXIS, width=width),
             "in": Port(name="in", ptype=BusType.AXIS, width=width)}
    k = Kernel(name="k", component_xml_path=Path(), ports=ports)
    return {n: KernelInstance(name=n, kernel=k) for n in ("a", "b")}


def _ctx(streams, enabled, rdma=None, width=1024):
    """rdma: a port or a tuple of ports."""
    ports = () if rdma is None else (rdma,) if isinstance(rdma, int) else tuple(rdma)
    net = NetworkSpec(enabled_eth=set(enabled), rdma=ports)
    return build_network_axis_context(
        _instances(width), [StreamConnect(*s) for s in streams], net)


@pytest.mark.parametrize("rdma, lanes", [
    (0, ["/dcmac_axis_noc_4/S00_AXIS", "/dcmac_axis_noc_6/S00_AXIS"]),
    (2, ["/dcmac_axis_noc_2/S00_AXIS", "/dcmac_axis_noc_6/S00_AXIS"]),
])
def test_rdma_tx_goes_over_the_lanes_of_the_core(rdma, lanes):
    ctx = _ctx([("a", "out", "rdma", "tx")], {rdma}, rdma)
    assert ctx["axis_to_fabric"] == [
        {"src_pin": "a/out", "dst_pin": f"rdma_{rdma}_stream_tx/s_axis"},
        {"src_pin": f"rdma_{rdma}_stream_tx/m_axis0", "dst_pin": lanes[0]},
        {"src_pin": f"rdma_{rdma}_stream_tx/m_axis1", "dst_pin": lanes[1]},
    ]
    assert ctx["rdma_stream_cells"] == [{"name": f"rdma_{rdma}_stream_tx",
                                         "vlnv": "slash:rdma:rdma_stream_tx:1.0"}]
    assert ctx["streams_leftover"] == []


@pytest.mark.parametrize("rdma, lanes", [
    (0, ["/dcmac_axis_noc_s_4/M00_AXIS", "/dcmac_axis_noc_s_6/M00_AXIS"]),
    (2, ["/dcmac_axis_noc_s_2/M00_AXIS", "/dcmac_axis_noc_s_6/M00_AXIS"]),
])
def test_rdma_rx_comes_over_the_lanes_of_the_core(rdma, lanes):
    ctx = _ctx([("rdma", "rx", "b", "in")], {rdma}, rdma)
    assert ctx["axis_from_fabric"] == [
        {"src_pin": lanes[0], "dst_pin": f"rdma_{rdma}_stream_rx/s_axis0"},
        {"src_pin": lanes[1], "dst_pin": f"rdma_{rdma}_stream_rx/s_axis1"},
        {"src_pin": f"rdma_{rdma}_stream_rx/m_axis", "dst_pin": "b/in"},
    ]
    assert ctx["rdma_stream_cells"] == [{"name": f"rdma_{rdma}_stream_rx",
                                         "vlnv": "slash:rdma:rdma_stream_rx:1.0"}]


def test_rdma_stream_ips_are_shipped():
    import xml.etree.ElementTree as ET
    from slashkit.emit.hw.service_region.network_ctx import RDMA_STREAM_IP
    ns = {"spirit": "http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009"}
    for name in ("rdma_stream_tx", "rdma_stream_rx"):
        root = ET.parse(RDMA_STREAM_IP / name / "component.xml").getroot()
        vlnv = ":".join(root.find(f"spirit:{k}", ns).text
                        for k in ("vendor", "library", "name", "version"))
        assert vlnv == f"slash:rdma:{name}:1.0"
        # the packaged source is the one in hdl/user
        assert ((RDMA_STREAM_IP / name / "src" / "rdma_stream_user.v").read_text()
                == (RDMA_STREAM_IP.parent / "hdl" / "user" / "rdma_stream_user.v").read_text())


def test_rdma_and_plain_ethernet_side_by_side():
    ctx = _ctx([("a", "out", "rdma", "tx"), ("a", "out", "eth_2", "tx0"),
                ("eth_2", "rx0", "b", "in")], {0, 2}, 0)
    assert [e["dst_pin"] for e in ctx["axis_to_fabric"]] == [
        "rdma_0_stream_tx/s_axis", "/dcmac_axis_noc_0/S00_AXIS", "/dcmac_axis_noc_1/S00_AXIS",
        "/dcmac_axis_noc_4/S00_AXIS"]
    assert [e["src_pin"] for e in ctx["axis_from_fabric"]] == [
        "/dcmac_axis_noc_s_4/M00_AXIS"]


def test_no_rdma_stream_no_cells():
    ctx = _ctx([("a", "out", "eth_0", "tx0")], {0})
    assert ctx["rdma_stream_cells"] == []


@pytest.mark.parametrize("lane", ["tx0", "tx1", "rx0", "rx1", "foo"])
def test_rdma_lanes_are_not_endpoints(lane):
    with pytest.raises(ValueError, match="one stream each way: rdma_<n>.tx and rdma_<n>.rx"):
        _ctx([("a", "out", "rdma", lane)], {0}, 0)


def test_rdma_tx_is_one_stream():
    with pytest.raises(ValueError, match="rdma_0.tx is one stream"):
        _ctx([("a", "out", "rdma", "tx"), ("b", "out", "rdma", "tx")], {0}, 0)


def test_rdma_rx_is_one_stream():
    with pytest.raises(ValueError, match="rdma_0.rx is one stream"):
        _ctx([("rdma", "rx", "a", "in"), ("rdma", "rx", "b", "in")], {0}, 0)


def test_rdma_directions():
    with pytest.raises(ValueError, match=r"Only rdma\[_<n>\].tx takes"):
        _ctx([("a", "out", "rdma", "rx")], {0}, 0)
    with pytest.raises(ValueError, match=r"Only rdma\[_<n>\].rx gives"):
        _ctx([("rdma", "tx", "b", "in")], {0}, 0)


def test_rdma_stream_is_1024_bits():
    with pytest.raises(ValueError, match="a.out is 512-bit"):
        _ctx([("a", "out", "rdma", "tx")], {0}, 0, width=512)


def test_rdma_endpoint_without_rdma_port():
    with pytest.raises(ValueError, match="needs an rdma= port"):
        _ctx([("a", "out", "rdma", "tx")], {0})


def test_rdma_to_rdma_rejected():
    with pytest.raises(ValueError, match="cannot be eth->eth"):
        _ctx([("rdma", "rx", "rdma", "tx")], {0}, 0)


def test_generic_stream_builder_rejects_rdma_endpoints():
    with pytest.raises(ValueError, match="rdma"):
        build_stream_connect_context(
            _instances(), [StreamConnect("a", "out", "rdma", "tx")])


# ---------------------------------------------------------------- two cores

def test_two_cores_each_on_their_lanes():
    ctx = _ctx([("a", "out", "rdma_0", "tx"), ("rdma_0", "rx", "b", "in"),
                ("b", "out", "rdma_2", "tx"), ("rdma_2", "rx", "a", "in")],
               {0, 2}, (0, 2))
    assert [c["name"] for c in ctx["rdma_stream_cells"]] == [
        "rdma_0_stream_tx", "rdma_0_stream_rx", "rdma_2_stream_tx", "rdma_2_stream_rx"]
    to = {(e["src_pin"], e["dst_pin"]) for e in ctx["axis_to_fabric"]}
    assert ("rdma_0_stream_tx/m_axis1", "/dcmac_axis_noc_1/S00_AXIS") in to
    assert ("rdma_2_stream_tx/m_axis0", "/dcmac_axis_noc_2/S00_AXIS") in to
    assert ("b/out", "rdma_2_stream_tx/s_axis") in to
    frm = {(e["src_pin"], e["dst_pin"]) for e in ctx["axis_from_fabric"]}
    assert ("/dcmac_axis_noc_s_6/M00_AXIS", "rdma_2_stream_rx/s_axis1") in frm
    assert ("rdma_0_stream_rx/m_axis", "b/in") in frm


def test_named_core_with_one_core():
    ctx = _ctx([("a", "out", "rdma_2", "tx")], {2}, 2)
    assert ctx["rdma_stream_cells"][0]["name"] == "rdma_2_stream_tx"


def test_bare_rdma_endpoint_with_two_cores_rejected():
    with pytest.raises(ValueError, match="more than one RDMA core"):
        _ctx([("a", "out", "rdma", "tx")], {0, 2}, (0, 2))


def test_endpoint_of_a_port_without_core_rejected():
    with pytest.raises(ValueError, match="no RDMA core on eth_2"):
        _ctx([("a", "out", "rdma_2", "tx")], {0, 2}, 0)


def test_each_core_takes_one_stream_each_way():
    with pytest.raises(ValueError, match="rdma_2.tx is one stream"):
        _ctx([("a", "out", "rdma_2", "tx"), ("b", "out", "rdma_2", "tx")],
             {0, 2}, (0, 2))
