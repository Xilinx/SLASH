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
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple
import re

from slashkit.core.kernel import Kernel, KernelInstance
from slashkit.core.connectivity import *
from slashkit.core.port import BusType


# -----------------------------
# Parsing helpers
# -----------------------------

_RE_TARGET = re.compile(r"^\s*([A-Za-z]+)\s*(\d*)\s*$")
_RE_NK = re.compile(r"^\s*([^:]+)\s*:\s*(\d+)(?::(.*))?\s*$")
_RE_ETH_KEY = re.compile(r"^eth_(\d+)$", re.IGNORECASE)
_RE_DEBUG_NET = re.compile(r"^\s*([^.:\s]+)\.([^.:\s]+)\s*$")


def _parse_target(s: str) -> MemoryTarget:
    m = _RE_TARGET.match(s)
    if not m:
        raise ValueError(
            f"Invalid memory target '{s}'. Expected e.g. HBM0, DDR3, MEM, HOST.")
    domain, idx_str = m.group(1).upper(), m.group(2)
    if domain not in {"HBM", "DDR", "MEM", "VIRT", "HOST"}:
        raise ValueError(
            f"Unsupported memory domain '{domain}'. Use HBM, DDR, MEM, VIRT or HOST.")
    # HOST (and MEM) have no numeric index
    idx = int(idx_str) if (idx_str and domain not in {"MEM", "HOST"}) else ""
    return MemoryTarget(domain=domain, index=idx)


def _split_instance_names(s: str) -> list[str]:
    return [x for x in re.split(r"[.\s,]+", s.strip()) if x]


def _parse_nk_value(val: str) -> NKSpec:
    """
    Accepts:
      nk=perf:15:perf_0.perf_1....perf_14
      nk=dma:2:dma_0 dma_1
      nk=offset:1:offset_0
      nk=foo:3                        # auto-names: foo_0..foo_2
    """
    m = _RE_NK.match(val)
    if not m:
        raise ValueError(
            f"Invalid nk entry: '{val}' (expected '<kernel>:<count>[:<names>]').")

    kernel_type = m.group(1).strip()
    count = int(m.group(2))
    names_str = (m.group(3) or "").strip()

    names = _split_instance_names(names_str) if names_str else []
    if len(names) != count:
        # Auto-fill or trim to match 'count'
        base = kernel_type
        names = (
            names + [f"{base}_{i}" for i in range(len(names), count)])[:count]

    return NKSpec(kernel_type=kernel_type, count=count, instance_names=names)


def _parse_stream_connect_value(val: str) -> StreamConnect:
    """
    Expects: 'srcInst.srcPort:dstInst.dstPort'
    """
    try:
        left, right = val.split(":")
        src_inst, src_port = left.split(".", 1)
        dst_inst, dst_port = right.split(".", 1)
        return StreamConnect(src_inst.strip(), src_port.strip(),
                             dst_inst.strip(), dst_port.strip())
    except Exception as e:
        raise ValueError(
            f"Invalid stream_connect '{val}'. Expected 'a.b:c.d'") from e


def _parse_sp_value(val: str) -> SpMapping:
    """
    Expects: 'inst.port:HBM0' or 'inst.port:DDR3'
    """
    try:
        left, right = val.split(":")
        inst, port = left.split(".", 1)
    except Exception as e:
        raise ValueError(
            f"Invalid sp '{val}'. Expected 'inst.port:TARGET'") from e
    target = _parse_target(right.strip())
    return SpMapping(inst=inst.strip(), port=port.strip(), target=target)


def _parse_debug_net_value(val: str) -> DebugNetSpec:
    m = _RE_DEBUG_NET.match(val)
    if not m:
        raise ValueError(
            f"Invalid debug net '{val}'. Expected '<instance>.<port>'.")
    return DebugNetSpec(inst=m.group(1).strip(), port=m.group(2).strip())


# -----------------------------
# Main parser
# -----------------------------

def parse_connectivity_file(path: str | Path) -> ConnectivityConfig:
    """
    Custom parser that supports repeated [clock] sections, [network] section,
    [user_region] section, [debug] section, and a single [connectivity] section.
    Lines beginning with '#' or ';' are ignored as comments.
    """
    cfg = ConnectivityConfig()
    path = Path(path)
    lines = path.read_text(encoding="utf-8").splitlines()

    section: Optional[str] = None
    pending_clock: Dict[str, str] = {}
    enabled_eth: set[int] = set()
    pre_synth_tcls: list[str] = []
    debug_nets: list[DebugNetSpec] = []

    def _commit_clock():
        nonlocal pending_clock
        if not pending_clock:
            return
        krnl = pending_clock.get("krnl")
        freq = pending_clock.get("freqhz")
        if krnl and freq:
            try:
                cfg.clocks.append(
                    ClockSpec(inst=krnl.strip(), freq_hz=int(freq.strip())))
            except ValueError:
                raise ValueError(f"Invalid freqhz value in [clock]: '{freq}'")
        elif krnl or freq:
            raise ValueError(
                "Incomplete [clock] block: both 'krnl' and 'freqhz' are required.")
        pending_clock = {}

    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue

        if line.startswith("[") and line.endswith("]"):
            # New section starting — commit any pending clock
            _commit_clock()
            section = line[1:-1].strip().lower()
            continue

        if section == "connectivity":
            if line.startswith("nk="):
                cfg.nk.append(_parse_nk_value(line.split("=", 1)[1].strip()))
            elif line.startswith("stream_connect="):
                cfg.streams.append(_parse_stream_connect_value(
                    line.split("=", 1)[1].strip()))
            elif line.startswith("sp="):
                cfg.sps.append(_parse_sp_value(line.split("=", 1)[1].strip()))

        elif section == "clock":
            # Accumulate key-value pairs for this clock block
            if "=" in line:
                k, v = line.split("=", 1)
                pending_clock[k.strip().lower()] = v.strip()
            else:
                raise ValueError(f"Invalid line in [clock] section: '{line}'")

        elif section == "network":
            # Parse eth_<idx>=<0|1> (nonzero means enabled)
            if "=" not in line:
                raise ValueError(
                    f"Invalid line in [network] section: '{line}'")
            k, v = [t.strip() for t in line.split("=", 1)]
            m = _RE_ETH_KEY.match(k)
            if not m:
                # ignore unknown keys in [network] to be lenient
                continue
            idx = int(m.group(1))
            try:
                val = int(v, 0)
            except ValueError:
                val = 0
            if val != 0:
                enabled_eth.add(idx)

        elif section == "user_region":
            if "=" not in line:
                raise ValueError(
                    f"Invalid line in [user_region] section: '{line}'")
            k, v = [t.strip() for t in line.split("=", 1)]
            if k.lower() == "pre_synth":
                if not v:
                    raise ValueError(
                        "Invalid line in [user_region] section: empty pre_synth path")
                tcl_path = Path(v).expanduser()
                if not tcl_path.is_absolute():
                    tcl_path = path.parent / tcl_path
                pre_synth_tcls.append(str(tcl_path.resolve()))
            else:
                # ignore unknown keys in [user_region] to be lenient
                continue

        elif section == "debug":
            if "=" not in line:
                raise ValueError(f"Invalid line in [debug] section: '{line}'")
            k, v = [t.strip() for t in line.split("=", 1)]
            if k.lower() != "net":
                raise ValueError(
                    f"Invalid key '{k}' in [debug] section. Only 'net=<instance>.<port>' is supported."
                )
            debug_nets.append(_parse_debug_net_value(v))

        else:
            pass

    # End of file: commit any trailing clock block
    _commit_clock()

    # Attach network spec
    cfg.net = NetworkSpec(enabled_eth=enabled_eth)
    cfg.user_region = UserRegionSpec(pre_synth_tcls=pre_synth_tcls)
    cfg.debug = DebugSpec(nets=debug_nets)

    return cfg


def _resolve_port_name_for_kernel(kernel: Kernel, requested: str) -> str:
    # Case-insensitive resolution to the canonical name from component.xml
    if requested in kernel.ports:
        return requested
    low_map = {n.lower(): n for n in kernel.ports.keys()}
    req = requested.lower()
    if req in low_map:
        return low_map[req]
    raise KeyError(
        f"Port '{requested}' not found on kernel '{kernel.name}'. "
        f"Available: {list(kernel.ports.keys())}"
    )


def apply_config_to_instances(
    cfg: ConnectivityConfig,
    kernel_library: List[Kernel],
    *,
    default_ddr_index: int = 0  # DDR0 fallback for missing AXI4FULL ports
) -> List[KernelInstance]:
    instances: Dict[str, KernelInstance] = {}
    kernel_library = {kernel.name: kernel for kernel in kernel_library}

    # 1) Instantiate from nk
    for nk in cfg.nk:
        if nk.kernel_type not in kernel_library:
            raise KeyError(
                f"Kernel type '{nk.kernel_type}' not found in kernel_library.")
        k = kernel_library[nk.kernel_type]
        for name in nk.instance_names:
            if name in instances:
                raise ValueError(f"Duplicate instance name '{name}'.")
            instances[name] = KernelInstance(name=name, kernel=k)

    # 2) Attach clock frequencies
    for c in cfg.clocks:
        if c.inst not in instances:
            raise KeyError(f"[clock] refers to unknown instance '{c.inst}'.")
        instances[c.inst].params["clock_hz"] = c.freq_hz

    # 3) Apply explicit sp mappings (store with CANONICAL port names)
    for sp in cfg.sps:
        if sp.inst not in instances:
            raise KeyError(
                f"[connectivity] sp refers to unknown instance '{sp.inst}'.")
        inst = instances[sp.inst]
        canon_port = _resolve_port_name_for_kernel(inst.kernel, sp.port)
        if inst.kernel.port(canon_port).ptype != BusType.AXI4FULL:
            raise ValueError(
                f"[connectivity] sp '{sp.inst}.{sp.port}' is not an AXI4FULL port on kernel '{inst.kernel.name}'."
            )
        mem_map: Dict[str, dict] = inst.params.setdefault("mem_sp", {})
        mem_map[canon_port] = {
            "domain": sp.target.domain, "index": sp.target.index}

    # 4) Per-instance fallback: fill ONLY the missing AXI4FULL ports with MEM (round-robin later)
    for inst in instances.values():
        mem_map: Dict[str, dict] = inst.params.setdefault("mem_sp", {})
        axi_full_ports = [
            p.name for p in inst.kernel.ports_of_type(BusType.AXI4FULL)]
        for pname in axi_full_ports:
            if pname not in mem_map:
                mem_map[pname] = {"domain": "MEM", "index": ""}

    return list(instances.values())
