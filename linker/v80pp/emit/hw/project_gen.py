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

from enum import Enum
from pathlib import Path
import logging
import re
import shutil
import subprocess
import importlib.resources as resources
from typing import Optional
from v80pp.emit.metadata.report_util import convert_report_utilization_to_xml
from v80pp.core.command_config import LinkerConfiguration, InstallerConfiguration, CommandConfiguration

logger = logging.getLogger(__name__)

AVED_DESIGN_NAME = "amd_v80_gen5x8_25.1"


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


def generate_base_pdi_with_aved(config: CommandConfiguration) -> Path:
    aved_dir = config.build_dir / "AVED"

    with resources.path("v80pp.resources.submodules", "AVED") as aved_reference_dir:
        if not aved_reference_dir.is_dir():
            raise FileNotFoundError(aved_reference_dir)
        if aved_dir.is_dir():
            shutil.rmtree(aved_dir)
        shutil.copytree(aved_reference_dir, aved_dir)

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
        with resources.path("v80pp.resources.aved", file_name) as in_path:
            _copy_checked(in_path, target_dir / file_name)

    logger.info("Running AVED build script in %s", aved_hw_dir)
    subprocess.run(["bash", "build_all.sh"], cwd=str(aved_hw_dir), check=True)

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

    with resources.path("v80pp.resources.base.scripts", "create_project.tcl") as tcl_path:
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

        subprocess.run(cmd, cwd=str(config.build_dir), check=True)


class RM_KIND(Enum):
    SLASH_PROJECT = "slash"
    SERVICE_LAYER = "service_layer"


def _run_rm_build(config: LinkerConfiguration, rm_kind: RM_KIND) -> None:
    if rm_kind == RM_KIND.SLASH_PROJECT:
        # Copy all kernels into the ip repository
        for kernel in config.kernels:
            shutil.copytree(kernel.component_xml_path.parent,
                            config.ip_repository / kernel.name)

    logs_dir = config.build_dir / "logs"
    image_out_dir = config.build_dir / "images"
    rm_work_dir = config.build_dir / f"{rm_kind.value}_rm"

    logs_dir.mkdir(parents=True, exist_ok=True)
    image_out_dir.mkdir(parents=True, exist_ok=True)
    rm_work_dir.mkdir(parents=True, exist_ok=True)

    if rm_kind == RM_KIND.SERVICE_LAYER:
        tcl_name = "service_layer_build.tcl"
        log_path = logs_dir / "service_layer_build.log"
    else:
        tcl_name = "slash_project_build.tcl"
        log_path = logs_dir / "slash_project_build.log"

    with resources.path("v80pp.resources.base.scripts", tcl_name) as tcl_path, resources.path("v80pp", "resources") as resources_path:
        if not tcl_path.exists():
            raise FileNotFoundError(f"RM build Tcl not found: {tcl_path}")
        if not resources_path.is_dir():
            raise FileNotFoundError(resources_path)

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
            "--resources-dir",
            str(resources_path),
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

        subprocess.run(cmd, cwd=str(config.build_dir), check=True)

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


def install_abstract_shell(config: InstallerConfiguration) -> None:
    # Assuming that this file is v80pp/emit/hw/project_gen.py
    resources_dir = (Path(__file__).parent.parent.parent /
                     "resources").resolve()
    if not resources_dir.is_dir():
        raise FileNotFoundError(
            f"{resources_dir}. Has v80pp/emit/hw/project_gen.py been renamed?")

    abstract_shell_dir = resources_dir / "abstract_shell"
    abstract_shell_dir.mkdir(parents=True, exist_ok=True)

    create_build_project(config)

    impl_dir = config.build_dir / "slash.runs" / "impl_1"
    dcp_sources = (
        impl_dir / "top_wrapper_routed_bb.dcp",
        impl_dir / "abs_shell_slash.dcp",
        impl_dir / "abs_shell_service_layer.dcp",
    )
    for src in dcp_sources:
        if not src.exists():
            raise FileNotFoundError(
                f"Expected install artifact not found: {src}")
    _copy_files(list(dcp_sources), abstract_shell_dir)

    src_dirs = config.build_dir / "slash.srcs" / "sources_1" / "bd"
    for src_dir in (src_dirs / "slash_base", src_dirs / "service_layer"):
        if not src_dir.is_dir():
            raise FileNotFoundError(
                f"Expected install BD directory not found: {src_dir}")
        _copy_tree(src_dir, abstract_shell_dir)

    aved_pdi_path = generate_base_pdi_with_aved(config)
    if not aved_pdi_path.exists():
        raise FileNotFoundError(
            f"Expected AVED PDI not found in results/base: {aved_pdi_path}")
    _copy_files([aved_pdi_path], abstract_shell_dir)


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
