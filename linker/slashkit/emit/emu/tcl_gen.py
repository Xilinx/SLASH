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
import logging

from slashkit.emit.render import render_template
from slashkit.emit.metadata.system_map_ctx import build_system_map_context, resolve_system_map_clock
from slashkit.emit.hw.user_region.addr_ctx import build_axilite_address_context
from slashkit.emit.emu.tb_ctx import build_tb_context
from slashkit.core.command_config import LinkerConfiguration

logger = logging.getLogger(__name__)


def generate_emu_tcl(config: LinkerConfiguration) -> None:
    # Retrieve inputs
    cfg = config.configuration
    instances = {kernel.name: kernel for kernel in config.kernel_instances}
    streams = cfg.streams
    kernel_hls_by_type = {
        kernel.name: kernel.hls_data_path for kernel in config.kernels}

    # Build test bench context
    tb_ctx = build_tb_context(instances, streams, kernel_hls_by_type)
    if isinstance(tb_ctx.get("emu_manifest"), dict):
        tb_ctx["emu_manifest"]["project"] = config._project_name

    # 4.1) Render tb.cpp
    tb_path = config.build_dir / "tb.cpp"
    tb_path.parent.mkdir(parents=True, exist_ok=True)
    render_template(
        template="sw_emu_tb.cpp",
        out_path=tb_path,
        context=tb_ctx,
    )
    logger.info("Rendered sw_emu tb.cpp to %s", tb_path)

    # 4.2) Render emu_manifest.json
    emu_manifest_path = config.build_dir / "emu_manifest.json"
    with emu_manifest_path.open("w", encoding="utf-8") as f:
        json.dump(tb_ctx.get("emu_manifest", {}), f, indent=2, sort_keys=True)
    logger.info("Rendered emu manifest to %s", emu_manifest_path)

    # 5) Render system map (Emulation)
    axilite_ctx = build_axilite_address_context(
        instances,
        addr_space="S_AXILITE_INI",
        base_offset=0x0202_0000_0000,
        min_align=0x0001_0000,
    )
    clock_hz = resolve_system_map_clock(config.clock_hz, instances)
    system_map_ctx = build_system_map_context(
        instances,
        axilite_ctx.get("axilite_addr", []),
        clock_hz=clock_hz,
        platform="Emulation",
        kernel_hls_by_type=kernel_hls_by_type,
        network=getattr(cfg, "network", None),
    )

    system_map_path = config.build_dir / "system_map.xml"
    render_template(
        template="system_map.xml",
        out_path=system_map_path,
        context=system_map_ctx,
    )
    logger.info("Rendered system map to %s", system_map_path)
