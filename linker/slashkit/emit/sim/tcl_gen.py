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

from pathlib import Path
import logging
import xml.etree.ElementTree as ET
from typing import List, Tuple
import importlib.resources as resources

from slashkit.emit.render import render_template
from slashkit.emit.hw.user_region.addr_ctx import build_axilite_address_context
from slashkit.emit.hw.service_region.stream_ctx import build_stream_connect_context
from slashkit.emit.metadata.system_map_ctx import build_system_map_context, resolve_system_map_clock
from slashkit.core.command_config import LinkerConfiguration

from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType

logger = logging.getLogger(__name__)

_IPXACT_NS = {
    "spirit": "http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009",
}


def _xml_text(el: ET.Element | None) -> str | None:
    if el is None or el.text is None:
        return None
    txt = el.text.strip()
    return txt if txt else None


def _find_sim_checkpoint_dcp(component_xml: Path) -> str | None:
    """
    Return the relative DCP path from the xilinx_simulationcheckpoint_view_fileset, if present.
    """
    root = ET.parse(component_xml).getroot()
    for fs in root.findall("spirit:fileSets/spirit:fileSet", _IPXACT_NS):
        if _xml_text(fs.find("spirit:name", _IPXACT_NS)) != "xilinx_simulationcheckpoint_view_fileset":
            continue
        for f in fs.findall("spirit:file", _IPXACT_NS):
            rel = _xml_text(f.find("spirit:name", _IPXACT_NS))
            if not rel:
                continue
            user_types = {
                (_xml_text(uft) or "").lower()
                for uft in f.findall("spirit:userFileType", _IPXACT_NS)
            }
            if "dcp" in user_types or rel.lower().endswith(".dcp"):
                return rel
    return None


def _collect_ports(
    instances: dict[str, KernelInstance],
) -> tuple[List[Tuple[str, str]], List[Tuple[str, str]], List[Tuple[str, str]], List[Tuple[str, str]]]:
    axilite: List[Tuple[str, str]] = []
    axifull: List[Tuple[str, str]] = []
    clocks: List[Tuple[str, str]] = []
    resets: List[Tuple[str, str]] = []

    for iname in sorted(instances.keys()):
        inst = instances[iname]
        ports = sorted(inst.kernel.ports.values(), key=lambda p: p.name)
        for p in ports:
            if p.ptype == BusType.AXILITE:
                axilite.append((iname, p.name))
            elif p.ptype == BusType.AXI4FULL:
                axifull.append((iname, p.name))
            elif p.ptype == BusType.CLOCK:
                phys = inst.kernel.bus_physical_port(p.name) or p.name
                clocks.append((iname, phys))
            elif p.ptype == BusType.RESET:
                phys = inst.kernel.bus_physical_port(p.name) or p.name
                resets.append((iname, phys))

    return axilite, axifull, clocks, resets


def _fmt_sc_slot(prefix: str, idx: int) -> str:
    if idx < 10:
        return f"{prefix}0{idx}_AXI"
    return f"{prefix}{idx}_AXI"


def _build_reduction_tree(
    sources: List[str],
    *,
    max_si: int = 16,
    max_roots: int | None = None,
    base_name: str = "sc_red",
) -> tuple[List[dict], List[str]]:
    """
    Build a SmartConnect reduction tree:
      - leaves connect sources to node SIs
      - each node outputs M00_AXI
    Returns (nodes, roots) where roots are src pins feeding the final consumer.
    """
    if max_roots is None:
        max_roots = max_si

    if len(sources) <= max_roots:
        return [], sources

    nodes: List[dict] = []
    level = 0
    current = [{"src": s} for s in sources]
    while len(current) > max_roots:
        groups = [current[i:i+max_si] for i in range(0, len(current), max_si)]
        next_level = []
        for g_idx, group in enumerate(groups):
            name = f"{base_name}_L{level}_{g_idx}"
            node = {
                "name": name,
                "num_si": len(group),
                "si": [
                    {"slot_name": _fmt_sc_slot("S", i), "src": g["src"]}
                    for i, g in enumerate(group)
                ],
            }
            nodes.append(node)
            next_level.append({"src": f"{name}/M00_AXI"})
        current = next_level
        level += 1

    roots = [g["src"] for g in current]
    return nodes, roots


def _build_fanout_tree(
    endpoints: List[str],
    *,
    max_mi: int = 16,
    base_name: str = "axi_sc",
    si_bd_port: str = "s_axi_ctrl",
) -> List[dict]:
    """
    Build a SmartConnect fanout tree (1 SI, many MIs) with max_mi per node.
    Returns nodes with create+connect metadata.
    """
    if not endpoints:
        return []

    nodes: dict[str, dict] = {}
    child_parent: dict[str, tuple[str, int]] = {}

    # Build leaf nodes that connect directly to endpoints.
    leaves: List[str] = []
    for idx, chunk_start in enumerate(range(0, len(endpoints), max_mi)):
        name = f"{base_name}_L0_{idx}"
        chunk = endpoints[chunk_start:chunk_start + max_mi]
        mi = [{"slot_name": _fmt_sc_slot("M", i), "dst_pin": ep}
              for i, ep in enumerate(chunk)]
        nodes[name] = {"name": name, "num_mi": len(mi), "mi": mi}
        leaves.append(name)

    # Build parent levels that fanout to child smartconnects.
    level = 1
    current = leaves
    while len(current) > 1:
        next_level: List[str] = []
        for g_idx, group_start in enumerate(range(0, len(current), max_mi)):
            group = current[group_start:group_start + max_mi]
            name = f"{base_name}_L{level}_{g_idx}"
            mi = []
            for i, child in enumerate(group):
                mi.append({"slot_name": _fmt_sc_slot("M", i),
                          "dst_pin": f"{child}/S00_AXI"})
                child_parent[child] = (name, i)
            nodes[name] = {"name": name, "num_mi": len(mi), "mi": mi}
            next_level.append(name)
        current = next_level
        level += 1

    root = current[0]
    for n in nodes.values():
        if n["name"] == root:
            n["si_from"] = {"type": "bd_port", "name": si_bd_port}
        else:
            parent, slot = child_parent[n["name"]]
            n["si_from"] = {"type": "smartconnect", "prev": parent,
                            "prev_slot_name": _fmt_sc_slot("M", slot)}

    # Stable order: root first, then others by name
    ordered = [nodes[root]] + \
        [n for k, n in sorted(nodes.items()) if k != root]
    return ordered


def _classify_mem_targets(instances: dict[str, KernelInstance]) -> tuple[List[str], List[str]]:
    mem0: List[str] = []
    mem1: List[str] = []
    for iname in sorted(instances.keys()):
        inst = instances[iname]
        mem_map = inst.params.get("mem_sp", {})
        for p in inst.kernel.ports_of_type(BusType.AXI4FULL):
            tgt = mem_map.get(p.name, {"domain": "MEM"})
            domain = (tgt.get("domain") or "MEM").upper()
            ep = f"{iname}/{p.name}"
            if domain == "DDR":
                mem1.append(ep)
            else:
                mem0.append(ep)
    return mem0, mem1


def generate_sim_tcl(config: LinkerConfiguration) -> None:
    # 1) Parse kernels
    cfg = config.configuration
    instances = {kernel.name: kernel for kernel in config.kernel_instances}
    streams = cfg.streams
    kernel_hls_by_type = {
        kernel.name: kernel.hls_data_path for kernel in config.kernels}

    kernel_sim_meta: dict[str, dict] = {}
    for kernel in config.kernels:
        kpath = kernel.component_xml_path

        sim_checkpoint_rel = _find_sim_checkpoint_dcp(kpath)
        sim_checkpoint_abs = None
        if sim_checkpoint_rel is not None:
            sim_checkpoint_abs = (kpath.parent / sim_checkpoint_rel).resolve()
            if not sim_checkpoint_abs.exists():
                raise FileNotFoundError(
                    f"Simulation checkpoint DCP from component.xml not found: {sim_checkpoint_abs}"
                )
        kernel_sim_meta[kernel.name] = {
            "component_xml": str(kpath),
            "sim_checkpoint_dcp": str(sim_checkpoint_abs) if sim_checkpoint_abs else None,
        }

    # 4) Build template context
    axilite_ports, axifull_ports, clock_ports, reset_ports = _collect_ports(
        instances)

    kernels_ctx = []
    for iname in sorted(instances.keys()):
        inst = instances[iname]
        vlnv = inst.kernel.vlnv or f"xilinx.com:hls:{inst.kernel.name}:1.0"
        kernels_ctx.append({"name": iname, "vlnv": vlnv})

    sim_checkpoint_netlists_ctx = []
    sim_ckpt_out_dir = config.build_dir / "checkpoint_funcsim"
    for iname in sorted(instances.keys()):
        inst = instances[iname]
        sim_meta = kernel_sim_meta.get(inst.kernel.name, {})
        dcp_path = sim_meta.get("sim_checkpoint_dcp")
        if not dcp_path:
            continue
        top_mod = f"top_{iname}_0"
        sim_checkpoint_netlists_ctx.append({
            "inst": iname,
            "dcp_path": dcp_path,
            "funcsim_v_path": str((sim_ckpt_out_dir / f"{top_mod}.v").resolve()),
            "rename_top": top_mod,
            "rename_prefix": f"{top_mod}_",
        })

    axilite_endpoints = [f"{iname}/{pname}" for iname, pname in axilite_ports]
    axilite_sc_ctx = _build_fanout_tree(
        axilite_endpoints,
        max_mi=16,
        base_name="axi_sc",
        si_bd_port="s_axi_ctrl",
    )

    mem_reduce_nodes, mem_roots = _build_reduction_tree(
        [f"{iname}/{pname}" for iname, pname in axifull_ports],
        max_si=16,
        max_roots=15,
        base_name="mem_sc_red",
    )
    mem_roots_ctx = [
        {"slot_name": _fmt_sc_slot("S", idx + 1), "src_pin": src}
        for idx, src in enumerate(mem_roots)
    ]
    mem_sc_num_si = 1 + len(mem_roots_ctx)

    stream_ctx = build_stream_connect_context(instances, streams)
    axis_streams_ctx = []
    for s in stream_ctx.get("axis_streams", []):
        axis_streams_ctx.append({
            "src_pin": s["src_pin"],
            "dst_pin": s["dst_pin"],
            "net_name": s["src_pin"].replace("/", "_"),
        })

    axilite_ctx = build_axilite_address_context(
        instances,
        addr_space="S_AXILITE_INI",
        base_offset=0x0202_0000_0000,
        min_align=0x0001_0000,
    )
    axilite_addr_ctx = []
    for item in axilite_ctx.get("axilite_addr", []):
        axilite_addr_ctx.append({
            "inst": item["inst"],
            "busif": item["busif"],
            "segment": item["segment"],
            "offset_hex": f"0x{item['offset']:X}",
            "range_hex": f"0x{item['range']:X}",
        })

    # 5) Render sim_prj.tcl template
    sim_out = config.build_dir / "run_pre.tcl"

    sim_mem_src = resources.read_text(
        "slashkit.resources.sim", "sim_mem.v", encoding="utf-8")
    sim_mem_dst = config.build_dir / "sim_mem.v"
    sim_mem_dst.write_text(sim_mem_src)

    render_template(
        template="sim_prj.tcl",
        out_path=sim_out,
        context={
            "sim_root": config.build_dir,
            "sim_prj_dir": str(config.build_dir / "sim_prj"),
            "ip_repo_path": str(config.build_dir / "iprepo"),
            "sim_mem_path": str(sim_mem_dst),
            "bd_name": "top",
            "part": "xcv80-lsva4737-2MHP-e-S",
            "kernels": kernels_ctx,
            "sim_checkpoint_netlists": sim_checkpoint_netlists_ctx,
            "axilite_scs": axilite_sc_ctx,
            "mem_reduce_nodes": mem_reduce_nodes,
            "mem_roots": mem_roots_ctx,
            "mem_sc_num_si": mem_sc_num_si,
            "clock_ports": [f"{iname}/{pname}" for iname, pname in clock_ports],
            "reset_ports": [f"{iname}/{pname}" for iname, pname in reset_ports],
            "axis_streams": axis_streams_ctx,
            "axilite_addr": axilite_addr_ctx,
        },
    )
    logger.info("Rendered simulation Tcl to %s", sim_out)

    # 6) Render system map (same as HW but marked as Simulation)
    clock_hz = resolve_system_map_clock(config.clock_hz, instances)
    system_map_ctx = build_system_map_context(
        instances,
        axilite_ctx.get("axilite_addr", []),
        clock_hz=clock_hz,
        platform="Simulation",
        kernel_hls_by_type=kernel_hls_by_type,
        network=getattr(cfg, "network", None),
    )
    system_map_out = config.build_dir / "system_map.xml"
    render_template(
        template="system_map.xml",
        out_path=system_map_out,
        context=system_map_ctx,
    )
    logger.info("Rendered system map to %s", system_map_out)
