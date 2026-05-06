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
import re
from typing import Dict, List

from slashkit.core.port import BusType
from slashkit.emit.hls_meta import (
    load_hls_metadata,
    parse_hls_args,
)
from slashkit.core.kernel import KernelInstance
from slashkit.core.connectivity import StreamConnect


def _norm_stream_type(src: str) -> str:
    # Canonicalize stream types so downstream checks are stable.
    parsed = _parse_stream_type(src)
    if parsed is None:
        return src.strip()
    elem_type, is_ref = parsed
    return f"hls::stream<{elem_type}>{'&' if is_ref else ''}"


def _parse_stream_type(src: str) -> tuple[str, bool] | None:
    """
    Parse stream-like types with nested templates, e.g.
      stream<ap_uint<32>, 0>&
      hls::stream<ap_uint<512>>&
      hls::stream<qdma_axis<64,0,0,0> >&
    Returns (element_type, is_reference) or None if not a stream type.
    """
    s = src.strip()
    if not re.match(r"^(?:hls::)?stream\s*<", s):
        return None

    lt = s.find("<")
    if lt < 0:
        return None

    depth = 0
    gt = -1
    for i in range(lt, len(s)):
        c = s[i]
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
            if depth == 0:
                gt = i
                break
    if gt < 0:
        return None

    inner = s[lt + 1: gt]
    tail = s[gt + 1:].strip()
    is_ref = tail.endswith("&")

    # Split stream template args at top-level commas only.
    parts: list[str] = []
    cur: list[str] = []
    depth = 0
    for ch in inner:
        if ch == "<":
            depth += 1
            cur.append(ch)
        elif ch == ">":
            depth = max(0, depth - 1)
            cur.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur).strip())

    if not parts or not parts[0]:
        return None
    return parts[0], is_ref


def _is_stream(cpp_t: str) -> bool:
    return _parse_stream_type(cpp_t) is not None


def _stream_inner(cpp_t: str) -> str:
    parsed = _parse_stream_type(cpp_t)
    return parsed[0] if parsed is not None else "ap_uint<512>"


def _is_ptr(cpp_t: str) -> bool:
    return "*" in cpp_t


def _strip_ref(cpp_t: str) -> tuple[str, bool]:
    t = cpp_t.strip()
    # do not treat hls::stream<...>& as scalar ref
    if t.endswith("&") and not _is_stream(t):
        return t[:-1].strip(), True
    return t, False


def _select_register_block(kernel):
    mmaps = getattr(kernel, "memory_maps", []) or []

    for mm in mmaps:
        if mm.name and "control" in mm.name.lower():
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


_REG_SPLIT_RE = re.compile(r".*_\d+$")
_QDMA_PORT_RE = re.compile(r"qdma_(\d+)$")


def _is_split_reg_name(name: str) -> bool:
    return bool(_REG_SPLIT_RE.fullmatch(name or ""))


def _stream_aliases_for_edge(edge, wire_name: str) -> list[str]:
    names = [wire_name]

    def _maybe_add(inst_name: str, port_name: str, prefix: str) -> None:
        if not inst_name.lower().startswith("cips"):
            return
        m = _QDMA_PORT_RE.fullmatch(port_name or "")
        if not m:
            return
        names.append(f"{prefix}{m.group(1)}")

    _maybe_add(getattr(edge, "src_inst"), getattr(
        edge, "src_port"), "streamingBuffer_")
    _maybe_add(getattr(edge, "dst_inst"), getattr(
        edge, "dst_port"), "outputStreamingBuffer_")

    # preserve order while deduping
    out: list[str] = []
    seen: set[str] = set()
    for n in names:
        if n not in seen:
            seen.add(n)
            out.append(n)
    return out


def _infer_control_mode(*, has_axilite: bool, stream_only: bool) -> str:
    if has_axilite:
        return "s_axilite"
    if stream_only:
        return "ap_ctrl_none"
    return "unknown"


def _call_kind_for_cpp_type(cpp_t: str) -> str:
    return "buffer" if _is_ptr(cpp_t) else "scalar"


def parse_sol1_data(sol1_json: Path) -> dict:
    d = load_hls_metadata(sol1_json, strict=True)
    assert d is not None  # strict=True guarantees dict or exception
    top = str(d.get("Top", "") or "")
    args = []
    for order_idx, info in enumerate(parse_hls_args(d)):
        idx = info["index"] if info["index"] is not None else order_idx
        src_type = info["src_type"]
        cpp_type = _norm_stream_type(src_type)

        # interface name if present (axis_in/axis_out/m_axi_gmem0)
        iface = None
        for ref in info.get("hw_refs", []):
            if ref.get("type") == "interface":
                iface = ref.get("interface")
                break

        args.append(
            {
                "name": info["name"],
                "index": idx,
                "direction": info.get("direction"),
                "srcType": src_type,
                "cppType": cpp_type,
                "iface": iface,
            }
        )
    args.sort(key=lambda a: a["index"])
    return {"Top": top, "Args": args}


def build_tb_context(instances: Dict[str, KernelInstance], streams: List[StreamConnect], kernel_sol1_by_type: dict[str, Path]) -> dict:
    """
    instances: from apply_config_to_instances(), name -> Instance(kernel=KernelType,...)
    streams: list of edges, each with .src_inst .src_port .dst_inst .dst_port (your parser types)
    kernel_sol1_by_type: kernel-type-name -> HLS metadata json Path (hls_data.json / sol1_data.json)
    """

    # Load HLS metadata per kernel type
    hls_meta: dict[str, dict] = {}
    for ktype, sol1p in kernel_sol1_by_type.items():
        hls_meta[ktype] = parse_sol1_data(sol1p)

    # Prototypes: generated from Top + Args (metadata doesn't store full prototype)
    prototypes = []
    for ktype, meta in hls_meta.items():
        sig = [f'{a["cppType"]} {a["name"]}' for a in meta["Args"]]
        prototypes.append(f'void {meta["Top"]}({", ".join(sig)});')

    # Streams: wire name per stream_connect edge
    wires = []
    endpoint_to_wire: dict[str, str] = {}

    def _get_stream_ctype_from_endpoint(inst_name: str, iface: str) -> str | None:
        if inst_name not in instances:
            return None
        ktype = instances[inst_name].kernel.name
        meta = hls_meta[ktype]
        a = next((x for x in meta["Args"] if x["iface"]
                 == iface and _is_stream(x["cppType"])), None)
        if a is None:
            return None
        return _stream_inner(a["cppType"])

    def get_stream_ctype(edge) -> str:
        return (
            _get_stream_ctype_from_endpoint(edge.src_inst, edge.src_port)
            or _get_stream_ctype_from_endpoint(edge.dst_inst, edge.dst_port)
            or "ap_uint<512>"
        )

    stream_routes = []
    for i, e in enumerate(streams):
        wname = f"stream_{i}"
        ctype = get_stream_ctype(e)
        wires.append({"name": wname, "ctype": ctype})
        stream_routes.append(
            {"wire": wname, "ctype": ctype, "names": _stream_aliases_for_edge(e, wname)})
        endpoint_to_wire[f"{e.src_inst}.{e.src_port}"] = wname
        endpoint_to_wire[f"{e.dst_inst}.{e.dst_port}"] = wname

    # Variables + function dispatch blocks
    vars_decl = []
    function_calls = []
    ref_vars = []
    fetch_scalar_cases = []
    autostart_calls = []
    manifest_kernels = []

    for inst_name, inst in instances.items():
        ktype = inst.kernel.name
        meta = hls_meta[ktype]
        non_stream_args = []

        # declare per-arg vars (skip streams)
        for a in meta["Args"]:
            cpp_t, is_ref = _strip_ref(a["cppType"])
            vname = f"{inst_name}_{a['name']}"

            if _is_stream(cpp_t):
                continue
            if _is_ptr(cpp_t):
                base = cpp_t.split("*", 1)[0].strip()
                vars_decl.append(f"{base}* {vname}")
            else:
                vars_decl.append(f"{cpp_t} {vname}")
                if is_ref:
                    ref_vars.append(vname)
            non_stream_args.append(
                {
                    "name": a["name"],
                    "var": vname,
                    "cppType": cpp_t,
                    "is_ref": is_ref,
                }
            )

        decode_blocks = []
        call_args = []
        manifest_call_args = []

        argN = 0
        for a in meta["Args"]:
            cpp_t = a["cppType"]
            vname = f"{inst_name}_{a['name']}"

            if _is_stream(cpp_t):
                w = endpoint_to_wire.get(f"{inst_name}.{a['iface']}")
                call_args.append(w if w else "/*MISSING_STREAM*/")
                continue

            decode_blocks.append(
                f'argType = root["args"]["arg{argN}"]["type"].asString();')
            if _is_ptr(cpp_t):
                base = cpp_t.split("*", 1)[0].strip()
                decode_blocks.append('if (argType == "buffer") {')
                decode_blocks.append(
                    f'  std::string bufferName = root["args"]["arg{argN}"]["name"].asString();')
                decode_blocks.append(
                    '  if (buffers.find(bufferName) != buffers.end()) {')
                decode_blocks.append(
                    f'    {vname} = static_cast<{base}*>(buffers[bufferName]);')
                decode_blocks.append('  }')
                decode_blocks.append('}')
            else:
                decode_blocks.append('if (argType == "scalar") {')
                decode_blocks.append(
                    f'  assignValue({vname}, root["args"]["arg{argN}"]["value"]);')
                decode_blocks.append('}')

            include_in_manifest_call = True
            if (a.get("direction") or "in") == "out" and not _is_ptr(cpp_t):
                # VRT emu calls omit scalar/register-style outputs (read back via fetch),
                # but still pass pointer outputs (e.g. m_axi write destinations).
                include_in_manifest_call = False

            if include_in_manifest_call:
                manifest_call_args.append(
                    {
                        "arg": f"arg{argN}",
                        "kind": _call_kind_for_cpp_type(cpp_t),
                        "source_arg": a["name"],
                        "cpp_type": cpp_t,
                    }
                )
            call_args.append(vname)
            argN += 1

        function_calls.append(
            {
                "inst": inst_name,
                "top": meta["Top"],
                "decode_blocks": decode_blocks,
                "call_args": call_args,
            }
        )

        has_axilite = any(
            True for _ in inst.kernel.ports_of_type(BusType.AXILITE))
        stream_only = bool(meta["Args"]) and all(
            _is_stream(a["cppType"]) for a in meta["Args"])
        has_missing_stream = any(
            arg == "/*MISSING_STREAM*/" for arg in call_args)
        control_mode = _infer_control_mode(
            has_axilite=has_axilite, stream_only=stream_only)
        autostart = stream_only and not has_axilite and not has_missing_stream
        callable_kernel = has_axilite and not has_missing_stream
        scheduling_policy = "autostart" if autostart else "call"
        autostart_reason = "stream_only_no_axilite" if autostart else ""
        shutdown_policy = "fast_exit" if autostart else "normal_exit"
        if autostart:
            autostart_calls.append(
                {
                    "inst": inst_name,
                    "top": meta["Top"],
                    "call_args": call_args,
                }
            )
        manifest_kernels.append(
            {
                "instance": inst_name,
                "top": meta["Top"],
                "has_axilite": has_axilite,
                "control_mode": control_mode,
                "callable": callable_kernel,
                "autostart": autostart,
                "autostart_reason": autostart_reason,
                "scheduling_policy": scheduling_policy,
                "shutdown_policy": shutdown_policy,
                "missing_stream_bindings": has_missing_stream,
                "call_arg_count": len(manifest_call_args),
                "call_args": manifest_call_args,
                "registers": [],
                "args": [
                    {
                        "name": a["name"],
                        "cpp_type": a["cppType"],
                        "iface": a["iface"],
                        "direction": a.get("direction"),
                        "is_stream": _is_stream(a["cppType"]),
                        "is_pointer": _is_ptr(_strip_ref(a["cppType"])[0]),
                        "call_arg": next(
                            (
                                ca["arg"]
                                for ca in manifest_call_args
                                if ca["source_arg"] == a["name"]
                            ),
                            None,
                        ),
                        "call_kind": next(
                            (
                                ca["kind"]
                                for ca in manifest_call_args
                                if ca["source_arg"] == a["name"]
                            ),
                            None,
                        ),
                    }
                    for a in meta["Args"]
                ],
            }
        )

        reg_block = _select_register_block(inst.kernel)
        regs = []
        if reg_block is not None and getattr(reg_block, "registers", None):
            regs = sorted(reg_block.registers, key=lambda r: r.address_offset)
        manifest_kernels[-1]["registers"] = [
            {
                "name": (getattr(r, "name", "") or ""),
                "offset": int(getattr(r, "address_offset", 0) or 0),
                "width": int(getattr(r, "range", 32) or 32),
                "access": (getattr(r, "access", "") or ""),
                "description": (getattr(r, "description", "") or ""),
            }
            for r in regs
        ]

        # Mirror vrt::Kernel::read() emulation indexing, which starts after the
        # first 4 control registers and synthesizes argN based on register order.
        if regs:
            reg_idx = 4
            fetch_arg_idx = 0
            logical_arg_idx = 0
            prev_value_reg_name: str | None = None
            while reg_idx < len(regs):
                reg_name = getattr(regs[reg_idx], "name", "") or ""
                reg_off = int(getattr(regs[reg_idx], "address_offset", 0) or 0)

                if _is_split_reg_name(reg_name):
                    # Preserve the logical value name (e.g. "sum" from "sum_1") so a
                    # following "<name>_ctrl" validity register can be synthesized.
                    prev_value_reg_name = reg_name.rsplit("_", 1)[0]
                    if logical_arg_idx < len(non_stream_args):
                        hi_reg = regs[reg_idx +
                                      1] if (reg_idx + 1) < len(regs) else None
                        hi_reg_name = (
                            getattr(hi_reg, "name",
                                    "") or "" if hi_reg is not None else ""
                        )
                        hi_reg_off = (
                            int(getattr(hi_reg, "address_offset", 0) or 0)
                            if hi_reg is not None
                            else None
                        )
                        fetch_scalar_cases.append(
                            {
                                "inst": inst_name,
                                "arg": f"arg{fetch_arg_idx}",
                                "kind": "var",
                                "var": non_stream_args[logical_arg_idx]["var"],
                                "source": "register_metadata",
                                "register_name": reg_name,
                                "register_offset": reg_off,
                                "register_split": True,
                            }
                        )
                        # Expose the high 32-bit word of the same logical scalar for
                        # split 64-bit AXI-Lite register reads (e.g. sum_2).
                        if hi_reg_off is not None:
                            fetch_scalar_cases.append(
                                {
                                    "inst": inst_name,
                                    "arg": f"arg{fetch_arg_idx}",
                                    "kind": "var_u32_hi",
                                    "var": non_stream_args[logical_arg_idx]["var"],
                                    "source": "register_metadata",
                                    "register_name": hi_reg_name,
                                    "register_offset": hi_reg_off,
                                    "register_split": True,
                                    "register_split_part": "hi",
                                }
                            )
                        logical_arg_idx += 1
                    fetch_arg_idx += 1
                    reg_idx += 2
                    continue

                if prev_value_reg_name and reg_name == f"{prev_value_reg_name}_ctrl":
                    fetch_scalar_cases.append(
                        {
                            "inst": inst_name,
                            "arg": f"arg{fetch_arg_idx}",
                            "kind": "const_u32",
                            "value": 1,
                            "source": "register_metadata",
                            "register_name": reg_name,
                            "register_offset": reg_off,
                            "synthetic": "ctrl_valid",
                            "derived_from_register": prev_value_reg_name,
                        }
                    )
                    fetch_arg_idx += 1
                    reg_idx += 1
                    continue

                if logical_arg_idx < len(non_stream_args):
                    fetch_scalar_cases.append(
                        {
                            "inst": inst_name,
                            "arg": f"arg{fetch_arg_idx}",
                            "kind": "var",
                            "var": non_stream_args[logical_arg_idx]["var"],
                            "source": "register_metadata",
                            "register_name": reg_name,
                            "register_offset": reg_off,
                            "register_split": False,
                        }
                    )
                    logical_arg_idx += 1
                    prev_value_reg_name = reg_name
                else:
                    prev_value_reg_name = None

                fetch_arg_idx += 1
                reg_idx += 1
        elif non_stream_args:
            raise RuntimeError(
                "EMU fetch metadata generation requires register metadata for "
                f"kernel instance '{inst_name}'"
            )

    fetch_scalar_var_symbols = sorted(
        {
            c["var"]
            for c in fetch_scalar_cases
            if c.get("kind") in ("var", "var_u32_hi") and isinstance(c.get("var"), str)
        }
    )

    manifest_fetch_scalar = []
    for c in fetch_scalar_cases:
        entry = {
            "function": c["inst"],
            "arg": c["arg"],
            "kind": c["kind"],
        }
        if c["kind"] in ("var", "var_u32_hi"):
            entry["var_symbol"] = c["var"]
        elif c["kind"] == "const_u32":
            entry["value"] = int(c["value"])
        source = {"mode": c.get("source", "unknown")}
        if isinstance(c.get("register_name"), str):
            source["register_name"] = c["register_name"]
        if isinstance(c.get("register_offset"), int):
            source["register_offset"] = c["register_offset"]
        if c.get("register_split") is not None:
            source["register_split"] = bool(c["register_split"])
        if isinstance(c.get("synthetic"), str):
            source["synthetic"] = c["synthetic"]
        if isinstance(c.get("derived_from_register"), str):
            source["derived_from_register"] = c["derived_from_register"]
        entry["source"] = source
        manifest_fetch_scalar.append(entry)

    return {
        "prototypes": prototypes,
        "vars": vars_decl,
        "wires": wires,
        "stream_routes": stream_routes,
        "function_calls": function_calls,
        "autostart_calls": autostart_calls,
        "fetch_scalar_cases": fetch_scalar_cases,
        "fetch_scalar_var_symbols": fetch_scalar_var_symbols,
        "ref_vars": ref_vars,
        "emu_manifest": {
            "manifest_schema": {
                "name": "slash.sw_emu",
                "version": 1,
                "required_sections": ["kernels", "streams", "commands", "fetch"],
            },
            "emu_protocol_version": 1,
            "kernels": manifest_kernels,
            "streams": [
                {
                    "wire": s["wire"],
                    "ctype": s["ctype"],
                    "aliases": list(s["names"]),
                }
                for s in stream_routes
            ],
            "commands": [
                "populate",
                "stream_in",
                "stream_out",
                "call",
                "wait",
                "read_register",
                "fetch",
                "exit",
            ],
            "fetch": {
                "schema_version": 1,
                "scalar": manifest_fetch_scalar,
            },
        },
    }
