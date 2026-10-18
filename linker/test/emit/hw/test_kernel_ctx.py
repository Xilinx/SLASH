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

"""Tests for the kernel reset replicas: one per pinned SLR, one for unpinned kernels."""

import importlib.resources as resources
from pathlib import Path

import jinja2
import pytest

from slashkit.core.kernel import Kernel, KernelInstance
from slashkit.core.port import BusType, Port
from slashkit.emit.hw.user_region.kernel_ctx import build_kernel_add_context


def _instances(slr_by_name):
    kernel = Kernel(name="k", component_xml_path=Path(), ports={
        "ap_clk": Port(name="ap_clk", ptype=BusType.CLOCK, width=1),
        "ap_rst_n": Port(name="ap_rst_n", ptype=BusType.RESET, width=1)})
    instances = {}
    for name, slr in slr_by_name.items():
        inst = KernelInstance(name=name, kernel=kernel)
        if slr is not None:
            inst.params["slr"] = slr
        instances[name] = inst
    return instances


def _render(template, **ctx):
    """Render a link template on top of an empty kernel context."""
    env = jinja2.Environment(loader=jinja2.FileSystemLoader(
        str(resources.files("slashkit.resources.templates"))))
    return env.get_template(template).render(
        **{**build_kernel_add_context({}), **ctx})


def test_kernels_take_reset_from_the_replica_of_their_slr():
    ctx = build_kernel_add_context(_instances({"k_0": 1, "k_1": None, "k_2": 2, "k_3": 1}))
    assert {r["src_pin"]: r["replica"] for r in ctx["resets"]} == {
        "k_0/ap_rst_n": "rst_repl_slr1", "k_1/ap_rst_n": "rst_repl_kernels",
        "k_2/ap_rst_n": "rst_repl_slr2", "k_3/ap_rst_n": "rst_repl_slr1"}
    assert ctx["reset_replicas"] == ["rst_repl_kernels", "rst_repl_slr1", "rst_repl_slr2"]


def test_without_floorplan_one_replica_feeds_every_kernel():
    ctx = build_kernel_add_context(_instances({"k_0": None, "k_1": None}))
    assert ctx["reset_replicas"] == ["rst_repl_kernels"]


@pytest.mark.parametrize("template", ["slash.tcl", "slash_compute.tcl"])
def test_template_builds_replicas_off_the_spine_without_bufg_fabric(template):
    ctx = build_kernel_add_context(_instances({"k_0": 1, "k_1": None}))
    out = _render(template, **ctx)
    assert "util_ds_buf_0" not in out
    assert ("connect_bd_net -net c_shift_ram_0_Q  [get_bd_pins c_shift_ram_0/Q] \\\n"
            "  [get_bd_pins ilreduced_logic_0/Op1]") in out
    for replica in ("rst_repl_kernels", "rst_repl_slr1"):
        assert f"create_bd_cell -type ip -vlnv xilinx.com:ip:c_shift_ram:12.0 {replica} ]" in out
        assert f"connect_bd_net [get_bd_pins {replica}/D] [get_bd_pins ilreduced_logic_0/Res]" in out
    assert "connect_bd_net [get_bd_pins k_0/ap_rst_n] [get_bd_pins rst_repl_slr1/Q]" in out
    assert "connect_bd_net [get_bd_pins k_1/ap_rst_n] [get_bd_pins rst_repl_kernels/Q]" in out


@pytest.mark.parametrize("template", ["slash.tcl", "slash_compute.tcl"])
def test_terminators_are_held_in_reset_off_the_spine(template):
    terms = [{"name": "axi_register_slice_memterm_0", "dst": "/hbm_vnoc_04/S00_AXI"},
             {"name": "axi_register_slice_hostterm_0", "dst": "/qdma_slave_bridge_noc/S00_AXI",
              "clk": "user_clk"}]
    out = _render(template, axi_terminators=terms)
    assert "CONFIG.CONST_VAL {0}] $term_rst_asserted" in out
    for t in terms:
        assert (f"connect_bd_net [get_bd_pins {t['name']}/aresetn] "
                "[get_bd_pins term_rst_asserted/dout]") in out
    assert "/aresetn] [get_bd_pins ilreduced_logic_0/Res]" not in out.split(
        "=== AXI Register Slice terminators")[1].split("=== HOST aggregation")[0]


@pytest.mark.parametrize("template", ["slash.tcl", "slash_compute.tcl"])
def test_no_terminators_no_constant_cell(template):
    assert "term_rst_asserted" not in _render(template, axi_terminators=[])
