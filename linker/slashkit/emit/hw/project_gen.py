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
from typing import List, Optional, Dict
from contextlib import ExitStack

from slashkit.emit.metadata.report_util import convert_report_utilization_to_xml
from slashkit.emit.render import export_package
from slashkit.core.command_config import (
    LinkerConfiguration,
    InstallerConfiguration,
    CommandConfiguration,
    ShellType,
)
from slashkit.emit.metadata.timing_freq import (
    require_static_shell_timing_or_confirm,
    read_system_map_clock_hz,
)
from slashkit.core.launcher import (
    TASK_AVED,
    TASK_BOOTGEN,
    TASK_RM_SERVICE_LAYER,
    TASK_RM_SLASH,
    TASK_STATIC_SHELL,
    TASK_STATIC_SHELL_COMPUTE,
    run_tool,
)

logger = logging.getLogger(__name__)

AVED_DESIGN_NAME = "amd_v80_gen5x8_25.1"
_RP1_RESOURCE_PACKAGE = "slashkit.resources.aved"
_RP1_RESOURCE_DIRECTORY = "rp1"
_RP1_REQUIRED_RESOURCES = (
    "CMakeLists.txt",
    "build-rp1.sh",
    "config/rp1_platform_config.h.in",
    "include/slash/uapi/rp1_protocol.h",
    "tools/generate_platform_config.py",
)


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
    # bootgen resolves the file paths written inside the BIF against its own
    # working directory, not against the BIF, so impl_dir is part of the
    # contract rather than a convenience. run_tool carries it across to the
    # execution host in SLASH_TOOL_CWD; making the arguments below absolute
    # would not help, because the paths inside the BIF stay relative.
    run_tool(
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
        task=TASK_BOOTGEN,
        cwd=impl_dir,
    )

    if not output_pdi.exists():
        raise FileNotFoundError(
            f"Expected bootgen output not found: {output_pdi}")
    return output_pdi


def _first_existing(candidates: List[str]) -> Optional[str]:
    """Return the first path in candidates that exists, or None."""
    for candidate in candidates:
        if Path(candidate).is_file():
            return candidate
    return None


def _rp1_resource_root():
    try:
        root = resources.files(_RP1_RESOURCE_PACKAGE)
    except ModuleNotFoundError as error:
        raise FileNotFoundError(
            "Required packaged RP1 firmware resources are unavailable; "
            "reinstall slashkit from a complete wheel or source archive"
        ) from error

    root = root.joinpath(_RP1_RESOURCE_DIRECTORY)
    if not root.is_dir():
        raise FileNotFoundError(
            "Required packaged RP1 firmware resource tree is missing: "
            f"{_RP1_RESOURCE_DIRECTORY}"
        )

    missing = [
        name for name in _RP1_REQUIRED_RESOURCES
        if not root.joinpath(*name.split("/")).is_file()
    ]
    if missing:
        raise FileNotFoundError(
            "Required packaged RP1 firmware resources are missing: "
            + ", ".join(missing)
        )
    return root


def _copy_rp1_resource_tree(source, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for entry in source.iterdir():
        if entry.is_dir() and (
            entry.name == "build"
            or entry.name.startswith("build-")
            or entry.name == "__pycache__"
        ):
            continue

        target = destination / entry.name
        if entry.is_dir():
            _copy_rp1_resource_tree(entry, target)
        elif entry.is_file():
            with resources.as_file(entry) as source_path:
                shutil.copy2(source_path, target)


def _copy_rp1_sources_to_aved(aved_dir: Path) -> None:
    rp1_resources = _rp1_resource_root()
    rp1_dest_dir = aved_dir / "fw" / "RP1"
    if rp1_dest_dir.exists():
        shutil.rmtree(rp1_dest_dir)
    _copy_rp1_resource_tree(rp1_resources, rp1_dest_dir)


def _add_init_files(path: Path) -> None:
    """Make every installed artifact directory importable as package data."""
    (path / "__init__.py").touch()
    for sub_path in path.iterdir():
        if not sub_path.is_dir():
            continue
        _add_init_files(sub_path)


def _environment_with_udev_ld_preload() -> Dict[str, str]:
    """
    Create a dictionary of environment variables (based on the current one),
    that works around a weird issue when running Vivado in a container.

    Details:
    https://adaptivesupport.amd.com/s/question/0D54U00005Sgst2SAB/failed-batch-mode-execution-in-linux-docker-running-under-windows-host?language=en_US
    https://community.flexera.com/t5/InstallAnywhere-Forum/Issues-when-running-Xilinx-tools-or-Other-vendor-tools-in-docker/m-p/245820#M10647

    The preload is inherited by the bundled MicroBlaze cross-compiler that builds
    the DDRMC firmware (phy_ddrmc.elf) under a restricted library path, so a
    forced library must bring its own dependencies. On Ubuntu 24.04 libudev.so.1
    needs libcap.so.2, so preloading libudev alone makes that compiler abort
    ("libcap.so.2: cannot open shared object file") and silently drops the ELF.
    Preload libcap alongside it. On 22.04 libudev has no libcap dependency, so
    preloading it there has no effect.
    """
    env = dict(os.environ)
    libudev = _first_existing(
        ["/lib/x86_64-linux-gnu/libudev.so.1", "/lib64/libudev.so.1"])
    if libudev is None:
        return env
    preload = [libudev]
    libcap = _first_existing(
        ["/lib/x86_64-linux-gnu/libcap.so.2", "/lib64/libcap.so.2"])
    if libcap is not None:
        preload.append(libcap)
    env["LD_PRELOAD"] = ":".join(preload)
    return env


def generate_base_pdi_with_aved(config: CommandConfiguration) -> tuple[Path, Path]:
    aved_dir = config.build_dir / "AVED"

    aved_hw_dir = aved_dir / "hw" / AVED_DESIGN_NAME
    aved_build_dir = aved_hw_dir / "build"
    aved_fpt_dir = aved_hw_dir / "fpt"
    aved_fw_profile_dir = aved_dir / "fw" / "AMC" / \
        "src" / "profiles" / "v80"

    _copy_rp1_sources_to_aved(aved_dir)

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

    # build_all.sh and the four files staged above all live in the AVED clone,
    # so offloading this step needs nothing installed on the execution host
    # beyond a Vitis toolchain. It does need more of the base system than the
    # other steps (cmake, make, git, python3); see scripts/lsf/README.md.
    logger.info("Running AVED build script in %s", aved_hw_dir)
    run_tool(
        ["bash", "build_all.sh"],
        task=TASK_AVED,
        cwd=aved_hw_dir,
        env=_clean_cross_build_env(),
    )

    aved_pdi = aved_hw_dir / f"{AVED_DESIGN_NAME}.pdi"
    if not aved_pdi.exists():
        raise FileNotFoundError(f"Expected AVED output not found: {aved_pdi}")

    aved_nofpt_pdi = aved_build_dir / f"{AVED_DESIGN_NAME}_nofpt.pdi"
    if not aved_nofpt_pdi.exists():
        raise FileNotFoundError(
            f"Expected AVED nofpt PDI not found: {aved_nofpt_pdi}")

    logger.info("AVED fallback complete. Generated %s", aved_pdi)
    return aved_pdi, aved_nofpt_pdi


def _compute_build_id_env() -> Dict[str, str]:
    """
    Derive the shell build-ID constants from the git commit of the SLASH source
    tree and return them as environment variables consumed by create_project.tcl.

    Encoding (60-bit hash + shell type + dirty), split across two 32-bit GPIO
    channels. The 60 hash bits are the top 60 bits of the SHA-1
    (bits[159:100]), so the value starts with the commit's GitHub short hash:
      - SLASH_BUILD_ID_LO = low 32 bits of the 60-bit window
      - SLASH_BUILD_ID_HI = high 28 bits in bits[27:0], shell type in bit[28]
        (0 = service, 1 = compute), bits[30:29] reserved, dirty flag in bit[31]

    Bit[28] is left clear here: each shell's create_project.tcl forces it to its
    own value, so the bit is owned by the design that it describes and stays
    correct even when the Tcl is run outside this driver.

    Falls back to hash 0 with the dirty bit set when git information is
    unavailable (e.g. building from an exported tarball, not a git checkout).
    """
    repo_dir = Path(__file__).resolve().parents[3]

    def _git(*args: str) -> Optional[str]:
        try:
            out = subprocess.run(
                ["git", "-C", str(repo_dir), *args],
                check=True, capture_output=True, text=True,
            )
            return out.stdout.strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            return None

    sha = _git("rev-parse", "HEAD")
    if sha is None:
        logger.warning("Not a git checkout; shell build-ID will be 0 (dirty).")
        return {"SLASH_BUILD_ID_LO": "0x0", "SLASH_BUILD_ID_HI": "0x80000000"}

    # `git diff --quiet` exits non-zero when the working tree has changes.
    dirty = subprocess.run(
        ["git", "-C", str(repo_dir), "diff", "--quiet"]
    ).returncode != 0

    sha_int = int(sha, 16) >> 100  # keep the top 60 bits (first 15 hex chars)
    lo = sha_int & 0xFFFFFFFF
    hi = (sha_int >> 32) & 0x0FFFFFFF
    if dirty:
        hi |= 0x80000000

    logger.info("Shell build-ID: commit %s%s",
                sha[:14], " (dirty)" if dirty else "")
    return {"SLASH_BUILD_ID_LO": f"0x{lo:08x}", "SLASH_BUILD_ID_HI": f"0x{hi:08x}"}


def create_build_project(
    config: CommandConfiguration,
    action: Optional[str] = None
) -> None:
    log_path = config.build_dir / "vivado.log"

    with resources.path(
        "slashkit.resources.base.service.scripts", "create_project.tcl"
    ) as tcl_path:
        if not tcl_path.exists():
            raise FileNotFoundError(
                f"create_project.tcl not found: {tcl_path}")
        cmd = [
            str(config.vivado_bin),
            "-mode",
            "batch",
            "-nojournal",
            "-log",
            str(log_path),
            "-source",
            str(tcl_path),
            "-tclargs",
            config.project_name,
            str(config.ip_repository),
        ]
        if action:
            cmd.append(action)

        cmd.append(str(config.n_jobs))

        env = _environment_with_udev_ld_preload()
        # Computed here, from this checkout, and carried across to wherever the
        # run happens: an execution host may see a different git tree, or none.
        env.update(_compute_build_id_env())

        run_tool(cmd, task=TASK_STATIC_SHELL,
                 cwd=config.build_dir, env=env)


def create_build_project_compute(
    config: CommandConfiguration, action: Optional[str] = None
) -> None:
    log_path = config.build_dir / "vivado_compute.log"

    with resources.path(
        "slashkit.resources.base.compute.scripts", "create_project.tcl"
    ) as tcl_path:
        if not tcl_path.exists():
            raise FileNotFoundError(
                f"create_project.tcl not found: {tcl_path}"
            )
        cmd = [
            str(config.vivado_bin),
            "-mode",
            "batch",
            "-nojournal",
            "-log",
            str(log_path),
            "-source",
            str(tcl_path),
            "-tclargs",
            config.project_name,
            str(config.ip_repository),
        ]
        if action:
            cmd.append(action)
        cmd.append(str(config.n_jobs))

        env = _environment_with_udev_ld_preload()
        # Same as the service path: derived from this checkout, not from
        # whatever tree the execution host happens to see.
        env.update(_compute_build_id_env())

        run_tool(
            cmd,
            task=TASK_STATIC_SHELL_COMPUTE,
            cwd=config.build_dir,
            env=env,
        )


class RM_KIND(Enum):
    SLASH_PROJECT = "slash"
    SERVICE_LAYER = "service_layer"


def _resolve_target_user_clock_hz(config: LinkerConfiguration) -> Optional[int]:
    # The resolved target user-clock frequency has already been written to
    # system_map.xml by generate_tcl(), so read it back rather than re-deriving
    # it, keeping a single source of truth (see resolve_system_map_clock).
    system_map_path = config.build_dir / "system_map.xml"
    target_hz = read_system_map_clock_hz(system_map_path)
    if target_hz is None or target_hz <= 0:
        logger.warning(
            "No valid target ClockFrequency in %s; skipping user-clock constraint",
            system_map_path,
        )
        return None
    return target_hz


def _generate_user_clock_xdc(
    config: LinkerConfiguration, target_hz: int
) -> Path:
    # Turn the resolved target user-clock frequency into an actual Vivado timing
    # constraint for the reconfigurable-module implementation.
    period_ns = 1e9 / float(target_hz)
    xdc_path = config.build_dir / "user_clock.xdc"
    # Defining a clock at the RM's user_clk port overrides, downstream of that
    # point, the clock the abstract shell propagates in. That clock is
    # clkout1_primitive_2, auto-derived by Vivado from the clocking wizard's
    # MMCME5 CLKOUT0 and therefore pinned to the 200 MHz the static shell was
    # built with -- it does not follow the wizard's runtime DRP reprogramming,
    # so it has to be overridden here rather than trusted.
    #
    # Overriding at the RM boundary rather than at the MMCM output keeps the
    # signed-off static timing untouched, and needs no reference to a
    # static-region hierarchy path, which would differ between the service and
    # compute shells.
    #
    # Known limitation: this leaves two clock objects on one physical net --
    # user_clk at the requested period inside the RM, and the shell's
    # clkout1_primitive_2 at 200 MHz outside it. A handful of static-side flops
    # do drive into the RM's clock domain (2 endpoints on 00_axilite), and
    # Vivado times those crossings against the beat frequency of the two
    # periods rather than treating them as the same clock: at 250 MHz the
    # 4 ns / 5 ns pair yields a bogus 1 ns requirement. Both are physically the
    # same net and run at the same rate once the wizard is reprogrammed, so
    # those paths are not really failing. Measured cost is under 1 MHz on both
    # shells, and it is latent at the default -- at 200 MHz the two clocks share
    # a 5 ns grid and the crossings pass with positive slack.
    constraint = (
        f"create_clock -name user_clk -period {period_ns:.6f}"
        " [get_ports user_clk]\n"
    )
    xdc_path.write_text(constraint, encoding="utf-8")
    logger.info("Wrote user-clock constraint (%.6f ns / %d Hz) to %s",
                period_ns, target_hz, xdc_path)
    return xdc_path


def _run_rm_build(config: LinkerConfiguration, rm_kind: RM_KIND) -> None:
    # Copy all base IP cores into the ip repository
    config.ip_repository.mkdir(parents=True, exist_ok=True)
    base_ip_repository = config.ip_repository / "slash_base"
    if not base_ip_repository.exists():
        export_package("slashkit.resources.base.common.iprepo",
                       base_ip_repository)

    if rm_kind == RM_KIND.SLASH_PROJECT:
        # Copy all user kernels into the ip repository
        for kernel in config.kernels:
            shutil.copytree(
                kernel.component_xml_path.parent, config.ip_repository / kernel.name
            )

    logs_dir = config.build_dir / "logs"
    image_out_dir = config.build_dir / "images"
    rm_work_dir = config.build_dir / f"{rm_kind.value}_rm"

    logs_dir.mkdir(parents=True, exist_ok=True)
    image_out_dir.mkdir(parents=True, exist_ok=True)
    rm_work_dir.mkdir(parents=True, exist_ok=True)

    if rm_kind == RM_KIND.SERVICE_LAYER:
        tcl_package = "slashkit.resources.base.service.scripts"
        tcl_name = "service_layer_build.tcl"
        static_shell_package = "slashkit.resources.static_shell"
        static_shell_dcp_name = "static_shell_service_layer.dcp"
        base_bd_package = "slashkit.resources.static_shell.service_layer"
        base_bd_name = "service_layer.bd"
        log_path = logs_dir / "service_layer_build.log"
    elif config.shell_type == ShellType.COMPUTE:
        tcl_package = "slashkit.resources.base.common.scripts"
        tcl_name = "slash_project_build.tcl"
        static_shell_package = "slashkit.resources.static_shell_compute"
        static_shell_dcp_name = "static_shell_slash.dcp"
        base_bd_package = "slashkit.resources.static_shell_compute.slash_base"
        base_bd_name = "slash_base.bd"
        log_path = logs_dir / "slash_project_build.log"
    else:  # SERVICE slash RM
        tcl_package = "slashkit.resources.base.common.scripts"
        tcl_name = "slash_project_build.tcl"
        static_shell_package = "slashkit.resources.static_shell"
        static_shell_dcp_name = "static_shell_slash.dcp"
        base_bd_package = "slashkit.resources.static_shell.slash_base"
        base_bd_name = "slash_base.bd"
        log_path = logs_dir / "slash_project_build.log"

    with ExitStack() as stack:
        tcl_path = stack.enter_context(resources.path(tcl_package, tcl_name))
        static_shell_dcp_path = stack.enter_context(
            resources.path(static_shell_package, static_shell_dcp_name)
        )
        base_bd_path = stack.enter_context(
            resources.path(base_bd_package, base_bd_name)
        )

        cmd = [
            str(config.vivado_bin),
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
            util_report_path = (
                config.build_dir /
                f"report_utilization_{config.project_name}.txt"
            )
            util_report_path.parent.mkdir(parents=True, exist_ok=True)
            cmd.extend(["--util-report-file", str(util_report_path)])

            for path in config.pre_synth_tcls:
                cmd.extend(["--pre-synth-tcl", str(path)])

            # Both halves of the user-clock chain come from the same resolved
            # target: --user-clock-hz retargets the RM block design (and with
            # it the module's synthesis constraints), --user-clock-xdc
            # constrains the implementation run.
            target_hz = _resolve_target_user_clock_hz(config)
            if target_hz is not None:
                cmd.extend(["--user-clock-hz", str(target_hz)])
                user_clock_xdc = _generate_user_clock_xdc(config, target_hz)
                cmd.extend(["--user-clock-xdc", str(user_clock_xdc)])

        if rm_kind == RM_KIND.SERVICE_LAYER:
            opt_post_tcl = stack.enter_context(
                resources.path(
                    "slashkit.resources.base.service.constraints.service_layer.eth",
                    "service_layer_eth.opt.post.tcl",
                )
            )
            cmd.extend(["--opt-post-tcl", str(opt_post_tcl)])

        # Distinct task kinds: a single `slashkit link` runs both RM builds, so
        # the launcher cannot tell them apart from a per-invocation override.
        task = (TASK_RM_SLASH if rm_kind == RM_KIND.SLASH_PROJECT
                else TASK_RM_SERVICE_LAYER)
        run_tool(cmd, task=task, cwd=config.build_dir,
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


def _install_static_shell_base(config: InstallerConfiguration, static_shell_dir: Path) -> None:
    """Build and install implementation artifacts for the selected shell."""
    static_shell_dir.mkdir(parents=True, exist_ok=True)

    aved_dir = config.build_dir / "AVED"
    if not aved_dir.exists():
        # Clone AVED early so that errors are caught before the multi-hour
        # implementation run. Stays local even when the tool steps are
        # offloaded: this needs network egress, which compute nodes typically
        # do not have.
        subprocess.run(
            [
                "git",
                "clone",
                "--recurse-submodules",
                "-b",
                config.aved_ref,
                config.aved_repo,
                aved_dir,
            ],
            check=True,
        )

    if config.shell_type == ShellType.SERVICE:
        create_build_project(config)
    else:
        create_build_project_compute(config)

    require_static_shell_timing_or_confirm(
        build_dir=config.build_dir,
        project_name=config.project_name,
        ignore_failure=config.ignore_timing_failure,
        noninteractive=config.noninteractive,
    )

    impl_dir = config.build_dir / "slash.runs" / "impl_1"
    bd_src_dir = config.build_dir / "slash.srcs" / "sources_1" / "bd"

    if config.shell_type == ShellType.SERVICE:
        # debug_nets.ltx is auto-emitted by Vivado because the service base shell
        # instantiates the debug hub. It is the full debug probe file
        # (FULL_PROBES.FILE) that must be loaded before a user region's partial
        # probe file in the Vivado Hardware Manager.
        install_sources = (
            impl_dir / "static_shell_slash.dcp",
            impl_dir / "static_shell_service_layer.dcp",
            impl_dir / "debug_nets.ltx",
        )
        for src in install_sources:
            if not src.exists():
                raise FileNotFoundError(
                    f"Expected install artifact not found: {src}")
        _copy_files(list(install_sources), static_shell_dir)

        for bd_name in ("slash_base", "service_layer"):
            src_dir = bd_src_dir / bd_name
            if not src_dir.is_dir():
                raise FileNotFoundError(
                    f"Expected install BD directory not found: {src_dir}"
                )
            _copy_tree(src_dir, static_shell_dir)

    else:  # ShellType.COMPUTE
        # debug_nets.ltx is auto-emitted by Vivado because the compute base shell
        # instantiates the debug hub. It is the full debug probe file
        # (FULL_PROBES.FILE) that must be loaded before a user region's partial
        # probe file in the Vivado Hardware Manager.
        install_sources = (
            impl_dir / "static_shell_slash.dcp",
            impl_dir / "debug_nets.ltx",
        )
        for src in install_sources:
            if not src.exists():
                raise FileNotFoundError(
                    f"Expected install artifact not found: {src}")
        _copy_files(list(install_sources), static_shell_dir)

        src_dir = bd_src_dir / "slash_base"
        if not src_dir.is_dir():
            raise FileNotFoundError(
                f"Expected install BD directory not found: {src_dir}")
        _copy_tree(src_dir, static_shell_dir)

    _add_init_files(static_shell_dir)


def _install_static_shell_firmware(config: InstallerConfiguration, static_shell_dir: Path) -> None:
    """Build AMC and RP1 firmware and install both packaged static-shell PDIs."""
    static_shell_dir.mkdir(parents=True, exist_ok=True)
    if not (config.build_dir / "AVED").is_dir():
        raise FileNotFoundError(
            f"Expected AVED checkout not found in build directory: {config.build_dir / 'AVED'}")

    aved_pdi_path, aved_nofpt_pdi_path = generate_base_pdi_with_aved(config)
    for pdi_path in (aved_pdi_path, aved_nofpt_pdi_path):
        if not pdi_path.exists():
            raise FileNotFoundError(
                f"Expected AVED PDI not found in results/base: {pdi_path}")
    _copy_files([aved_pdi_path, aved_nofpt_pdi_path], static_shell_dir)

    _add_init_files(static_shell_dir)


def _install_static_shell_rp1_firmware(config: InstallerConfiguration,
                                       static_shell_dir: Path) -> None:
    """Rebuild RP1 and repack it with existing base-shell and AMC artifacts."""
    static_shell_dir.mkdir(parents=True, exist_ok=True)

    aved_dir = config.build_dir / "AVED"
    aved_hw_dir = aved_dir / "hw" / AVED_DESIGN_NAME
    aved_build_dir = aved_hw_dir / "build"
    aved_fpt_dir = aved_hw_dir / "fpt"
    aved_fw_dir = aved_dir / "fw"

    _copy_rp1_sources_to_aved(aved_dir)

    required = (
        aved_build_dir / "top_wrapper.pdi",
        aved_build_dir / f"{AVED_DESIGN_NAME}.xsa",
        aved_build_dir / "amc.elf",
        aved_build_dir / "fpt.bin",
        aved_fpt_dir / "pdi_combine.bif",
        aved_fpt_dir / "fpt_pdi_gen.py",
    )
    for path in required:
        if not path.exists():
            raise FileNotFoundError(
                f"RP1-only firmware repack requires existing artifact: {path}")

    rp1_dir = aved_fw_dir / "RP1"
    logger.info("Rebuilding RP1 firmware in %s", rp1_dir)
    rp1_env = _clean_cross_build_env()
    rp1_env["XSA"] = str(
        aved_build_dir / f"{AVED_DESIGN_NAME}.xsa")
    # Tagged as AVED rather than given a kind of its own: this is the same
    # cross-compile the AVED step already runs -- build_all.sh calls this very
    # script -- so it wants the same reservation and the same node.
    run_tool(
        ["bash", "build-rp1.sh"],
        task=TASK_AVED,
        cwd=rp1_dir,
        env=rp1_env,
    )
    _copy_checked(rp1_dir / "build" / "rp1.elf", aved_build_dir / "rp1.elf")

    nofpt_pdi = aved_build_dir / f"{AVED_DESIGN_NAME}_nofpt.pdi"
    logger.info(
        "Repacking %s with existing AMC/FPT and rebuilt RP1", nofpt_pdi.name)
    # As in _generate_top_wrapper_pdi_with_bootgen, the paths inside the BIF are
    # relative, so aved_hw_dir is part of the contract and travels in
    # SLASH_TOOL_CWD.
    run_tool(
        [
            "bootgen",
            "-arch",
            "versal",
            "-image",
            str(aved_fpt_dir / "pdi_combine.bif"),
            "-w",
            "-o",
            str(nofpt_pdi),
        ],
        task=TASK_BOOTGEN,
        cwd=aved_hw_dir,
        env=_clean_cross_build_env(),
    )

    aved_pdi = aved_hw_dir / f"{AVED_DESIGN_NAME}.pdi"
    # Deliberately local: fpt_pdi_gen.py is plain Python that splices two files
    # together, with no vendor toolchain behind it. Offloading it would buy a
    # queue wait and nothing else.
    subprocess.run(
        [
            str(aved_fpt_dir / "fpt_pdi_gen.py"),
            "--fpt",
            str(aved_build_dir / "fpt.bin"),
            "--pdi",
            str(nofpt_pdi),
            "--output",
            str(aved_pdi),
        ],
        cwd=str(aved_hw_dir),
        env=_clean_cross_build_env(),
        check=True,
    )
    for pdi_path in (aved_pdi, nofpt_pdi):
        if not pdi_path.is_file():
            raise FileNotFoundError(
                f"Expected RP1 repack output not found: {pdi_path}")
    _copy_files([aved_pdi, nofpt_pdi], static_shell_dir)
    _add_init_files(static_shell_dir)


def install_static_shell(config: InstallerConfiguration) -> None:
    """Run the requested install stage for the selected static-shell package."""
    static_shell_dir = config.out_dir / (
        "static_shell"
        if config.shell_type == ShellType.SERVICE
        else "static_shell_compute"
    )
    if config.stage in ("all", "base-shell"):
        _install_static_shell_base(config, static_shell_dir)
    if config.stage in ("all", "firmware"):
        _install_static_shell_firmware(config, static_shell_dir)
    if config.stage == "rp1-firmware":
        _install_static_shell_rp1_firmware(config, static_shell_dir)


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
