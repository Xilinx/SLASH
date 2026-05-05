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

import importlib.resources as resources
import logging
import os
import shutil
import subprocess
import tarfile

from slashkit.core.command_config import LinkerConfiguration
from slashkit.emit.render import export_package

logger = logging.getLogger(__name__)


def create_sim_project(config: LinkerConfiguration) -> None:
    config.build_dir.mkdir(parents=True, exist_ok=True)

    # Clean generated subfolders but keep run_pre.tcl if already generated.
    for sub in ["sim_prj", "build", "xsim.dir"]:
        p = config.build_dir / sub
        if p.exists():
            shutil.rmtree(p, ignore_errors=True)
    for p in config.build_dir.glob("vpp_sim*"):
        if p.is_file():
            try:
                p.unlink()
            except OSError:
                pass

    # Copy all kernels into the IP repository
    for kernel in config.kernels:
        shutil.copytree(kernel.component_xml_path.parent,
                        config.ip_repository / kernel.name)

    tcl = config.build_dir / "run_pre.tcl"
    if not tcl.exists():
        raise FileNotFoundError(f"Simulation TCL not found: {tcl}")

    log_path = config.build_dir / "vivado.log"

    cmd = [
        config.vivado_bin,
        "-mode",
        "tcl",
        "-nojournal",
        "-log",
        str(log_path),
        "-source",
        str(tcl),
    ]
    subprocess.run(cmd, cwd=str(config.build_dir), check=True)


def build_sim_project(config: LinkerConfiguration) -> None:
    xsim_dir = config.build_dir / "sim_prj" / \
        "sim_prj.sim" / "sim_1" / "behav" / "xsim"
    if not xsim_dir.exists():
        raise FileNotFoundError(f"XSIM dir not found: {xsim_dir}")

    subprocess.run(["./compile.sh"], cwd=str(xsim_dir), check=True)
    subprocess.run(["./elaborate.sh"], cwd=str(xsim_dir), check=True)

    cmake_build_dir = config.build_dir / "build"

    # Copy xsim.dir into build dir for sim executable
    xsim_build_dir = cmake_build_dir / "xsim.dir"
    if xsim_build_dir.exists():
        shutil.rmtree(xsim_build_dir, ignore_errors=True)
    shutil.copytree(xsim_dir / "xsim.dir", xsim_build_dir)

    sim_src_dir = config.build_dir / "sim_src"
    export_package("slashkit.resources.sim", sim_src_dir)

    subprocess.run(["cmake", str(sim_src_dir)],
                   cwd=str(cmake_build_dir), check=True)
    jobs = str(os.cpu_count() or 8)
    subprocess.run(["make", "-j", jobs], cwd=str(cmake_build_dir), check=True)

    vpp_sim_path = cmake_build_dir / "vpp_sim"
    if not vpp_sim_path.exists():
        raise FileNotFoundError(f"vpp_sim not found: {vpp_sim_path}")
    shutil.copy2(vpp_sim_path, config.build_dir / "vpp_sim")

    # Copy xsim.dir next to vpp_sim for runtime
    xsim_result_dir = config.build_dir / "xsim.dir"
    if xsim_result_dir.exists():
        shutil.rmtree(xsim_result_dir, ignore_errors=True)
    shutil.copytree(xsim_build_dir, xsim_result_dir)

    system_map_path = config.build_dir / "system_map.xml"
    if not system_map_path.exists():
        raise FileNotFoundError(f"system_map.xml not found: {system_map_path}")

    with tarfile.open(config.out_path, mode="w") as tf:
        tf.add(system_map_path, arcname="system_map.xml")
        tf.add(vpp_sim_path, arcname="vpp_sim")
        tf.add(xsim_build_dir, arcname="xsim.dir")

    logger.info("Simulation build outputs in %s", config.build_dir)
