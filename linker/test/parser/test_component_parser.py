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
from pathlib import Path

import pytest

from parser.component_parser import parse_component_xml
from core.port import BusType


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


# ---------------------------------------------------------------------------
# Tests against real HLS-generated fixtures
# ---------------------------------------------------------------------------

FIXTURES_DIR = Path(__file__).parents[1] / "fixtures"


class TestPassthroughFixture:
    """AXIS-only kernel: slave + master stream, clock, reset, no AXI control."""

    @classmethod
    def setup_class(cls):
        cls.k = parse_component_xml(FIXTURES_DIR / "passthrough" / "component.xml")

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


class TestDmaInFixture:
    """AXI4Lite control + AXI4Full master + AXIS master (read-from-memory, stream-out)."""

    @classmethod
    def setup_class(cls):
        cls.k = parse_component_xml(FIXTURES_DIR / "dma_in" / "component.xml")

    def test_name(self):
        assert self.k.name == "dma_in"

    def test_axilite_port(self):
        assert "s_axi_control" in self.k.ports
        assert self.k.ports["s_axi_control"].ptype == BusType.AXILITE
        assert self.k.ports["s_axi_control"].width == 32

    def test_axi4full_port_present(self):
        axi_full = [p for p in self.k.ports.values() if p.ptype == BusType.AXI4FULL]
        assert len(axi_full) >= 1

    def test_axis_port_present(self):
        axis = [p for p in self.k.ports.values() if p.ptype == BusType.AXIS]
        assert len(axis) >= 1

    def test_memory_maps(self):
        assert len(self.k.memory_maps) >= 1


class TestDmaOutFixture:
    """AXI4Lite control + AXI4Full master + AXIS slave (stream-in, write-to-memory)."""

    @classmethod
    def setup_class(cls):
        cls.k = parse_component_xml(FIXTURES_DIR / "dma_out" / "component.xml")

    def test_name(self):
        assert self.k.name == "dma_out"

    def test_axilite_port(self):
        assert "s_axi_control" in self.k.ports
        assert self.k.ports["s_axi_control"].ptype == BusType.AXILITE
        assert self.k.ports["s_axi_control"].width == 32

    def test_axi4full_port_present(self):
        axi_full = [p for p in self.k.ports.values() if p.ptype == BusType.AXI4FULL]
        assert len(axi_full) >= 1

    def test_axis_port_present(self):
        axis = [p for p in self.k.ports.values() if p.ptype == BusType.AXIS]
        assert len(axis) >= 1

    def test_memory_maps(self):
        assert len(self.k.memory_maps) >= 1
