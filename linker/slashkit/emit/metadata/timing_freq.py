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

import logging
import math
import os
import sys
from pathlib import Path
import re
from typing import Optional
import xml.etree.ElementTree as ET

logger = logging.getLogger(__name__)

HW_BUILD_DIR_ENV_KEYS = ("SLASH_HW_BUILD_DIR", "slash_hw_build_dir")
_FLOAT_RE = re.compile(r"[-+]?\d+(?:\.\d+)?")


def _find_design_timing_summary_row(report_text: str) -> Optional[list[float]]:
    if not report_text:
        return None

    lines = report_text.splitlines()
    design_idx = None
    for i, line in enumerate(lines):
        if "Design Timing Summary" in line:
            design_idx = i
            break
    if design_idx is None:
        return None

    header_idx = None
    for i in range(design_idx, min(design_idx + 120, len(lines))):
        if "WNS(ns)" in lines[i] and "TNS(ns)" in lines[i]:
            header_idx = i
            break
    if header_idx is None:
        return None

    for i in range(header_idx + 1, min(header_idx + 20, len(lines))):
        line = lines[i].strip()
        if not line:
            continue
        if set(line) <= {"-", " "}:
            continue
        values = [float(m.group(0)) for m in _FLOAT_RE.finditer(line)]
        if values:
            return values

    return None


def extract_design_timing_slacks_ns(report_text: str) -> Optional[tuple[float, float]]:
    values = _find_design_timing_summary_row(report_text)
    if values is None or len(values) < 3:
        return None
    return values[0], values[2]


def extract_design_wns_ns(report_text: str) -> Optional[float]:
    slacks = extract_design_timing_slacks_ns(report_text)
    if slacks is None:
        return None
    return slacks[0]


def design_timing_met(wns_ns: float, whs_ns: float) -> bool:
    return wns_ns >= 0 and whs_ns >= 0


def find_static_shell_timing_report(build_dir: Path, project_name: str) -> Optional[Path]:
    path = build_dir / f"report_timing_{project_name}.txt"
    if path.is_file():
        return path
    return None


def require_static_shell_timing_or_confirm(
    *,
    build_dir: Path,
    project_name: str,
    ignore_failure: bool,
    noninteractive: bool,
) -> None:
    timing_report = find_static_shell_timing_report(build_dir, project_name)
    if timing_report is None:
        raise RuntimeError(
            f"Static shell timing report not found: {build_dir / f'report_timing_{project_name}.txt'}"
        )

    report_text = timing_report.read_text(encoding="utf-8", errors="replace")
    slacks = extract_design_timing_slacks_ns(report_text)
    if slacks is None:
        raise RuntimeError(
            f"Could not parse WNS/WHS from static shell timing report: {timing_report}"
        )

    wns_ns, whs_ns = slacks
    if design_timing_met(wns_ns, whs_ns):
        logger.info(
            "Static shell timing met: WNS(ns)=%.3f, WHS(ns)=%.3f (%s)",
            wns_ns,
            whs_ns,
            timing_report,
        )
        return

    print("ERROR: Static shell timing failed.", file=sys.stderr)
    print(f"  WNS(ns)={wns_ns:.3f}", file=sys.stderr)
    print(f"  WHS(ns)={whs_ns:.3f}", file=sys.stderr)
    print(f"  Report: {timing_report}", file=sys.stderr)

    if ignore_failure:
        logger.warning(
            "Proceeding despite static shell timing failure (--ignore-timing-failure)"
        )
        return

    if noninteractive:
        raise RuntimeError(
            "Static shell timing failed and SLASH_NONINTERACTIVE is set; "
            "use --ignore-timing-failure to override"
        )

    if sys.stdin.isatty():
        answer = input(
            "Proceed with install/packaging anyway? [y/N] ").strip().lower()
        if answer in ("y", "yes"):
            logger.warning(
                "User confirmed install despite static shell timing failure")
            return
        raise SystemExit(1)

    raise RuntimeError(
        "Static shell timing failed and no interactive terminal is available; "
        "use --ignore-timing-failure to override"
    )


def compute_max_freq_hz_from_wns(wns_ns: float, base_freq_hz: int = 400_000_000) -> Optional[int]:
    if base_freq_hz <= 0:
        return None

    target_period_ns = 1e9 / float(base_freq_hz)
    achievable_period_ns = target_period_ns - float(wns_ns)
    if achievable_period_ns <= 0:
        return None

    max_freq_hz = math.floor(1e9 / achievable_period_ns)
    if max_freq_hz <= 0:
        return None
    return int(max_freq_hz)


def read_system_map_clock_hz(system_map_path: Path) -> Optional[int]:
    if not system_map_path.exists():
        return None

    try:
        root = ET.parse(system_map_path).getroot()
    except ET.ParseError:
        return None

    clock_node = root.find("ClockFrequency")
    if clock_node is None or clock_node.text is None:
        return None

    try:
        return int(clock_node.text.strip())
    except ValueError:
        return None


def write_system_map_clock_hz(system_map_path: Path, new_clock_hz: int) -> None:
    tree = ET.parse(system_map_path)
    root = tree.getroot()

    clock_node = root.find("ClockFrequency")
    if clock_node is None:
        clock_node = ET.SubElement(root, "ClockFrequency")
    clock_node.text = str(int(new_clock_hz))

    try:
        ET.indent(tree, space="  ")
    except AttributeError:
        pass
    tree.write(system_map_path, encoding="utf-8", xml_declaration=True)


def _resolve_hw_build_dir(explicit_hw_build_dir: Optional[Path]) -> Optional[Path]:
    if explicit_hw_build_dir is not None:
        return explicit_hw_build_dir.expanduser().resolve()

    for key in HW_BUILD_DIR_ENV_KEYS:
        configured = os.getenv(key)
        if configured:
            return Path(configured).expanduser().resolve()
    return None


def _find_timing_report(project_name: str, hw_build_dir: Path) -> Optional[Path]:
    slash_rm_dir = hw_build_dir / "rm" / f"slash_{project_name}"
    candidates = [
        slash_rm_dir / f"report_timing_{project_name}.txt",
        slash_rm_dir / "report_timing.txt",
    ]
    for path in candidates:
        if path.exists() and path.is_file():
            return path
    return None


def apply_timing_frequency_cap(
    *,
    project_name: str,
    system_map_path: Path,
    base_freq_hz: int = 400_000_000,
    hw_build_dir: Optional[Path] = None,
) -> Optional[int]:
    user_clock_hz = read_system_map_clock_hz(system_map_path)
    if user_clock_hz is None:
        logger.warning(
            "ClockFrequency missing or invalid in system_map.xml: %s", system_map_path)
        return None

    resolved_hw_build_dir = _resolve_hw_build_dir(hw_build_dir)
    if resolved_hw_build_dir is None:
        logger.warning(
            "HW build directory env var is unset; keeping user clock_hz=%d", user_clock_hz)
        return user_clock_hz

    timing_report = _find_timing_report(project_name, resolved_hw_build_dir)
    if timing_report is None:
        logger.warning(
            "Timing report not found under %s for project %s; keeping user clock_hz=%d",
            resolved_hw_build_dir,
            project_name,
            user_clock_hz,
        )
        return user_clock_hz

    logger.info("Timing report for frequency cap: %s", timing_report)
    report_text = timing_report.read_text(encoding="utf-8", errors="replace")
    wns_ns = extract_design_wns_ns(report_text)
    if wns_ns is None:
        logger.warning(
            "Could not parse WNS(ns) from timing report %s; keeping user clock_hz=%d", timing_report, user_clock_hz)
        return user_clock_hz

    computed_max_hz = compute_max_freq_hz_from_wns(
        wns_ns, base_freq_hz=base_freq_hz)
    if computed_max_hz is None:
        logger.warning(
            "Computed max frequency is invalid (WNS=%s ns, base=%d Hz); keeping user clock_hz=%d",
            wns_ns,
            base_freq_hz,
            user_clock_hz,
        )
        return user_clock_hz

    final_clock_hz = min(user_clock_hz, computed_max_hz)
    logger.info(
        "Timing frequency cap: WNS(ns)=%.3f, base_freq_hz=%d, computed_max_hz=%d, user_clock_hz=%d, final_clock_hz=%d",
        wns_ns,
        base_freq_hz,
        computed_max_hz,
        user_clock_hz,
        final_clock_hz,
    )

    if final_clock_hz != user_clock_hz:
        write_system_map_clock_hz(system_map_path, final_clock_hz)
        logger.info("Updated system_map ClockFrequency to %d: %s",
                    final_clock_hz, system_map_path)
    else:
        logger.info("Keeping user ClockFrequency=%d in system_map: %s",
                    user_clock_hz, system_map_path)

    return final_clock_hz
