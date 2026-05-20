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
import tarfile
from pathlib import Path

from slashkit.core.command_config import LinkerConfiguration

logger = logging.getLogger(__name__)


def build_vbin(config: LinkerConfiguration) -> Path:
    """! @brief Build a compressed .vbin tarball for a project.

    @param project_name Project name used to locate results and name the archive.
    @param results_dir Optional override of the project results directory.
    @return Path to the generated .vbin file.
    """
    images_dir = config.build_dir / "images"
    service_layer_pdi_path = images_dir / \
        f"top_i_service_layer_service_layer_{config.project_name}_inst_0_partial.pdi"
    slash_pdi_path = images_dir / \
        f"top_i_slash_slash_{config.project_name}_inst_0_partial.pdi"
    util_xml = config.build_dir / \
        f"report_utilization_{config.project_name}.xml"
    system_map = config.build_dir / "system_map.xml"

    files = [slash_pdi_path, util_xml, system_map]
    if config.networking_enabled:
        files.append(service_layer_pdi_path)

    for file in files:
        if not file.exists():
            raise FileNotFoundError(file)

    logger.info("Creating vbin archive: %s", config.out_path)

    with tarfile.open(config.out_path, "w:gz") as tf:
        for path in files:
            arcname = path.relative_to(config.build_dir)
            logger.info("Adding to vbin: %s", arcname)
            tf.add(path, arcname=str(arcname))

    logger.info("vbin archive complete: %s", config.out_path)
    return config.out_path
