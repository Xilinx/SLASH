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

import os
from enum import Enum
from pathlib import Path
import logging
import re
import shutil
import subprocess
import importlib.resources as resources
from typing import Optional, Dict
from contextlib import ExitStack

from slashkit.emit.metadata.report_util import convert_report_utilization_to_xml
from slashkit.emit.render import export_package
from slashkit.core.command_config import LinkerConfiguration, InstallerConfiguration, CommandConfiguration

logger = logging.getLogger(__name__)

AVED_DESIGN_NAME = "amd_v80_gen5x8_25.1"


# Host toolchain flags injected by dpkg-buildpackage (e.g. -mno-omit-leaf-frame-pointer,
# -fcf-protection, -fstack-clash-protection) are not understood by the arm-xilinx-eabi
# cross-compiler used for the AVED AMC firmware. Strip them before shelling out.
_CROSS_BUILD_ENV_BLOCKLIST = (
    "CFLAGS",
    "CXXFLAGS",
    "CPPFLAGS",
    "LDFLAGS",
    "FFLAGS",
    "FCFLAGS",
    "OBJCFLAGS",
    "OBJCXXFLAGS",
    "GCJFLAGS",
    "ASFLAGS",
)


def _clean_cross_build_env() -> dict[str, str]:
    env = {k: v for k, v in os.environ.items()
           if k not in _CROSS_BUILD_ENV_BLOCKLIST}
    return {k: v for k, v in env.items() if not k.startswith("DEB_")}


def _copy_checked(src: Path, dest: Path) -> None:
    if not src.exists():
        raise FileNotFoundError(f"Expected file not found: {src}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)


def _copy_files(src_files: list[Path], destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for src in src_files:
        dst = destination / src.name
        # Allow install_dir to match the staging directory without failing on no-op copies.
        if dst.exists():
            try:
                if src.samefile(dst):
                    logger.info(
                        "Skipping copy because source and destination are the same file: %s", src)
                    continue
            except FileNotFoundError:
                pass
        shutil.copy2(src, dst)


def _copy_tree(src_dir: Path, destination: Path) -> None:
    target_dir = destination / src_dir.name
    target_dir.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src_dir, target_dir, dirs_exist_ok=True)


def _ensure_boot_device_pcie_in_bif(bif_path: Path) -> None:
    if not bif_path.exists():
        raise FileNotFoundError(f"Expected BIF file not found: {bif_path}")

    lines = bif_path.read_text().splitlines()
    if any(line.strip() == "boot_device { pcie }" for line in lines):
        return

    # Find id=0x2
    pattern = re.compile(r"^(\s*)id\s*=\s*0x2\s*$")
    for idx, line in enumerate(lines):
        match = pattern.match(line)
        if match:
            lines.insert(idx + 1, f"{match.group(1)}boot_device {{ pcie }}")
            bif_path.write_text("\n".join(lines) + "\n")
            return

    raise ValueError(f"Could not find 'id = 0x2' in BIF file: {bif_path}")


def _generate_top_wrapper_pdi_with_bootgen(impl_dir: Path) -> Path:
    bif_path = impl_dir / "top_wrapper.bif"
    output_pdi = impl_dir / "top_wrapper.pdi"

    _ensure_boot_device_pcie_in_bif(bif_path)
    logger.info("Running bootgen in %s to generate %s",
                impl_dir, output_pdi.name)
    subprocess.run(
        [
            "bootgen",
            "-arch",
            "versal",
            "-image",
            bif_path.name,
            "-w",
            "-o",
            output_pdi.name,
        ],
        cwd=str(impl_dir),
        check=True,
    )

    if not output_pdi.exists():
        raise FileNotFoundError(
            f"Expected bootgen output not found: {output_pdi}")
    return output_pdi


def _environment_with_udev_ld_preload() -> Dict[str, str]:
    """
    Create a dictionary of environment variables (based on the current one),
    that works around a weird issue when running Vivado in a container.

    Details:
    https://adaptivesupport.amd.com/s/question/0D54U00005Sgst2SAB/failed-batch-mode-execution-in-linux-docker-running-under-windows-host?language=en_US
    https://community.flexera.com/t5/InstallAnywhere-Forum/Issues-when-running-Xilinx-tools-or-Other-vendor-tools-in-docker/m-p/245820#M10647
    """
    possible_paths = [
        Path("/lib/x86_64-linux-gnu/libudev.so.1"), Path("/lib64/libudev.so.1")]
    existing_paths = [str(path) for path in possible_paths if path.is_file()]
    env = dict(os.environ)
    if len(existing_paths) > 0:
        env["LD_PRELOAD"] = ":".join(existing_paths)
    return env


def generate_base_pdi_with_aved(config: CommandConfiguration) -> Path:
    aved_dir = config.build_dir / "AVED"

    aved_hw_dir = aved_dir / "hw" / AVED_DESIGN_NAME
    aved_build_dir = aved_hw_dir / "build"
    aved_fpt_dir = aved_hw_dir / "fpt"
    aved_fw_profile_dir = aved_dir / "fw" / "AMC" / \
        "src" / "profiles" / "v80"

    logger.info("Starting AVED base build for %s", config.project_name)
    aved_build_dir.mkdir(parents=True, exist_ok=True)

    static_impl_dir = config.build_dir / "slash.runs" / "impl_1"
    regenerated_top_wrapper_pdi = _generate_top_wrapper_pdi_with_bootgen(
        static_impl_dir)
    _copy_checked(regenerated_top_wrapper_pdi,
                  aved_build_dir / "top_wrapper.pdi")

    files_to_copy = [("build_all.sh", aved_hw_dir), ("profile_hal.h", aved_fw_profile_dir),
                     ("pdi_combine.bif", aved_fpt_dir), (f"{AVED_DESIGN_NAME}.xsa", aved_build_dir)]

    for (file_name, target_dir) in files_to_copy:
        with resources.path("slashkit.resources.aved", file_name) as in_path:
            _copy_checked(in_path, target_dir / file_name)

    logger.info("Running AVED build script in %s", aved_hw_dir)
    subprocess.run(
        ["bash", "build_all.sh"],
        cwd=str(aved_hw_dir),
        env=_clean_cross_build_env(),
        check=True,
    )

    aved_pdi = aved_hw_dir / f"{AVED_DESIGN_NAME}.pdi"
    if not aved_pdi.exists():
        raise FileNotFoundError(f"Expected AVED output not found: {aved_pdi}")
    logger.info("AVED fallback complete. Generated %s", aved_pdi)
    return aved_pdi


def create_build_project(
    config: CommandConfiguration,
    action: Optional[str] = None
) -> None:
    log_path = config.build_dir / "vivado.log"

    with resources.path("slashkit.resources.base.scripts", "create_project.tcl") as tcl_path:
        if not tcl_path.exists():
            raise FileNotFoundError(
                f"create_project.tcl not found: {tcl_path}")
        cmd = [
            config.vivado_bin,
            "-mode",
            "batch",
            "-nojournal",
            "-log",
            str(log_path),
            "-source",
            str(tcl_path),
            "-tclargs",
            config.project_name,
            config.ip_repository
        ]
        if action:
            cmd.append(action)

        cmd.append(str(config.n_jobs))

        subprocess.run(cmd, cwd=str(config.build_dir), check=True,
                       env=_environment_with_udev_ld_preload())


class RM_KIND(Enum):
    SLASH_PROJECT = "slash"
    SERVICE_LAYER = "service_layer"


def _run_rm_build(config: LinkerConfiguration, rm_kind: RM_KIND) -> None:
    if rm_kind == RM_KIND.SLASH_PROJECT:
        # Copy all base IP cores into the ip repository
        config.ip_repository.mkdir(parents=True)
        export_package("slashkit.resources.base.iprepo",
                       config.ip_repository / "slash_base")

        # Copy all user kernels into the ip repository
        for kernel in config.kernels:
            shutil.copytree(kernel.component_xml_path.parent,
                            config.ip_repository / kernel.name)
    elif rm_kind == RM_KIND.SERVICE_LAYER and not config.ip_repository.is_dir():
        raise RuntimeError("The IP repository is missing, the user region has to be built before the service layer.\n"
                           "This is a bug, please report it at https://github.com/Xilinx/SLASH")

    logs_dir = config.build_dir / "logs"
    image_out_dir = config.build_dir / "images"
    rm_work_dir = config.build_dir / f"{rm_kind.value}_rm"

    logs_dir.mkdir(parents=True, exist_ok=True)
    image_out_dir.mkdir(parents=True, exist_ok=True)
    rm_work_dir.mkdir(parents=True, exist_ok=True)

    if rm_kind == RM_KIND.SERVICE_LAYER:
        tcl_name = "service_layer_build.tcl"
        static_shell_dcp_name = "static_shell_service_layer.dcp"
        base_bd_package = "slashkit.resources.static_shell.service_layer"
        base_bd_name = "service_layer.bd"
        log_path = logs_dir / "service_layer_build.log"
    else:
        tcl_name = "slash_project_build.tcl"
        static_shell_dcp_name = "static_shell_slash.dcp"
        base_bd_package = "slashkit.resources.static_shell.slash_base"
        base_bd_name = "slash_base.bd"
        log_path = logs_dir / "slash_project_build.log"

    with ExitStack() as stack:
        tcl_path = stack.enter_context(
            resources.path("slashkit.resources.base.scripts", tcl_name)
        )
        static_shell_dcp_path = stack.enter_context(
            resources.path("slashkit.resources.static_shell",
                           static_shell_dcp_name)
        )
        base_bd_path = stack.enter_context(
            resources.path(base_bd_package, base_bd_name)
        )

        cmd = [
            config.vivado_bin,
            "-mode",
            "batch",
            "-nojournal",
            "-log",
            str(log_path),
            "-source",
            str(tcl_path),
            "-tclargs",
            "--project-name",
            config.project_name,
            "--ip-repo",
            str(config.ip_repository),
            "--static-shell-dcp",
            str(static_shell_dcp_path),
            "--base-bd",
            str(base_bd_path),
            "--linker-results-dir",
            str(config.build_dir),
            "--rm-work-dir",
            str(rm_work_dir),
            "--artifact-out-dir",
            str(image_out_dir),
            "--jobs",
            str(config.n_jobs),
        ]
        if rm_kind == RM_KIND.SLASH_PROJECT:
            util_report_path = config.build_dir / \
                f"report_utilization_{config.project_name}.txt"
            util_report_path.parent.mkdir(parents=True, exist_ok=True)
            cmd.extend(["--util-report-file", str(util_report_path)])

            for path in config.pre_synth_tcls:
                cmd.extend(["--pre-synth-tcl", str(path)])

        if rm_kind == RM_KIND.SERVICE_LAYER:
            opt_post_tcl = stack.enter_context(
                resources.path(
                    "slashkit.resources.base.constraints.service_layer.eth", "service_layer_eth.opt.post.tcl")
            )
            cmd.extend(["--opt-post-tcl", str(opt_post_tcl)])

        subprocess.run(cmd, cwd=str(config.build_dir), check=True,
                       env=_environment_with_udev_ld_preload())

    if rm_kind == RM_KIND.SLASH_PROJECT:
        pdi_out_path = image_out_dir / \
            f"top_i_slash_slash_{config.project_name}_inst_0_partial.pdi"
    else:
        pdi_out_path = image_out_dir / \
            f"top_i_service_layer_service_layer_{config.project_name}_inst_0_partial.pdi"

    if not pdi_out_path.is_file():
        raise FileNotFoundError(
            f"{str(pdi_out_path)} is missing! Check {str(log_path)} for errors!")


def build_service_layer_rm(config: LinkerConfiguration) -> None:
    _run_rm_build(config, RM_KIND.SERVICE_LAYER)


def build_slash_rm(config: LinkerConfiguration) -> None:
    _run_rm_build(config, RM_KIND.SLASH_PROJECT)


def install_static_shell(config: InstallerConfiguration) -> None:
    static_shell_dir = config.out_dir / "static_shell"
    static_shell_dir.mkdir(parents=True, exist_ok=True)

    # Cloning the AVED repository into the build directory
    # We're doing this early so that errors are caught *before* the 10-hour Vivado run!
    subprocess.run([
        "git", "clone",
        "--recurse-submodules",
        "-b", config.aved_ref,
        config.aved_repo,
        config.build_dir / "AVED"
    ], check=True)

    create_build_project(config)

    impl_dir = config.build_dir / "slash.runs" / "impl_1"
    dcp_sources = (
        impl_dir / "top_wrapper_routed_bb.dcp",
        impl_dir / "static_shell_slash.dcp",
        impl_dir / "static_shell_service_layer.dcp",
    )
    for src in dcp_sources:
        if not src.exists():
            raise FileNotFoundError(
                f"Expected install artifact not found: {src}")
    _copy_files(list(dcp_sources), static_shell_dir)

    src_dirs = config.build_dir / "slash.srcs" / "sources_1" / "bd"
    for src_dir in (src_dirs / "slash_base", src_dirs / "service_layer"):
        if not src_dir.is_dir():
            raise FileNotFoundError(
                f"Expected install BD directory not found: {src_dir}")
        _copy_tree(src_dir, static_shell_dir)

    aved_pdi_path = generate_base_pdi_with_aved(config)
    if not aved_pdi_path.exists():
        raise FileNotFoundError(
            f"Expected AVED PDI not found in results/base: {aved_pdi_path}")
    _copy_files([aved_pdi_path], static_shell_dir)

    def add_init_files(path: Path):
        (path / "__init__.py").touch()
        for sub_path in path.iterdir():
            if not sub_path.is_dir():
                continue
            add_init_files(sub_path)
    add_init_files(static_shell_dir)


def generate_util_report(config: CommandConfiguration) -> None:
    report_path = config.build_dir / \
        f"report_utilization_{config.project_name}.txt"
    xml_path = config.build_dir / \
        f"report_utilization_{config.project_name}.xml"
    logger.info("Generating utilization report XML for project %s",
                config.project_name)
    logger.info("Utilization report input: %s", report_path)
    logger.info("Utilization report output: %s", xml_path)
    if not report_path.exists():
        raise FileNotFoundError(report_path)
    convert_report_utilization_to_xml(report_path, xml_path)
    logger.info("Utilization report XML generation complete for %s",
                config.project_name)
