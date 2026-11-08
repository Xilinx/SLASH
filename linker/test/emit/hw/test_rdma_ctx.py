# SPDX-License-Identifier: MIT
"""The RDMA service in the service layer: contexts, staging, rendered Tcl, system map."""

import xml.etree.ElementTree as ET
from pathlib import Path
from types import SimpleNamespace

import pytest

from slashkit.core.connectivity import ConnectivityConfig, NetworkSpec
from slashkit.emit.emu.tcl_gen import generate_emu_tcl
from slashkit.emit.hw.service_region import rdma_ctx
from slashkit.emit.hw.service_region.service_layer_ctx import (
    build_service_axilite_ctx, build_service_layer_context, build_service_noc_axis_ctx,
    dcmac_paths)
from slashkit.emit.render import render_template


def _net(enabled, rdma=None, lanes=None, read_window=None):
    return NetworkSpec(enabled_eth=set(enabled), rdma=rdma, rdma_lanes=lanes,
                       rdma_read_window=read_window)


# ---------------------------------------------------------------- contexts

def test_rdma_port_is_not_a_plain_dcmac():
    ctx = build_service_layer_context(_net({0, 2}, rdma=0))
    assert (ctx["dc_enable_0"], ctx["dc_enable_1"]) == (0, 1)
    assert ctx["needs_dcmac"]


def test_rdma_only_still_needs_the_dcmac_sources():
    ctx = build_service_layer_context(_net({2}, rdma=2))
    assert (ctx["dc_enable_0"], ctx["dc_enable_1"]) == (0, 0)
    assert ctx["needs_dcmac"]


def test_no_rdma_unchanged():
    ctx = build_service_layer_context(_net({0, 2}))
    assert (ctx["dc_enable_0"], ctx["dc_enable_1"]) == (1, 1)
    ax = build_service_axilite_ctx(_net({0, 2}))
    assert ax["sl_num_clks"] == 1 and ax["sl_extra_clks"] == []


def test_rdma_registers_on_a_second_smartconnect_clock():
    ax = build_service_axilite_ctx(_net({0, 2}, rdma=2))
    assert ax["sl_mi_targets"] == [
        "qsfp_0_n_1/s_axi", "qsfp_2_n_3/s_axi", "rdma_0/s_axil", "sl_window_anchor/S_AXI"]
    assert ax["sl_num_mi"] == 4
    # only the plain port's hierarchy is clocked by service_clk
    assert ax["sl_qsfp_blocks"] == ["qsfp_0_n_1"]
    assert ax["sl_num_clks"] == 2
    assert ax["sl_extra_clks"] == ["qsfp_2_n_3/user_clk"]


def test_rdma_lanes_are_not_linked_to_the_mac():
    ctx = build_service_noc_axis_ctx(_net({0, 2}, rdma=0))
    links = ctx["sl_axis_noc_links"]
    pins = {l["src_pin"] for l in links} | {l["dst_pin"] for l in links}
    assert not any(p.startswith("qsfp_0_n_1") or p.startswith("dummy_noc_0") or
                   p.startswith("dummy_noc_m_0") for p in pins)
    assert "qsfp_2_n_3/S_AXIS_0" in pins


# ---------------------------------------------------------------- staging

@pytest.fixture
def fake_ip(tmp_path, monkeypatch):
    repo = tmp_path / "fns_iprepo"
    (repo / "rdma_200").mkdir(parents=True)
    (repo / "rdma_200" / "component.xml").write_text("<component/>")
    monkeypatch.setenv("SLASH_RDMA_IPREPO", str(repo))
    return repo


def test_stage_rdma_copies_sources_and_ip(tmp_path, fake_ip):
    dest = rdma_ctx.stage_rdma(tmp_path / "build" / "rdma")
    for f in ("hdl/rdma_service.sv", "hdl/rdma_service_bd.v", "tcl/rdma_service.tcl",
              "tcl/rdma_dcmac.tcl", "constraints/rdma.opt.post.tcl",
              "iprepo/rdma_200/component.xml"):
        assert (dest / f).is_file(), f


def test_stage_rdma_without_ip_explains(tmp_path, monkeypatch):
    monkeypatch.delenv("SLASH_RDMA_IPREPO", raising=False)
    monkeypatch.setattr(rdma_ctx, "rdma_ip_candidates",
                        lambda: [tmp_path / "none" / "rdma_200"])
    with pytest.raises(FileNotFoundError, match="stage-rdma.sh"):
        rdma_ctx.stage_rdma(tmp_path / "rdma")


def test_opt_post_chains_slash_hook_then_rdma(tmp_path):
    out = rdma_ctx.write_rdma_opt_post(
        tmp_path, Path("/x/service_layer_eth.opt.post.tcl"), 2)
    # service_layer_build.tcl looks for it
    assert out.name == "service_layer_eth.opt.post.tcl"
    lines = out.read_text().splitlines()
    assert lines[0] == "source {/x/service_layer_eth.opt.post.tcl}"
    assert lines[1] == "set ::slash_rdma_eth 2"
    assert lines[2].endswith("rdma/constraints/rdma.opt.post.tcl}")


# ---------------------------------------------------------------- rendering

def _render(tmp_path, net, rdma_dir=None):
    ctx = {}
    ctx.update(build_service_layer_context(net))
    ctx.update(build_service_axilite_ctx(net))
    ctx.update(build_service_noc_axis_ctx(net))
    ctx.update(dcmac_paths(tmp_path / "dcmac"))
    ctx.update(rdma_ctx.build_service_rdma_ctx(net, rdma_dir))
    ctx["project_name"] = "p"
    ctx["service_layer_bd_name"] = "service_layer_p"
    out = tmp_path / "service_layer.tcl"
    render_template("service_layer.tcl", out, ctx)
    return out.read_text()


def test_template_with_rdma(tmp_path):
    text = _render(tmp_path, _net({0, 2}, rdma=0), tmp_path / "rdma")
    assert f'source [file normalize "{tmp_path / "rdma" / "tcl" / "rdma_service.tcl"}"]' in text
    assert (f'slash_add_rdma 0 {rdma_ctx.RDMA_BUILD} [file normalize "{tmp_path / "rdma"}"] {{0 1}} '
            f'{rdma_ctx.RDMA_READ_WINDOW}\n' in text)
    assert "connect_bd_net [get_bd_pins qsfp_0_n_1/user_clk] [get_bd_pins sl_xbar/aclk1]" in text
    assert "set ::DCMAC0_ENABLED   0" in text and "set ::DCMAC1_ENABLED   1" in text
    # the RDMA block comes before the SmartConnect that targets rdma_0, its
    # control addresses after it (else they are not reachable and get placed
    # anywhere: the first hardware image had rdma_0 at 0x203_0000_0000)
    assert text.index("slash_add_rdma 0") < text.index(
        "smartconnect:1.0 smartconnect_0")
    i = text.index("slash_rdma_assign_addresses 0")
    assert text.index("smartconnect:1.0 smartconnect_0") < i < text.rindex(
        "\nassign_bd_address\n")


def test_template_with_rdma_lanes(tmp_path):
    net = _net({0}, rdma=0, lanes=(4, 6))
    text = _render(tmp_path, net, tmp_path / "rdma")
    assert (f'slash_add_rdma 0 {rdma_ctx.RDMA_BUILD} [file normalize "{tmp_path / "rdma"}"] {{4 6}}'
            in text)


def test_template_with_rdma_read_window(tmp_path):
    net = _net({0}, rdma=0, read_window=16)
    text = _render(tmp_path, net, tmp_path / "rdma")
    assert (f'slash_add_rdma 0 {rdma_ctx.RDMA_BUILD} [file normalize "{tmp_path / "rdma"}"] {{0 1}} 16\n'
            in text)


def test_rdma_service_tcl_takes_the_read_window():
    tcl = (Path(rdma_ctx.__file__).resolve().parents[3] / "resources" / "rdma" / "tcl"
           / "rdma_service.tcl").read_text()
    assert "proc slash_add_rdma {eth build rdma_dir {lanes {}} {read_window 32}}" in tcl
    assert "CONFIG.RD_WINDOW $read_window" in tcl


def test_template_without_rdma(tmp_path):
    text = _render(tmp_path, _net({0}))
    assert "slash_add_rdma" not in text
    assert "aclk1" not in text


# ---------------------------------------------------------------- system map

def test_system_map_rdma_element(tmp_path):
    cfg = ConnectivityConfig()
    cfg.net = _net({0}, rdma=0)
    generate_emu_tcl(SimpleNamespace(
        configuration=cfg, kernel_instances=[], kernels=[], _project_name="r",
        build_dir=tmp_path, clock_hz=300_000_000, shell_type=SimpleNamespace(value="service")))
    root = ET.parse(tmp_path / "system_map.xml").getroot()
    r = root.find("ServiceLayer/Rdma")
    assert r is not None
    assert r.attrib == {"eth": "0", "base_addr": "0x020304000000", "range": "0x01000000",
                        "mem_base": "0x060000000000", "mem_range": "0x000800000000"}
