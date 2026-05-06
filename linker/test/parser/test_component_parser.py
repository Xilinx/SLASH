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

"""Tests for parser.component_parser — parse_component_xml and helpers."""

import textwrap
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from slashkit.parser.component_parser import parse_component_xml, _int
from slashkit.core.port import BusType
from slashkit.emit.hls_meta import load_hls_metadata, parse_hls_args


# ---------------------------------------------------------------------------
# Minimal component.xml builder
# ---------------------------------------------------------------------------

_NS_SPIRIT = "http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009"
_NS_XILINX = "http://www.xilinx.com"

_XML_HEADER = f"""\
<?xml version="1.0" encoding="UTF-8"?>
<spirit:component
    xmlns:spirit="{_NS_SPIRIT}"
    xmlns:xilinx="{_NS_XILINX}">
  <spirit:vendor>xilinx.com</spirit:vendor>
  <spirit:library>hls</spirit:library>
  <spirit:name>{{name}}</spirit:name>
  <spirit:version>1.0</spirit:version>
  <spirit:busInterfaces>
{{bus_interfaces}}
  </spirit:busInterfaces>
</spirit:component>
"""

_AXILITE_SLAVE_NO_PROTOCOL = """\
    <spirit:busInterface>
      <spirit:name>s_axi_ctrl</spirit:name>
      <spirit:busType spirit:vendor="xilinx.com" spirit:library="interface"
                      spirit:name="aximm" spirit:version="1.0"/>
      <spirit:slave/>
      <spirit:parameters/>
    </spirit:busInterface>"""

_UNKNOWN_BUS_TYPE = """\
    <spirit:busInterface>
      <spirit:name>mystery_if</spirit:name>
      <spirit:busType spirit:vendor="acme.com" spirit:library="proprietary"
                      spirit:name="wizbus" spirit:version="1.0"/>
      <spirit:parameters/>
    </spirit:busInterface>"""

_AXIS_NO_TDATA = """\
    <spirit:busInterface>
      <spirit:name>axis_stream</spirit:name>
      <spirit:busType spirit:vendor="xilinx.com" spirit:library="interface"
                      spirit:name="axis" spirit:version="1.0"/>
      <spirit:parameters/>
    </spirit:busInterface>"""

_NAMELESS_BUSIF = """\
    <spirit:busInterface>
      <spirit:busType spirit:vendor="xilinx.com" spirit:library="interface"
                      spirit:name="axis" spirit:version="1.0"/>
      <spirit:parameters/>
    </spirit:busInterface>"""

_AXILITE_BUSIF = """\
    <spirit:busInterface>
      <spirit:name>s_axilite</spirit:name>
      <spirit:busType spirit:vendor="xilinx.com" spirit:library="interface"
                      spirit:name="aximm" spirit:version="1.0"/>
      <spirit:slave/>
      <spirit:parameters>
        <spirit:parameter>
          <spirit:name>PROTOCOL</spirit:name>
          <spirit:value>AXI4LITE</spirit:value>
        </spirit:parameter>
        <spirit:parameter>
          <spirit:name>DATA_WIDTH</spirit:name>
          <spirit:value>32</spirit:value>
        </spirit:parameter>
      </spirit:parameters>
    </spirit:busInterface>"""

_AXIS_BUSIF = """\
    <spirit:busInterface>
      <spirit:name>axis_in</spirit:name>
      <spirit:busType spirit:vendor="xilinx.com" spirit:library="interface"
                      spirit:name="axis" spirit:version="1.0"/>
      <spirit:parameters>
        <spirit:parameter>
          <spirit:name>TDATA_NUM_BYTES</spirit:name>
          <spirit:value>8</spirit:value>
        </spirit:parameter>
      </spirit:parameters>
    </spirit:busInterface>"""


@pytest.fixture
def component_xml(tmp_path: Path):
    """
    Factory fixture: writes a component.xml with given bus interfaces and
    returns its path.
    """
    def _write(name: str = "dma", bus_interfaces: str = "") -> Path:
        # Place under impl/ip/ so hls_meta path inference doesn't crash
        ip_dir = tmp_path / "sol1" / "impl" / "ip"
        ip_dir.mkdir(parents=True, exist_ok=True)
        xml_path = ip_dir / "component.xml"
        content = _XML_HEADER.format(name=name, bus_interfaces=bus_interfaces)
        xml_path.write_text(textwrap.dedent(content), encoding="utf-8")
        return xml_path

    return _write


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestParseComponentXml:
    def test_kernel_name(self, component_xml):
        path = component_xml(name="my_kernel")
        k = parse_component_xml(path)
        assert k.name == "my_kernel"

    def test_vlnv(self, component_xml):
        path = component_xml(name="dma")
        k = parse_component_xml(path)
        assert k.vlnv.startswith("xilinx.com:hls:dma:")

    def test_axilite_port_parsed(self, component_xml):
        path = component_xml(bus_interfaces=_AXILITE_BUSIF)
        k = parse_component_xml(path)
        assert "s_axilite" in k.ports
        assert k.ports["s_axilite"].ptype == BusType.AXILITE
        assert k.ports["s_axilite"].width == 32

    def test_axis_port_parsed(self, component_xml):
        path = component_xml(bus_interfaces=_AXIS_BUSIF)
        k = parse_component_xml(path)
        assert "axis_in" in k.ports
        assert k.ports["axis_in"].ptype == BusType.AXIS
        assert k.ports["axis_in"].width == 64  # 8 bytes * 8

    def test_no_bus_interfaces(self, component_xml):
        path = component_xml()
        k = parse_component_xml(path)
        assert k.ports == {}

    def test_missing_file_raises(self, tmp_path):
        with pytest.raises(Exception):
            parse_component_xml(tmp_path / "missing.xml")

    def test_int_invalid_string_returns_none(self):
        # _int() must catch ValueError and return None when the element text
        # cannot be parsed as an integer (e.g. a malformed XML value).
        el = ET.fromstring("<n>not_a_number</n>")
        assert _int(el) is None

    def test_axilite_inferred_from_slave_without_protocol_param(self, component_xml):
        # An aximm slave with no PROTOCOL parameter still resolves to AXILITE
        # via the is_slave fallback branch, distinct from the PROTOCOL="AXI4LITE" path.
        path = component_xml(bus_interfaces=_AXILITE_SLAVE_NO_PROTOCOL)
        k = parse_component_xml(path)
        assert "s_axi_ctrl" in k.ports
        assert k.ports["s_axi_ctrl"].ptype == BusType.AXILITE

    def test_unknown_bus_type_is_skipped(self, component_xml):
        # An unrecognised vendor/library/name combination causes _to_port_type()
        # to return None, and the interface is silently skipped.
        path = component_xml(bus_interfaces=_UNKNOWN_BUS_TYPE)
        k = parse_component_xml(path)
        assert k.ports == {}

    def test_axis_width_none_when_tdata_num_bytes_absent(self, component_xml):
        # _axis_width_from_params() returns None when TDATA_NUM_BYTES is not
        # present in the parameter map, so the port is created with width=None.
        path = component_xml(bus_interfaces=_AXIS_NO_TDATA)
        k = parse_component_xml(path)
        assert "axis_stream" in k.ports
        assert k.ports["axis_stream"].width is None

    def test_nameless_bus_interface_is_skipped(self, component_xml):
        # A bus interface with no <spirit:name> element is skipped entirely
        # rather than being added under an empty key.
        path = component_xml(bus_interfaces=_NAMELESS_BUSIF)
        k = parse_component_xml(path)
        assert k.ports == {}


# ---------------------------------------------------------------------------
# Tests against real HLS-generated fixtures
# ---------------------------------------------------------------------------

FIXTURES_DIR = Path(__file__).parents[1] / "fixtures"


def component_path_for_kernel(name: str) -> Path:
    return FIXTURES_DIR / name / "hls" / "impl" / "ip" / "component.xml"


class TestPassthroughFixture:
    """AXIS-only kernel: slave + master stream, clock, reset, no AXI control."""

    @classmethod
    def setup_class(cls):
        cls.k = parse_component_xml(component_path_for_kernel("passthrough"))
        cls.hls = load_hls_metadata(cls.k.hls_data_path)
        cls.args = parse_hls_args(cls.hls)

    def test_name(self):
        assert self.k.name == "passthrough"

    def test_axis_in_port(self):
        assert "axis_in" in self.k.ports
        assert self.k.ports["axis_in"].ptype == BusType.AXIS
        assert self.k.ports["axis_in"].width == 64

    def test_axis_out_port(self):
        assert "axis_out" in self.k.ports
        assert self.k.ports["axis_out"].ptype == BusType.AXIS
        assert self.k.ports["axis_out"].width == 64

    def test_clock_port(self):
        assert "ap_clk" in self.k.ports
        assert self.k.ports["ap_clk"].ptype == BusType.CLOCK

    def test_reset_port(self):
        assert "ap_rst_n" in self.k.ports
        assert self.k.ports["ap_rst_n"].ptype == BusType.RESET

    def test_no_memory_maps(self):
        assert self.k.memory_maps == []

    # hls_data.json tests
    def test_hls_data_path_resolved(self):
        assert self.k.hls_data_path is not None
        assert self.k.hls_data_path.exists()

    def test_hls_function_protocol(self):
        assert self.hls["FunctionProtocol"] == "ap_ctrl_none"

    def test_hls_clock_name(self):
        assert self.hls["ClockInfo"]["ClockName"] == "ap_clk"

    def test_hls_clock_period(self):
        assert self.hls["ClockInfo"]["ClockPeriod"] == "2"

    def test_hls_top_name(self):
        assert self.hls["Top"] == "passthrough"

    def test_hls_args_count(self):
        # passthrough has axis_in and axis_out — two args
        assert len(self.args) == 2

    def test_hls_args_sorted_by_index(self):
        indices = [a["index"] for a in self.args]
        assert indices == sorted(indices)

    def test_hls_arg_axis_in(self):
        arg = next(a for a in self.args if a["name"] == "axis_in")
        assert arg["direction"] == "in"
        assert arg["src_size"] == 64
        refs = arg["hw_refs"]
        assert len(refs) == 1
        assert refs[0]["type"] == "interface"
        assert refs[0]["interface"] == "axis_in"

    def test_hls_arg_axis_out(self):
        arg = next(a for a in self.args if a["name"] == "axis_out")
        assert arg["direction"] == "out"
        assert arg["src_size"] == 64
        refs = arg["hw_refs"]
        assert len(refs) == 1
        assert refs[0]["type"] == "interface"
        assert refs[0]["interface"] == "axis_out"


class TestDmaInFixture:
    """AXI4Lite control + AXI4Full master + AXIS master (read-from-memory, stream-out)."""

    @classmethod
    def setup_class(cls):
        cls.k = parse_component_xml(component_path_for_kernel("dma_in"))
        cls.hls = load_hls_metadata(cls.k.hls_data_path)
        cls.args = parse_hls_args(cls.hls)

    def test_name(self):
        assert self.k.name == "dma_in"

    def test_axilite_port(self):
        assert "s_axi_control" in self.k.ports
        assert self.k.ports["s_axi_control"].ptype == BusType.AXILITE
        assert self.k.ports["s_axi_control"].width == 32

    def test_axi4full_port_present(self):
        axi_full = [p for p in self.k.ports.values() if p.ptype ==
                    BusType.AXI4FULL]
        assert len(axi_full) >= 1

    def test_axis_port_present(self):
        axis = [p for p in self.k.ports.values() if p.ptype == BusType.AXIS]
        assert len(axis) >= 1

    def test_memory_maps(self):
        assert len(self.k.memory_maps) >= 1

    # hls_data.json tests
    def test_hls_data_path_resolved(self):
        assert self.k.hls_data_path is not None
        assert self.k.hls_data_path.exists()

    def test_hls_function_protocol(self):
        assert self.hls["FunctionProtocol"] == "ap_ctrl_hs"

    def test_hls_top_name(self):
        assert self.hls["Top"] == "dma_in"

    def test_hls_clock_name(self):
        assert self.hls["ClockInfo"]["ClockName"] == "ap_clk"

    def test_hls_args_count(self):
        # dma_in has: in (pointer), axis_out (stream), size (scalar) — three args
        assert len(self.args) == 3

    def test_hls_args_sorted_by_index(self):
        indices = [a["index"] for a in self.args]
        assert indices == sorted(indices)

    def test_hls_arg_in_has_axi_interface_ref(self):
        arg = next(a for a in self.args if a["name"] == "in")
        assert arg["direction"] == "in"
        iface_refs = [r for r in arg["hw_refs"] if r["type"] == "interface"]
        assert any(r["interface"] == "m_axi_gmem0" for r in iface_refs)

    def test_hls_arg_in_has_register_refs(self):
        arg = next(a for a in self.args if a["name"] == "in")
        reg_refs = [r for r in arg["hw_refs"] if r["type"] == "register"]
        assert len(reg_refs) >= 1
        assert all(r["interface"] == "s_axi_control" for r in reg_refs)

    def test_hls_arg_axis_out(self):
        arg = next(a for a in self.args if a["name"] == "axis_out")
        assert arg["direction"] == "out"
        assert arg["src_size"] == 64
        refs = arg["hw_refs"]
        assert any(r["interface"] == "axis_out" for r in refs)

    def test_hls_arg_size_is_scalar_register(self):
        arg = next(a for a in self.args if a["name"] == "size")
        assert arg["direction"] == "in"
        assert arg["src_size"] == 32
        assert all(r["type"] == "register" for r in arg["hw_refs"])


class TestDmaOutFixture:
    """AXI4Lite control + AXI4Full master + AXIS slave (stream-in, write-to-memory)."""

    @classmethod
    def setup_class(cls):
        cls.k = parse_component_xml(component_path_for_kernel("dma_out"))
        cls.hls = load_hls_metadata(cls.k.hls_data_path)
        cls.args = parse_hls_args(cls.hls)

    def test_name(self):
        assert self.k.name == "dma_out"

    def test_axilite_port(self):
        assert "s_axi_control" in self.k.ports
        assert self.k.ports["s_axi_control"].ptype == BusType.AXILITE
        assert self.k.ports["s_axi_control"].width == 32

    def test_axi4full_port_present(self):
        axi_full = [p for p in self.k.ports.values() if p.ptype ==
                    BusType.AXI4FULL]
        assert len(axi_full) >= 1

    def test_axis_port_present(self):
        axis = [p for p in self.k.ports.values() if p.ptype == BusType.AXIS]
        assert len(axis) >= 1

    def test_memory_maps(self):
        assert len(self.k.memory_maps) >= 1

    # hls_data.json tests
    def test_hls_data_path_resolved(self):
        assert self.k.hls_data_path is not None
        assert self.k.hls_data_path.exists()

    def test_hls_function_protocol(self):
        assert self.hls["FunctionProtocol"] == "ap_ctrl_hs"

    def test_hls_top_name(self):
        assert self.hls["Top"] == "dma_out"

    def test_hls_clock_name(self):
        assert self.hls["ClockInfo"]["ClockName"] == "ap_clk"

    def test_hls_args_count(self):
        # dma_out has: size (scalar), axis_in (stream), out (pointer) — three args
        assert len(self.args) == 3

    def test_hls_args_sorted_by_index(self):
        indices = [a["index"] for a in self.args]
        assert indices == sorted(indices)

    def test_hls_arg_out_has_axi_interface_ref(self):
        arg = next(a for a in self.args if a["name"] == "out")
        assert arg["direction"] == "out"
        iface_refs = [r for r in arg["hw_refs"] if r["type"] == "interface"]
        assert any(r["interface"] == "m_axi_gmem0" for r in iface_refs)

    def test_hls_arg_out_has_register_refs(self):
        arg = next(a for a in self.args if a["name"] == "out")
        reg_refs = [r for r in arg["hw_refs"] if r["type"] == "register"]
        assert len(reg_refs) >= 1
        assert all(r["interface"] == "s_axi_control" for r in reg_refs)

    def test_hls_arg_axis_in(self):
        arg = next(a for a in self.args if a["name"] == "axis_in")
        assert arg["direction"] == "in"
        assert arg["src_size"] == 64
        refs = arg["hw_refs"]
        assert any(r["interface"] == "axis_in" for r in refs)

    def test_hls_arg_size_is_scalar_register(self):
        arg = next(a for a in self.args if a["name"] == "size")
        assert arg["direction"] == "in"
        assert arg["src_size"] == 32
        assert all(r["type"] == "register" for r in arg["hw_refs"])
