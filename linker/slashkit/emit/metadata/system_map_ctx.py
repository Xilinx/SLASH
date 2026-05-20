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
from typing import Dict, List, Optional, Tuple
import logging
import re

from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType
from slashkit.core.regs import AddressBlock
from slashkit.emit.hls_meta import load_hls_metadata, parse_hls_args

DEFAULT_CLOCK_HZ = 200_000_000
_REG_SPLIT_RE = re.compile(r"^(.*)_\d+$")
_CONTROL_REGS = {"ctrl", "gier", "ip_ier", "ip_isr"}
_PORT_NORM_RE = re.compile(r"[^a-z0-9]")
logger = logging.getLogger(__name__)


def resolve_system_map_clock(
    clock_override: Optional[int],
    instances: Dict[str, KernelInstance],
    *,
    default_hz: int = DEFAULT_CLOCK_HZ,
) -> int:
    if clock_override is not None:
        return int(clock_override)
    freqs = sorted(
        {
            int(inst.params.get("clock_hz"))
            for inst in instances.values()
            if inst.params.get("clock_hz") is not None
        }
    )
    if freqs:
        return freqs[0]
    return default_hz


def _format_hex(value: int) -> str:
    if value == 0:
        return "0"
    return hex(value)


def _format_hex_prefixed(value: int) -> str:
    return hex(value)


def _normalize_access(access: Optional[str]) -> str:
    if not access:
        return ""
    key = access.strip().lower().replace("-", "_")
    return {
        "read_only": "R",
        "readonly": "R",
        "ro": "R",
        "r": "R",
        "write_only": "W",
        "writeonly": "W",
        "wo": "W",
        "w": "W",
        "read_write": "RW",
        "readwrite": "RW",
        "rw": "RW",
    }.get(key, access)


def _select_register_block(kernel, busif: str) -> Optional[AddressBlock]:
    mmaps = getattr(kernel, "memory_maps", []) or []

    for mm in mmaps:
        if mm.name and mm.name.lower() == busif.lower():
            for ab in mm.address_blocks:
                if (ab.usage or "").lower() == "register":
                    return ab
            if mm.address_blocks:
                return mm.address_blocks[0]

    for mm in mmaps:
        for ab in mm.address_blocks:
            if (ab.usage or "").lower() == "register":
                return ab

    for mm in mmaps:
        if mm.address_blocks:
            return mm.address_blocks[0]

    return None


def _register_stem(name: str) -> str:
    m = _REG_SPLIT_RE.fullmatch(name or "")
    return m.group(1) if m else name


def _is_split_register_name(name: str) -> bool:
    return _REG_SPLIT_RE.fullmatch(name or "") is not None


def _access_flags(access: Optional[str]) -> tuple[int, int]:
    norm = _normalize_access(access)
    return (1 if "R" in norm else 0, 1 if "W" in norm else 0)


def _port_norm(name: str) -> str:
    return _PORT_NORM_RE.sub("", (name or "").lower())


def _resolve_axi4full_port_name(kernel, requested: str) -> Optional[str]:
    if not requested:
        return None

    axi4_ports = [p.name for p in kernel.ports_of_type(BusType.AXI4FULL)]
    if requested in axi4_ports:
        return requested

    by_lower = {p.lower(): p for p in axi4_ports}
    req_low = requested.lower()
    if req_low in by_lower:
        return by_lower[req_low]

    by_norm = {_port_norm(p): p for p in axi4_ports}
    req_norm = _port_norm(requested)
    if req_norm in by_norm:
        return by_norm[req_norm]

    return None


def _build_functional_args_from_hls(
    hls_data: dict,
    busif: str,
    reg_block: Optional[AddressBlock],
    *,
    kernel=None,
    connected_axi_ports: Optional[set[str]] = None,
    instance_name: Optional[str] = None,
) -> List[dict]:
    if reg_block is None or not reg_block.registers:
        return []

    reg_by_name = {
        str(r.name): r for r in reg_block.registers if getattr(r, "name", None)}
    out: List[tuple[int, int, dict]] = []

    for order_idx, arg in enumerate(parse_hls_args(hls_data)):
        idx = arg["index"] if arg["index"] is not None else order_idx
        refs = []
        seen_names = set()
        interface_refs: List[str] = []
        seen_ifaces = set()
        for ref in arg.get("hw_refs", []):
            ref_type = str(ref.get("type", "")).lower()
            if ref_type == "interface":
                iface_name = str(ref.get("interface", "") or "")
                if iface_name and iface_name not in seen_ifaces:
                    seen_ifaces.add(iface_name)
                    interface_refs.append(iface_name)
                continue
            if ref_type != "register":
                continue
            usage = str(ref.get("usage", "")).lower()
            if usage not in {"data", "address"}:
                continue
            iface = str(ref.get("interface", "") or "")
            if iface and iface.lower() != busif.lower():
                continue
            reg_name = str(ref.get("name", "") or "")
            if not reg_name or reg_name in seen_names:
                continue
            reg = reg_by_name.get(reg_name)
            if reg is None:
                continue
            seen_names.add(reg_name)
            refs.append(reg)

        if not refs:
            continue

        r_flag = 0
        w_flag = 0
        for reg in refs:
            r, w = _access_flags(getattr(reg, "access", None))
            r_flag = max(r_flag, r)
            w_flag = max(w_flag, w)
        if r_flag == 0 and w_flag == 0:
            continue

        base_offset = min(int(getattr(reg, "address_offset", 0) or 0)
                          for reg in refs)
        logical_name = _register_stem(
            str(getattr(refs[0], "name", "") or arg["name"]))
        src_type = str(arg.get("src_type", ""))
        src_size = arg.get("src_size")
        reg_bits = sum(int(getattr(reg, "size", 32) or 32) for reg in refs)
        has_address_ref = any(
            str(ref.get("usage", "")).lower() == "address" for ref in (arg.get("hw_refs", []) or [])
        )
        arg_type = "buffer" if (
            "*" in src_type or has_address_ref) else "scalar"

        if arg_type == "buffer":
            if reg_bits > 0 and src_size is not None and src_size > 0:
                range_bits = max(int(src_size), int(reg_bits))
            elif reg_bits > 0:
                range_bits = int(reg_bits)
            elif src_size is not None and src_size > 0:
                range_bits = int(src_size)
            else:
                range_bits = 32
        else:
            if src_size is not None and src_size > 0:
                range_bits = int(src_size)
            elif reg_bits > 0:
                range_bits = int(reg_bits)
            else:
                range_bits = 32

        arg_item = {
            "idx": int(idx),
            "name": logical_name,
            "type": arg_type,
            "offset": _format_hex_prefixed(base_offset),
            "range": str(int(range_bits)),
            "r": int(r_flag),
            "w": int(w_flag),
        }

        if arg_type == "buffer" and kernel is not None and connected_axi_ports is not None:
            resolved_port = None
            for iface_name in interface_refs:
                canonical_port = _resolve_axi4full_port_name(
                    kernel, iface_name)
                if canonical_port is None:
                    continue
                if canonical_port not in connected_axi_ports:
                    continue
                resolved_port = canonical_port
                break
            if resolved_port is None:
                logger.warning(
                    "Could not correlate buffer arg '%s' on instance '%s' (kernel '%s') "
                    "to a connected AXI4FULL port from hwRefs interfaces %s; "
                    "omitting functional_args port metadata.",
                    arg["name"],
                    instance_name or "",
                    getattr(kernel, "name", ""),
                    interface_refs,
                )
            else:
                arg_item["port"] = resolved_port

        out.append(
            (
                int(idx),
                base_offset,
                arg_item,
            )
        )

    out.sort(key=lambda item: (item[0], item[1], item[2]["name"]))
    dense: List[dict] = []
    for new_idx, (_, _, item) in enumerate(out):
        cloned = dict(item)
        cloned["idx"] = new_idx
        dense.append(cloned)
    return dense


def _is_control_or_status_register(name: str) -> bool:
    low = (name or "").strip().lower()
    return low in _CONTROL_REGS or low.endswith("_ctrl")


def _infer_fallback_type(stem: str, is_split: bool) -> str:
    low = stem.lower()
    if is_split and (low.endswith("_r") or "ptr" in low):
        return "buffer"
    return "scalar"


def _build_functional_args_fallback(reg_block: Optional[AddressBlock]) -> List[dict]:
    if reg_block is None or not reg_block.registers:
        return []

    groups: Dict[str, dict] = {}
    for reg in sorted(reg_block.registers, key=lambda r: r.address_offset):
        reg_name = str(getattr(reg, "name", "") or "")
        if not reg_name or _is_control_or_status_register(reg_name):
            continue
        stem = _register_stem(reg_name)
        g = groups.get(stem)
        if g is None:
            g = {
                "name": stem,
                "offset": int(getattr(reg, "address_offset", 0) or 0),
                "regs": [],
                "split": False,
            }
            groups[stem] = g
        g["regs"].append(reg)
        g["split"] = bool(g["split"] or _is_split_register_name(reg_name))
        g["offset"] = min(g["offset"], int(
            getattr(reg, "address_offset", 0) or 0))

    ordered = sorted(groups.values(), key=lambda g: (g["offset"], g["name"]))
    out: List[dict] = []
    next_idx = 0
    for g in ordered:
        r_flag = 0
        w_flag = 0
        total_range = 0
        for reg in g["regs"]:
            r, w = _access_flags(getattr(reg, "access", None))
            r_flag = max(r_flag, r)
            w_flag = max(w_flag, w)
            total_range += int(getattr(reg, "size", 32) or 32)
        if r_flag == 0 and w_flag == 0:
            continue

        out.append(
            {
                "idx": next_idx,
                "name": g["name"],
                "type": _infer_fallback_type(g["name"], bool(g["split"])),
                "offset": _format_hex_prefixed(int(g["offset"])),
                "range": str(int(total_range)),
                "r": int(r_flag),
                "w": int(w_flag),
            }
        )
        next_idx += 1

    return out


def _coerce_optional_int(v) -> Optional[int]:
    if v is None:
        return None
    if isinstance(v, int):
        return v
    s = str(v).strip()
    if s == "":
        return None
    try:
        return int(s, 0)
    except ValueError:
        return None


def _assign_mem_indices(
    instances: Dict[str, KernelInstance],
    *,
    num_mem_ports: int = 8,
) -> Dict[Tuple[str, str], int]:
    buckets: Dict[int, List[Tuple[str, str]]] = {
        i: [] for i in range(num_mem_ports)}
    rr = 0

    for inst in instances.values():
        mem_sp = inst.params.get("mem_sp", {}) or {}
        for k_port, tgt in mem_sp.items():
            if tgt.get("domain") != "MEM":
                continue
            if inst.kernel.port(k_port).ptype != BusType.AXI4FULL:
                continue
            idx = _coerce_optional_int(tgt.get("index"))
            if idx is not None and not (0 <= idx < num_mem_ports):
                raise ValueError(
                    f"MEM index {idx} out of range (0..{num_mem_ports - 1}) for {inst.name}/{k_port}"
                )
            if idx is None:
                idx = rr % num_mem_ports
                rr += 1
            buckets[idx].append((inst.name, k_port))

    mapping: Dict[Tuple[str, str], int] = {}
    for idx, items in buckets.items():
        for inst_name, port in items:
            mapping[(inst_name, port)] = idx
    return mapping


def _format_target(domain: str, index: Optional[int]) -> str:
    dom = domain.upper()
    if index is None or index == "":
        return dom
    return f"{dom}{index}"


def build_system_map_context(
    instances: Dict[str, KernelInstance],
    axilite_addr: List[dict],
    *,
    clock_hz: int,
    kernel_hls_by_type: Optional[Dict[str, Path]] = None,
    platform: str = "Hardware",
    num_mem_ports: int = 8,
    num_virt: int = 4,
    network: Optional[object] = None,
) -> dict:
    axilite_by_inst: Dict[str, List[dict]] = {}
    for entry in axilite_addr:
        axilite_by_inst.setdefault(entry["inst"], []).append(entry)

    mem_indices = _assign_mem_indices(instances, num_mem_ports=num_mem_ports)

    hls_by_type = kernel_hls_by_type or {}
    hls_cache: Dict[str, Optional[dict]] = {}

    def _kernel_hls_data(kernel_type: str) -> Optional[dict]:
        if kernel_type in hls_cache:
            return hls_cache[kernel_type]
        hls_path = hls_by_type.get(kernel_type)
        if hls_path is None:
            hls_cache[kernel_type] = None
            return None
        hls_cache[kernel_type] = load_hls_metadata(
            Path(hls_path), strict=False)
        return hls_cache[kernel_type]

    kernels: List[dict] = []
    for inst_name in sorted(instances.keys()):
        inst = instances[inst_name]
        entries = sorted(axilite_by_inst.get(
            inst_name, []), key=lambda e: e["busif"])
        if not entries:
            continue

        selected = None
        for e in entries:
            if "control" in e["busif"].lower():
                selected = e
                break
        if selected is None:
            for e in entries:
                block = _select_register_block(inst.kernel, e["busif"])
                if block and block.registers:
                    selected = e
                    break
        if selected is None:
            selected = entries[0]

        reg_block = _select_register_block(inst.kernel, selected["busif"])
        registers: List[dict] = []
        if reg_block and reg_block.registers:
            for reg in sorted(reg_block.registers, key=lambda r: r.address_offset):
                registers.append(
                    {
                        "offset": _format_hex(reg.address_offset),
                        "name": reg.name,
                        "access": _normalize_access(reg.access),
                        "description": reg.description or "",
                        "range": str(reg.size),
                    }
                )
        connections: List[dict] = []
        connected_axi_ports: set[str] = set()
        mem_sp = inst.params.get("mem_sp", {}) or {}
        for port in inst.kernel.ports_of_type(BusType.AXI4FULL):
            tgt = mem_sp.get(port.name)
            if not tgt:
                continue
            connected_axi_ports.add(port.name)
            domain = str(tgt.get("domain", "")).upper()
            idx = _coerce_optional_int(tgt.get("index"))
            if domain == "MEM" and idx is None:
                idx = mem_indices.get((inst.name, port.name))
            connections.append(
                {
                    "port": port.name,
                    "target": _format_target(domain, idx),
                }
            )
        hls_data = _kernel_hls_data(inst.kernel.name)
        if hls_data is not None:
            functional_args = _build_functional_args_from_hls(
                hls_data,
                selected["busif"],
                reg_block,
                kernel=inst.kernel,
                connected_axi_ports=connected_axi_ports,
                instance_name=inst_name,
            )
        else:
            functional_args = _build_functional_args_fallback(reg_block)

        kernels.append(
            {
                "name": inst.name,
                "base_addr": _format_hex(int(selected["offset"])),
                "range": _format_hex(int(selected["range"])),
                "registers": registers,
                "functional_args": functional_args,
                "connections": connections,
            }
        )

    enabled_eth = []
    if network is not None:
        enabled_eth = sorted(getattr(network, "enabled_eth", set()) or [])

    service_layer = {
        "eth_enabled": bool(enabled_eth),
        "eth_indices": enabled_eth,
        "virt": [{"index": i, "connection": "unused"} for i in range(num_virt)],
    }

    return {
        "platform": platform,
        "clock_hz": int(clock_hz),
        "kernels": kernels,
        "service_layer": service_layer,
    }
