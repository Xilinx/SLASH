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
import json


def infer_hls_json_from_component_xml(component_xml: Path) -> Path:
    """
    Given:
      .../sol1/impl/ip/component.xml
      .../hls/impl/ip/component.xml
    Return (preferred order):
      .../hls_data.json (alongside the solution dir)
      .../hls/hls_data.json (sibling to sol1)
      .../sol1_data.json (legacy)
    """
    p = component_xml.resolve()
    # ip -> impl -> <solution>
    sol_dir = p.parents[2]

    # Prefer new HLS metadata if present in the solution dir.
    hls_json = sol_dir / "hls_data.json"
    if hls_json.exists():
        return hls_json

    # Some flows keep hls_data.json in a sibling "hls" dir when component lives in "sol1".
    if sol_dir.name != "hls":
        sibling_hls = sol_dir.parent / "hls" / "hls_data.json"
        if sibling_hls.exists():
            return sibling_hls

    # Legacy fallback.
    sol1_json = sol_dir / "sol1_data.json"
    if sol1_json.exists():
        return sol1_json

    raise FileNotFoundError(
        "Cannot find HLS metadata inferred from "
        f"{p} -> tried: {hls_json}, {sol_dir.parent / 'hls' / 'hls_data.json'}, {sol1_json}"
    )


def _coerce_int(value: object) -> int | None:
    if value is None:
        return None
    if isinstance(value, int):
        return value
    txt = str(value).strip()
    if txt == "":
        return None
    try:
        return int(txt, 0)
    except ValueError:
        return None


def load_hls_metadata(hls_json: Path, *, strict: bool = True) -> dict | None:
    """
    Load HLS metadata JSON. In tolerant mode returns None on missing/invalid data.
    """
    try:
        data = json.loads(hls_json.read_text())
    except Exception:
        if strict:
            raise
        return None
    if not isinstance(data, dict):
        if strict:
            raise ValueError(
                f"HLS metadata root must be a JSON object: {hls_json}")
        return None
    return data


def parse_hls_args(hls_data: dict) -> list[dict]:
    """
    Parse hls_data.json Args into a stable list.
    """
    args_obj = hls_data.get("Args", {})
    if not isinstance(args_obj, dict):
        return []

    parsed: list[dict] = []
    for arg_name, info in args_obj.items():
        if not isinstance(info, dict):
            continue
        refs: list[dict] = []
        hw_refs = info.get("hwRefs", [])
        if isinstance(hw_refs, list):
            for ref in hw_refs:
                if not isinstance(ref, dict):
                    continue
                refs.append(
                    {
                        "type": str(ref.get("type", "") or ""),
                        "interface": str(ref.get("interface", "") or ""),
                        "name": str(ref.get("name", "") or ""),
                        "usage": str(ref.get("usage", "") or ""),
                        "direction": str(ref.get("direction", "") or ""),
                    }
                )

        parsed.append(
            {
                "name": str(arg_name),
                "index": _coerce_int(info.get("index")),
                "direction": str(info.get("direction", "") or ""),
                "src_type": str(info.get("srcType", "") or ""),
                "src_size": _coerce_int(info.get("srcSize")),
                "hw_refs": refs,
            }
        )

    parsed.sort(
        key=lambda a: (
            a["index"] is None,
            a["index"] if a["index"] is not None else 10**9,
            a["name"],
        )
    )
    return parsed
