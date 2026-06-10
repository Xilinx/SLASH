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

import logging
import re

from slashkit.emit.render import render_template, export_package
from slashkit.emit.hw.user_region.kernel_ctx import build_kernel_add_context
from slashkit.emit.hw.user_region.smartconnect_ctx import build_axilite_smartconnect_context
from slashkit.emit.hw.user_region.hbm_ctx import build_hbm_smartconnect_context
from slashkit.emit.hw.user_region.ddr_ctx import build_ddr_smartconnect_context
from slashkit.emit.hw.user_region.mem_ctx import build_mem_smartconnect_context
from slashkit.emit.hw.user_region.virt_ctx import build_virt_smartconnect_context
from slashkit.emit.hw.user_region.terminator_ctx import build_axi_terminators_context
from slashkit.emit.hw.user_region.terminator_ctx import build_ddr_noc_terminators
from slashkit.emit.hw.user_region.terminator_ctx import build_mem_noc_terminators
from slashkit.emit.hw.user_region.terminator_ctx import build_virt_noc_terminators
from slashkit.emit.hw.user_region.terminator_ctx import build_host_noc_terminator
from slashkit.emit.hw.service_region.network_ctx import build_network_axis_context
from slashkit.emit.hw.service_region.stream_ctx import build_stream_connect_context
from slashkit.emit.hw.user_region.host_ctx import build_host_smartconnect_context
from slashkit.emit.hw.user_region.addr_ctx import build_axilite_address_context
from slashkit.emit.hw.user_region.param_ctx import build_data_width_param_context
from slashkit.emit.metadata.system_map_ctx import build_system_map_context, resolve_system_map_clock
from slashkit.emit.hw.service_region.service_layer_ctx import *

from slashkit.core.command_config import LinkerConfiguration

logger = logging.getLogger(__name__)

_RX_SRC_PIN_RE = re.compile(r"^/?dcmac_axis_noc_s_(\d+)/M00_AXIS$")


def _collect_used_targets(ctx: dict) -> set[str]:
    """! @brief Collect used NoC/BD targets from a rendered context.

    @param ctx Render context dictionary.
    @return Set of target names/pins that are already used.
    """
    used: set[str] = set()

    # HBM uses BD ports (HBM_AXI_XX) via root MI -> port
    for o in ctx.get("hbm_root_out", []):
        used.add(o["dst_port"])   # e.g., HBM_AXI_00

    # DDR uses NoC pins
    for item in ctx.get("ddr_direct", []):
        used.add(item["dst_pin"])     # e.g., /ddr_noc_0/S00_AXI
    for item in ctx.get("ddr_smart_roots", []):
        used.add(item["dst_pin"])

    # MEM (VNOC) uses NoC pins
    for item in ctx.get("mem_direct", []):
        used.add(item["dst_pin"])     # e.g., /hbm_vnoc_00/S00_AXI
    for item in ctx.get("mem_smart_roots", []):
        used.add(item["dst_pin"])

    # VIRT now also uses NoC pins (noc_virt_00..03/S00_AXI)
    for item in ctx.get("virt_direct", []):
        used.add(item["dst_pin"])
    for item in ctx.get("virt_smart_roots", []):
        used.add(item["dst_pin"])

    # HOST (QDMA bridge) uses NoC pin
    for item in ctx.get("host_direct", []):
        used.add(item["dst_pin"])
    for item in ctx.get("host_smart_roots", []):
        used.add(item["dst_pin"])

    return used


def print_memory_maps(k):
    """! @brief Print memory map details for a kernel.

    @param k Kernel object with memory_maps metadata.
    """
    if not getattr(k, "memory_maps", None):
        print("  (no memory maps)")
        return
    print("  Memory maps:")
    for mm in k.memory_maps:
        print(f"    - map: {mm.name}")
        for ab in mm.address_blocks:
            ba = f"0x{ab.base_address:X}"
            rg = f"0x{ab.range:X}"
            print(
                f"        block {ab.name}: base={ba} range={rg} width={ab.width} usage={ab.usage or '-'} access={ab.access or '-'}")
            if ab.offset_base_param or ab.offset_high_param:
                print(
                    f"          params: base_param={ab.offset_base_param or '-'} high_param={ab.offset_high_param or '-'}")
            if ab.registers:
                for r in ab.registers:
                    off = f"0x{r.address_offset:X}"
                    print(
                        f"          reg {r.name}: off={off} size={r.size} access={r.access or '-'} reset={('0x%X' % r.reset_value) if r.reset_value is not None else '-'}")
                    if r.fields:
                        for f in r.fields:
                            rng = f"[{f.bit_offset + f.bit_width - 1}:{f.bit_offset}]"
                            print(f"            - {f.name} {rng} access={f.access or '-'}"
                                  f" reset={('0x%X' % f.reset_value) if f.reset_value is not None else '-'}")


def print_kernel(k):
    """! @brief Print a kernel summary to stdout.

    @param k Kernel object to print.
    """
    print(f"\nKernel: {k.name}")
    for p in k.ports.values():
        print(f"  - {p.name:24s} {p.ptype.name:9s} width={p.width}")
    print_memory_maps(k)


def print_cfg(cfg):
    """! @brief Print connectivity config summary to stdout.

    @param cfg Connectivity config object.
    """
    print("\n[connectivity] nk entries:")
    if cfg.nk:
        for nk in cfg.nk:
            print(
                f"  - {nk.kernel_type}: count={nk.count}, names={nk.instance_names}")
    else:
        print("  (none)")

    print("\n[connectivity] stream_connect:")
    if cfg.streams:
        for s in cfg.streams:
            print(f"  - {s.src_inst}.{s.src_port} -> {s.dst_inst}.{s.dst_port}")
    else:
        print("  (none)")

    print("\n[connectivity] sp mappings:")
    if cfg.sps:
        for sp in cfg.sps:
            print(
                f"  - {sp.inst}.{sp.port} -> {sp.target.domain}{sp.target.index}")
    else:
        print("  (none)")

    print("\n[clock] specs:")
    if cfg.clocks:
        for c in cfg.clocks:
            print(f"  - {c.inst}: {c.freq_hz} Hz")
    else:
        print("  (none)")

    debug = getattr(cfg, "debug", None)
    debug_nets = getattr(debug, "nets", []) if debug is not None else []
    print("\n[debug] nets:")
    if debug_nets:
        for n in debug_nets:
            print(f"  - {n.inst}.{n.port}")
    else:
        print("  (none)")


def print_instances(instances, stream_edges):
    """! @brief Print instantiated kernels and stream edges to stdout.

    @param instances Mapping of instance name to instance object.
    @param stream_edges Stream edge list.
    """
    print("\nInstances created:")
    if not instances:
        print("  (none)")
        return
    for name, inst in instances.items():
        print(f"  - {name} : kernel={inst.kernel.name}")
        if inst.params:
            clk = inst.params.get("clock_hz")
            if clk is not None:
                print(f"      clock_hz: {clk}")
            mem_sp = inst.params.get("mem_sp")
            if mem_sp:
                for port, tgt in mem_sp.items():
                    idx = "" if tgt.get("index") is None else str(tgt["index"])
                    print(f"      sp: {port} -> {tgt['domain']}{idx}")
            others = {k: v for k, v in inst.params.items() if k not in {
                "clock_hz", "mem_sp"}}
            for k, v in others.items():
                print(f"      {k}: {v}")

    print("\nStream connections to wire:")
    if stream_edges:
        for s in stream_edges:
            print(f"  - {s.src_inst}.{s.src_port} -> {s.dst_inst}.{s.dst_port}")
    else:
        print("  (none)")


def print_bd_ports(bd):
    """! @brief Print block design ports to stdout.

    @param bd Block design ports container.
    """
    print("\nBlock Design Ports:")
    if not bd.ports:
        print("  (none)")
        return
    for logical in sorted(bd.ports.keys()):
        for p in bd.get_all(logical):
            dom = "" if p.domain is None else str(p.domain)
            idx = "" if p.index is None else str(p.index)
            wid = "" if p.width is None else str(p.width)
            rtl = "" if p.rtl_name is None else p.rtl_name
            print(
                f"  - {logical:12s} -> rtl={rtl:20s} {p.ptype.name:9s} width={wid:>4s} domain={dom:>4s} index={idx:>2s}")


def generate_tcl(config: LinkerConfiguration) -> None:
    """! @brief Generate Tcl and system map artifacts from inputs.

    @param args Parsed CLI arguments.
    """
    bd = config.block_design_ports
    cfg = config.configuration
    instances = {kernel.name: kernel for kernel in config.kernel_instances}
    streams = cfg.streams
    kernel_hls_by_type = {
        kernel.name: kernel.hls_data_path for kernel in config.kernels}

    ctx = build_kernel_add_context(instances)
    ctx.update(build_data_width_param_context(instances))
    ctx.update(build_axilite_smartconnect_context(instances))
    ctx.update(build_hbm_smartconnect_context(instances, bd, max_si=16))
    ctx.update(build_ddr_smartconnect_context(instances, max_si=16))
    ctx.update(build_mem_smartconnect_context(
        instances, num_mem_ports=8, max_si=16))
    ctx.update(build_host_smartconnect_context(instances, bd, max_si=16))
    ctx.update(build_virt_smartconnect_context(instances, bd, max_si=16))
    net_ctx = build_network_axis_context(instances, streams, cfg.net)
    ctx.update({
        # inst.AXIS -> /dcmac_axis_noc_k/S00_AXIS
        "axis_to_fabric":   net_ctx["axis_to_fabric"],
        # /dcmac_axis_noc_s_k/M00_AXIS -> inst.AXIS
        "axis_from_fabric": net_ctx["axis_from_fabric"],
    })
    used_rx_slots: set[int] = set()
    for e in net_ctx.get("axis_from_fabric", []):
        m = _RX_SRC_PIN_RE.match(str(e.get("src_pin", "")).strip())
        if m:
            used_rx_slots.add(int(m.group(1)))

    # Tie-off RX NoC tready only for unused RX slots (0..7).
    # If no RX is used, we tie all 8.
    dcmac_rx_tready_tie_slots = [i for i in range(8) if i not in used_rx_slots]
    ctx["dcmac_rx_tready_tie_pins"] = [
        f"dcmac_axis_noc_s_{i}/M00_AXIS_tready" for i in dcmac_rx_tready_tie_slots
    ]

    ctx.update(build_stream_connect_context(
        instances, net_ctx["streams_leftover"]))

    used_targets = _collect_used_targets(ctx)

    for s in ctx.get("hbm_sc_sinks", []):
        used_targets.add(s["dst"])

    terms_generic = build_axi_terminators_context(
        bd, used_targets)  # HBM/VIRT BD ports only
    terms_ddr_noc = build_ddr_noc_terminators(
        used_targets, num_ddr=4, noc_pin_fmt="/ddr_noc_{index}/S00_AXI")
    terms_mem_noc = build_mem_noc_terminators(
        used_targets, num_mem=8, noc_pin_fmt="/hbm_vnoc_0{index}/S00_AXI")
    terms_virt_noc = build_virt_noc_terminators(
        used_targets, num_virt=4, noc_pin_fmt="/noc_virt_0{index}/S00_AXI")
    terms_host_noc = build_host_noc_terminator(used_targets)

    ctx["axi_terminators"] = (
        terms_generic.get("axi_terminators", [])
        + terms_ddr_noc.get("axi_terminators", [])
        + terms_mem_noc.get("axi_terminators", [])
        + terms_virt_noc.get("axi_terminators", [])
        + terms_host_noc.get("axi_terminators", [])
    )
    axilite_ctx = build_axilite_address_context(
        instances,
        addr_space="S_AXILITE_INI",
        base_offset=0x0202_0000_0000,
        min_align=0x0001_0000,
    )
    ctx.update(axilite_ctx)
    ctx["project_name"] = config.project_name
    ctx["slash_bd_name"] = f"slash_{config.project_name}"
    out_path = config.build_dir / "slash.tcl"  # slash.tcl
    render_template(
        template="slash.tcl",
        out_path=out_path,
        context=ctx,
    )
    logger.info("Rendered Tcl to %s", out_path)

    clock_hz = resolve_system_map_clock(config.clock_hz, instances)
    system_map_ctx = build_system_map_context(
        instances,
        axilite_ctx.get("axilite_addr", []),
        clock_hz=clock_hz,
        platform="Hardware",
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

    svc_ctx = {}
    svc_ctx.update(build_service_layer_context(cfg.net))
    svc_ctx.update(build_service_axilite_ctx(cfg.net)
                   )    # SmartConnect + MI targets
    svc_ctx.update(build_service_noc_axis_ctx(cfg.net))

    svc_ctx["project_name"] = config.project_name
    svc_ctx["service_layer_bd_name"] = f"service_layer_{config.project_name}"

    # --- Render service-layer Tcl ---
    svc_out = config.build_dir / "service_layer.tcl"

    dcmac_dir = config.build_dir / "dcmac"
    if not dcmac_dir.is_dir():
        export_package("slashkit.resources.dcmac", dcmac_dir)

    svc_ctx.update(dcmac_paths(dcmac_dir))
    render_template(
        template="service_layer.tcl",
        out_path=svc_out,
        context=svc_ctx,
    )

    logger.info("Rendered service layer Tcl to %s", svc_out)
