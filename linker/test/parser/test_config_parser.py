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

"""Tests for parser.config_parser — helper functions and file parser."""

import textwrap
from pathlib import Path

import pytest

from slashkit.parser.config_parser import (
    _parse_target,
    _parse_nk_value,
    _parse_stream_connect_value,
    _parse_sp_value,
    _parse_debug_net_value,
    _resolve_port_name_for_kernel,
    parse_connectivity_file,
    apply_config_to_instances,
)
from slashkit.core.kernel import Kernel
from slashkit.core.port import BusType, Port
from slashkit.core.connectivity import (
    ConnectivityConfig,
    NKSpec,
    ClockSpec,
    SpMapping,
    MemoryTarget,
)


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

def _make_kernel(name, port_specs):
    """Build a Kernel from {port_name: (BusType, width)} without any XML."""
    ports = {pname: Port(name=pname, ptype=ptype, width=width)
             for pname, (ptype, width) in port_specs.items()}
    return Kernel(name=name, component_xml_path=Path(), ports=ports)


@pytest.fixture
def cfg_file(tmp_path):
    """Factory: write cfg text to a temp file and return its path."""
    def _write(content: str) -> Path:
        p = tmp_path / "connectivity.cfg"
        p.write_text(textwrap.dedent(content))
        return p
    return _write


# ---------------------------------------------------------------------------
# Helper function tests
# ---------------------------------------------------------------------------

class TestHelperFunctions:

    # --- _parse_target ---

    def test_parse_target_hbm(self):
        t = _parse_target("HBM0")
        assert t.domain == "HBM"
        assert t.index == 0

    def test_parse_target_ddr(self):
        t = _parse_target("DDR3")
        assert t.domain == "DDR"
        assert t.index == 3

    def test_parse_target_host(self):
        # HOST has no numeric index; the parser stores "" for it.
        t = _parse_target("HOST")
        assert t.domain == "HOST"
        assert t.index == ""

    def test_parse_target_mem(self):
        # MEM likewise carries no index.
        t = _parse_target("MEM")
        assert t.domain == "MEM"
        assert t.index == ""

    def test_parse_target_invalid_domain(self):
        # An unrecognised domain name must be rejected.
        with pytest.raises(ValueError, match="Unsupported memory domain"):
            _parse_target("FLASH0")

    def test_parse_target_bad_format(self):
        # The regex requires a single alphabetic word optionally followed by
        # digits; extra tokens fail to match.
        with pytest.raises(ValueError, match="Invalid memory target"):
            _parse_target("HBM 0 extra")

    # --- _parse_nk_value ---

    def test_parse_nk_explicit_names(self):
        spec = _parse_nk_value("dma:2:dma_0 dma_1")
        assert spec.kernel_type == "dma"
        assert spec.count == 2
        assert spec.instance_names == ["dma_0", "dma_1"]

    def test_parse_nk_auto_names(self):
        # When no names are provided they are auto-generated as <type>_0..<type>_N-1.
        spec = _parse_nk_value("foo:3")
        assert spec.kernel_type == "foo"
        assert spec.count == 3
        assert spec.instance_names == ["foo_0", "foo_1", "foo_2"]

    def test_parse_nk_partial_names_padded(self):
        # Fewer names than count triggers auto-fill for the remainder.
        spec = _parse_nk_value("foo:3:foo_0")
        assert spec.count == 3
        assert spec.instance_names == ["foo_0", "foo_1", "foo_2"]

    def test_parse_nk_bad_format(self):
        # A value without a colon does not match the expected pattern.
        with pytest.raises(ValueError, match="Invalid nk entry"):
            _parse_nk_value("dma")

    # --- _parse_stream_connect_value ---

    def test_parse_stream_connect(self):
        sc = _parse_stream_connect_value("src_inst.out_port:dst_inst.in_port")
        assert sc.src_inst == "src_inst"
        assert sc.src_port == "out_port"
        assert sc.dst_inst == "dst_inst"
        assert sc.dst_port == "in_port"

    def test_parse_stream_connect_bad(self):
        # Missing the colon separator between src and dst is invalid.
        with pytest.raises(ValueError, match="Invalid stream_connect"):
            _parse_stream_connect_value("src_inst.out_port")

    # --- _parse_sp_value ---

    def test_parse_sp_hbm(self):
        sp = _parse_sp_value("dma_0.m_axi_gmem:HBM2")
        assert sp.inst == "dma_0"
        assert sp.port == "m_axi_gmem"
        assert sp.target.domain == "HBM"
        assert sp.target.index == 2

    def test_parse_sp_bad_format(self):
        # No colon → cannot split into instance.port and target.
        with pytest.raises(ValueError, match="Invalid sp"):
            _parse_sp_value("dma_0.m_axi_gmem")

    # --- _parse_debug_net_value ---

    def test_parse_debug_net(self):
        dn = _parse_debug_net_value("my_inst.my_port")
        assert dn.inst == "my_inst"
        assert dn.port == "my_port"

    def test_parse_debug_net_bad(self):
        # A value with no dot does not match the expected pattern.
        with pytest.raises(ValueError, match="Invalid debug net"):
            _parse_debug_net_value("no_dot")

    # --- _resolve_port_name_for_kernel ---

    def test_resolve_port_exact_match(self):
        # When the requested name matches a port exactly it is returned unchanged.
        k = Kernel(name="k", component_xml_path=Path(),
                   ports={"m_axi_gmem": Port(name="m_axi_gmem", ptype=BusType.AXI4FULL, width=512)})
        assert _resolve_port_name_for_kernel(k, "m_axi_gmem") == "m_axi_gmem"

    def test_resolve_port_case_insensitive(self):
        # A differently-cased request resolves to the canonical port name from the kernel.
        k = Kernel(name="k", component_xml_path=Path(),
                   ports={"M_AXI_GMEM": Port(name="M_AXI_GMEM", ptype=BusType.AXI4FULL, width=512)})
        assert _resolve_port_name_for_kernel(k, "m_axi_gmem") == "M_AXI_GMEM"

    def test_resolve_port_unknown_raises(self):
        # A name that does not appear in the kernel's ports raises KeyError.
        k = Kernel(name="k", component_xml_path=Path(),
                   ports={"axis_in": Port(name="axis_in", ptype=BusType.AXIS, width=64)})
        with pytest.raises(KeyError, match="not found"):
            _resolve_port_name_for_kernel(k, "nonexistent")


# ---------------------------------------------------------------------------
# parse_connectivity_file tests
# ---------------------------------------------------------------------------

class TestParseConnectivityFile:

    def test_empty_file(self, cfg_file):
        cfg = parse_connectivity_file(cfg_file(""))
        assert cfg.nk == []
        assert cfg.streams == []
        assert cfg.sps == []
        assert cfg.clocks == []
        assert cfg.net.enabled_eth == set()

    def test_comment_lines_ignored(self, cfg_file):
        # Lines starting with # or ; are treated as comments and produce no output.
        cfg = parse_connectivity_file(cfg_file("""\
            # this is a comment
            ; so is this
        """))
        assert cfg.nk == []

    def test_nk_parsed(self, cfg_file):
        cfg_inside = parse_connectivity_file(cfg_file("""\
            [connectivity]
            nk=dma:1:dma_0
        """))
        assert len(cfg_inside.nk) == 1
        assert cfg_inside.nk[0].kernel_type == "dma"
        assert cfg_inside.nk[0].instance_names == ["dma_0"]

    def test_stream_connect_parsed(self, cfg_file):
        cfg_inside = parse_connectivity_file(cfg_file("""\
            [connectivity]
            stream_connect=a.out:b.in
        """))
        assert len(cfg_inside.streams) == 1
        sc = cfg_inside.streams[0]
        assert sc.src_inst == "a" and sc.src_port == "out"
        assert sc.dst_inst == "b" and sc.dst_port == "in"

    def test_sp_parsed(self, cfg_file):
        cfg_inside = parse_connectivity_file(cfg_file("""\
            [connectivity]
            sp=dma_0.m_axi_gmem:HBM0
        """))
        assert len(cfg_inside.sps) == 1
        sp = cfg_inside.sps[0]
        assert sp.inst == "dma_0" and sp.port == "m_axi_gmem"
        assert sp.target.domain == "HBM" and sp.target.index == 0

    def test_clock_section_committed(self, cfg_file):
        # A complete [clock] block followed by another section is committed.
        cfg = parse_connectivity_file(cfg_file("""\
            [clock]
            krnl=dma_0
            freqhz=300000000
            [connectivity]
        """))
        assert len(cfg.clocks) == 1
        assert cfg.clocks[0].inst == "dma_0"
        assert cfg.clocks[0].freq_hz == 300_000_000

    def test_multiple_clock_sections(self, cfg_file):
        # Each [clock] block produces an independent ClockSpec.
        cfg = parse_connectivity_file(cfg_file("""\
            [clock]
            krnl=dma_0
            freqhz=300000000
            [clock]
            krnl=pass_0
            freqhz=500000000
        """))
        assert len(cfg.clocks) == 2
        insts = {c.inst for c in cfg.clocks}
        assert insts == {"dma_0", "pass_0"}

    def test_clock_missing_equals_raises(self, cfg_file):
        # A line inside [clock] that is not a key=value pair raises ValueError.
        with pytest.raises(ValueError, match="Invalid line in \\[clock\\]"):
            parse_connectivity_file(cfg_file("""\
                [clock]
                not_an_assignment
            """))

    def test_clock_missing_freqhz_raises(self, cfg_file):
        # A [clock] block with krnl but no freqhz is incomplete and must raise.
        with pytest.raises(ValueError, match="Incomplete"):
            parse_connectivity_file(cfg_file("""\
                [clock]
                krnl=dma_0
                [connectivity]
            """))

    def test_clock_invalid_freqhz_raises(self, cfg_file):
        # A non-integer freqhz value must raise ValueError.
        with pytest.raises(ValueError, match="Invalid freqhz"):
            parse_connectivity_file(cfg_file("""\
                [clock]
                krnl=dma_0
                freqhz=not_a_number
                [connectivity]
            """))

    def test_clock_committed_at_eof(self, cfg_file):
        # A [clock] block that is the last section in the file (no trailing
        # section header) must still be committed by the post-loop call.
        cfg = parse_connectivity_file(cfg_file("""\
            [clock]
            krnl=dma_0
            freqhz=250000000
        """))
        assert len(cfg.clocks) == 1
        assert cfg.clocks[0].inst == "dma_0"

    def test_network_section_eth_enabled(self, cfg_file):
        cfg = parse_connectivity_file(cfg_file("""\
            [network]
            eth_0=1
        """))
        assert 0 in cfg.net.enabled_eth

    def test_network_section_eth_disabled(self, cfg_file):
        # A zero value means the interface is not enabled.
        cfg = parse_connectivity_file(cfg_file("""\
            [network]
            eth_0=0
        """))
        assert cfg.net.enabled_eth == set()

    def test_network_invalid_value_treated_as_zero(self, cfg_file):
        # A non-integer eth value is silently treated as 0 (not enabled).
        cfg = parse_connectivity_file(cfg_file("""\
            [network]
            eth_0=bad
        """))
        assert cfg.net.enabled_eth == set()

    def test_network_unknown_key_ignored(self, cfg_file):
        # Unrecognised keys in [network] are silently skipped.
        cfg = parse_connectivity_file(cfg_file("""\
            [network]
            foo=1
        """))
        assert cfg.net.enabled_eth == set()

    def test_network_missing_equals_raises(self, cfg_file):
        with pytest.raises(ValueError, match="Invalid line in \\[network\\]"):
            parse_connectivity_file(cfg_file("""\
                [network]
                eth_0
            """))

    def test_user_region_pre_synth_relative_path(self, cfg_file, tmp_path):
        # A relative pre_synth path is resolved relative to the cfg file's parent.
        cfg = parse_connectivity_file(cfg_file("""\
            [user_region]
            pre_synth=setup.tcl
        """))
        expected = str((tmp_path / "setup.tcl").resolve())
        assert cfg.user_region.pre_synth_tcls == [expected]

    def test_user_region_pre_synth_absolute_path(self, cfg_file):
        cfg = parse_connectivity_file(cfg_file("""\
            [user_region]
            pre_synth=/absolute/path/to/setup.tcl
        """))
        assert cfg.user_region.pre_synth_tcls == [
            "/absolute/path/to/setup.tcl"]

    def test_user_region_empty_pre_synth_raises(self, cfg_file):
        with pytest.raises(ValueError, match="empty pre_synth"):
            parse_connectivity_file(cfg_file("""\
                [user_region]
                pre_synth=
            """))

    def test_user_region_unknown_key_ignored(self, cfg_file):
        # Unrecognised keys in [user_region] are silently skipped.
        cfg = parse_connectivity_file(cfg_file("""\
            [user_region]
            unknown_key=value
        """))
        assert cfg.user_region.pre_synth_tcls == []

    def test_user_region_missing_equals_raises(self, cfg_file):
        with pytest.raises(ValueError, match="Invalid line in \\[user_region\\]"):
            parse_connectivity_file(cfg_file("""\
                [user_region]
                not_an_assignment
            """))

    def test_debug_net_parsed(self, cfg_file):
        cfg = parse_connectivity_file(cfg_file("""\
            [debug]
            net=my_inst.my_port
        """))
        assert len(cfg.debug.nets) == 1
        assert cfg.debug.nets[0].inst == "my_inst"
        assert cfg.debug.nets[0].port == "my_port"

    def test_debug_unknown_key_raises(self, cfg_file):
        with pytest.raises(ValueError, match="Invalid key"):
            parse_connectivity_file(cfg_file("""\
                [debug]
                foo=bar
            """))

    def test_debug_missing_equals_raises(self, cfg_file):
        with pytest.raises(ValueError, match="Invalid line in \\[debug\\]"):
            parse_connectivity_file(cfg_file("""\
                [debug]
                no_equals_here
            """))

    def test_unknown_section_lines_silently_ignored(self, cfg_file):
        # Lines under an unrecognised section header produce no error.
        cfg = parse_connectivity_file(cfg_file("""\
            [totally_unknown]
            some_key=some_value
        """))
        assert cfg.nk == []

    def test_missing_file_raises(self, tmp_path):
        with pytest.raises(Exception):
            parse_connectivity_file(tmp_path / "nonexistent.cfg")


# ---------------------------------------------------------------------------
# apply_config_to_instances tests
# ---------------------------------------------------------------------------

class TestApplyConfigToInstances:

    # --- instantiation from nk ---

    def test_instantiates_kernels_from_nk(self):
        k = _make_kernel("dma", {"m_axi_gmem": (BusType.AXI4FULL, 512)})
        cfg = ConnectivityConfig(nk=[NKSpec("dma", 1, ["dma_0"])])
        instances = apply_config_to_instances(cfg, [k])
        assert len(instances) == 1
        assert instances[0].name == "dma_0"
        assert instances[0].kernel is k

    def test_multiple_nk_entries(self):
        dma = _make_kernel("dma", {"m_axi_gmem": (BusType.AXI4FULL, 512)})
        pt = _make_kernel("passthrough", {"axis_in": (BusType.AXIS, 64)})
        cfg = ConnectivityConfig(nk=[
            NKSpec("dma", 1, ["dma_0"]),
            NKSpec("passthrough", 2, ["pt_0", "pt_1"]),
        ])
        instances = apply_config_to_instances(cfg, [dma, pt])
        names = {i.name for i in instances}
        assert names == {"dma_0", "pt_0", "pt_1"}

    def test_unknown_kernel_type_raises(self):
        cfg = ConnectivityConfig(
            nk=[NKSpec("nonexistent", 1, ["nonexistent_0"])])
        with pytest.raises(KeyError, match="nonexistent"):
            apply_config_to_instances(cfg, [])

    def test_duplicate_instance_name_raises(self):
        k = _make_kernel("dma", {"m_axi_gmem": (BusType.AXI4FULL, 512)})
        cfg = ConnectivityConfig(nk=[
            NKSpec("dma", 1, ["dma_0"]),
            NKSpec("dma", 1, ["dma_0"]),  # duplicate
        ])
        with pytest.raises(ValueError, match="Duplicate"):
            apply_config_to_instances(cfg, [k])

    # --- clock attachment ---

    def test_clock_attached_to_instance(self):
        k = _make_kernel("dma", {"m_axi_gmem": (BusType.AXI4FULL, 512)})
        cfg = ConnectivityConfig(
            nk=[NKSpec("dma", 1, ["dma_0"])],
            clocks=[ClockSpec(inst="dma_0", freq_hz=300_000_000)],
        )
        instances = apply_config_to_instances(cfg, [k])
        inst = next(i for i in instances if i.name == "dma_0")
        assert inst.params["clock_hz"] == 300_000_000

    def test_clock_unknown_instance_raises(self):
        k = _make_kernel("dma", {"m_axi_gmem": (BusType.AXI4FULL, 512)})
        cfg = ConnectivityConfig(
            nk=[NKSpec("dma", 1, ["dma_0"])],
            clocks=[ClockSpec(inst="nonexistent", freq_hz=300_000_000)],
        )
        with pytest.raises(KeyError, match="nonexistent"):
            apply_config_to_instances(cfg, [k])

    # --- sp mapping ---

    def test_sp_mapping_applied(self):
        k = _make_kernel("dma", {"m_axi_gmem": (BusType.AXI4FULL, 512)})
        cfg = ConnectivityConfig(
            nk=[NKSpec("dma", 1, ["dma_0"])],
            sps=[SpMapping("dma_0", "m_axi_gmem", MemoryTarget("HBM", 2))],
        )
        instances = apply_config_to_instances(cfg, [k])
        inst = next(i for i in instances if i.name == "dma_0")
        assert inst.params["mem_sp"]["m_axi_gmem"] == {
            "domain": "HBM", "index": 2}

    def test_sp_unknown_instance_raises(self):
        k = _make_kernel("dma", {"m_axi_gmem": (BusType.AXI4FULL, 512)})
        cfg = ConnectivityConfig(
            nk=[NKSpec("dma", 1, ["dma_0"])],
            sps=[SpMapping("nonexistent", "m_axi_gmem",
                           MemoryTarget("HBM", 0))],
        )
        with pytest.raises(KeyError, match="nonexistent"):
            apply_config_to_instances(cfg, [k])

    def test_sp_non_axi4full_port_raises(self):
        k = _make_kernel("dma", {
            "m_axi_gmem": (BusType.AXI4FULL, 512),
            "s_axi_control": (BusType.AXILITE, 32),
        })
        cfg = ConnectivityConfig(
            nk=[NKSpec("dma", 1, ["dma_0"])],
            sps=[SpMapping("dma_0", "s_axi_control", MemoryTarget("HBM", 0))],
        )
        with pytest.raises(ValueError, match="not an AXI4FULL port"):
            apply_config_to_instances(cfg, [k])

    def test_sp_port_name_case_insensitive(self):
        # Port defined as uppercase; sp references it in lowercase.
        k = _make_kernel("dma", {"M_AXI_GMEM": (BusType.AXI4FULL, 512)})
        cfg = ConnectivityConfig(
            nk=[NKSpec("dma", 1, ["dma_0"])],
            sps=[SpMapping("dma_0", "m_axi_gmem", MemoryTarget("DDR", 0))],
        )
        instances = apply_config_to_instances(cfg, [k])
        inst = next(i for i in instances if i.name == "dma_0")
        # Stored under the canonical (uppercase) name.
        assert "M_AXI_GMEM" in inst.params["mem_sp"]

    # --- AXI4FULL fallback ---

    def test_axi4full_fallback_mem_assigned(self):
        # An AXI4FULL port with no explicit sp gets the MEM fallback.
        k = _make_kernel("dma", {"m_axi_gmem": (BusType.AXI4FULL, 512)})
        cfg = ConnectivityConfig(nk=[NKSpec("dma", 1, ["dma_0"])])
        instances = apply_config_to_instances(cfg, [k])
        inst = next(i for i in instances if i.name == "dma_0")
        assert inst.params["mem_sp"]["m_axi_gmem"] == {
            "domain": "MEM", "index": ""}

    def test_explicit_sp_not_overwritten_by_fallback(self):
        # Explicit sp for one port, fallback for the other — both coexist correctly.
        k = _make_kernel("dma", {
            "m_axi_gmem0": (BusType.AXI4FULL, 512),
            "m_axi_gmem1": (BusType.AXI4FULL, 512),
        })
        cfg = ConnectivityConfig(
            nk=[NKSpec("dma", 1, ["dma_0"])],
            sps=[SpMapping("dma_0", "m_axi_gmem0", MemoryTarget("HBM", 3))],
        )
        instances = apply_config_to_instances(cfg, [k])
        inst = next(i for i in instances if i.name == "dma_0")
        assert inst.params["mem_sp"]["m_axi_gmem0"] == {
            "domain": "HBM", "index": 3}
        assert inst.params["mem_sp"]["m_axi_gmem1"] == {
            "domain": "MEM", "index": ""}

    def test_no_axi4full_ports_mem_sp_empty(self):
        # Kernels with no AXI4FULL ports still get mem_sp={} (empty, not absent).
        k = _make_kernel("passthrough", {
            "axis_in":  (BusType.AXIS, 64),
            "axis_out": (BusType.AXIS, 64),
        })
        cfg = ConnectivityConfig(nk=[NKSpec("passthrough", 1, ["pt_0"])])
        instances = apply_config_to_instances(cfg, [k])
        inst = next(i for i in instances if i.name == "pt_0")
        assert inst.params["mem_sp"] == {}
