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
from enum import Enum
from pathlib import Path
from typing import Dict, List, Optional
import re
import os
import shutil
import argparse
import sys

from core.bd_ports import load_bd_ports_from_file, BlockDesignPorts
from core.kernel import Kernel, KernelInstance
from core.connectivity import StreamConnect, ConnectivityConfig
from parser.config_parser import parse_connectivity_file, apply_config_to_instances
from parser.component_parser import parse_component_xml
from emit.hls_meta import infer_hls_json_from_component_xml


class Platform(Enum):
    HARDWARE = "hw"
    SIMULATION = "sim"
    EMULATION = "emu"


def _find_vitis_include() -> Path:
    env_candidates = [
        os.environ.get("XILINX_VITIS"),
        os.environ.get("VITIS_HOME"),
        os.environ.get("VITIS"),
    ]
    for base in env_candidates:
        if not base:
            continue
        cand = Path(base) / "include"
        if cand.exists():
            return cand

    vitis_bin = shutil.which("vitis")
    if vitis_bin:
        return Path(vitis_bin).resolve().parents[1] / "include"

    raise FileNotFoundError(
        "Could not locate Vitis include path. Set XILINX_VITIS/VITIS_HOME "
        "or ensure 'vitis' is on PATH."
    )


class CommandConfiguration(object):
    @classmethod
    def populate_argument_parser(cls, ap: argparse.ArgumentParser):
        ap.formatter_class = argparse.RawTextHelpFormatter
        ap.add_argument("--vivado", required=False, type=Path, default=None,
                        help="Vivado binary to use for linking. If not given, it will be derived from PATH.")
        ap.add_argument("--jobs", required=False, type=int, default=8,
                        help="Number of parallel jobs for Vivado runs.")

    def __init__(self, args: argparse.Namespace):
        self._args = args

        def valid_resource_directory(candidate: Path) -> bool:
            return (candidate / "slash.tcl").is_file()

        def find_resource_directory() -> Path:
            # Find the resource directory
            env_resource_dir = os.getenv("V80PP_RESOURCE_DIR")
            if env_resource_dir is not None:
                env_resource_dir = Path(
                    env_resource_dir).expanduser().resolve()
                if valid_resource_directory(env_resource_dir):
                    return env_resource_dir
                else:
                    raise RuntimeError(
                        f"The requested resource directory in V80PP_RESOURCE_DIR='{env_resource_dir}' does not exist!")

            # Assumes that this class is defined in linker/src/core/linker_config.py
            repo_root_dir = Path(__file__).parent.parent.parent.resolve()

            candidates = [
                repo_root_dir / "resources",
                Path("~/.local/share/v80++/").expanduser().resolve(),
                Path("/usr/local/share/v80++/"),
                Path("/usr/share/v80++/")
            ]
            resource_dir = next(
                filter(valid_resource_directory, candidates), None)
            if resource_dir is None:
                raise RuntimeError(
                    f"Unable to find the resource directory! Evaluated candidates are: {[str(path) for path in candidates]}")
            else:
                return resource_dir

        self._resource_dir = find_resource_directory()

        # Resolve, if necessary find, and verify the Vivado binary
        self._vivado_bin: Path = args.vivado if args.vivado is not None else Path(
            shutil.which("vivado"))
        self._vivado_bin = self._vivado_bin.expanduser().resolve()
        if not self._vivado_bin.is_file():
            raise FileNotFoundError(self._vivado_bin)

        # Misc. arguments
        self._n_jobs: int = args.jobs

    @property
    def input_arguments(self) -> argparse.Namespace:
        return self._args

    @property
    def project_name(self) -> str:
        raise NotImplementedError()

    @property
    def build_dir(self) -> Path:
        raise NotImplementedError()

    @property
    def resources_dir(self) -> Path:
        return self._resource_dir

    @property
    def abstract_shell_dir(self) -> Path:
        return self.resources_dir / "abstract_shell"

    @property
    def ip_repository(self) -> Path:
        return self.build_dir / "iprepo"

    @property
    def vivado_bin(self) -> Path:
        return self._vivado_bin

    @property
    def n_jobs(self) -> int:
        return self._n_jobs


LINK_HELP_EPILOG = f"""
Typical Workflow:
  1. Create a connectivity configuration file (--config) defining kernel
     instances and their connections
  2. Specify one or more kernel IP cores via IP-XACT component.xml files (--kernels)
  3. Run the linker to produce a VBIN archive (--out)
  4. The VBIN archive contains all metadata and artifacts needed to execute
     the design on the target platform

Connectivity Configuration Format:
  The configuration file uses an INI-like format with the following sections:

  [connectivity]  - Define kernel instances and connections
    nk=<kernel>:<count>[:<names>]
      Example: nk=vadd:2:vadd_0.vadd_1
      Creates <count> instances of <kernel>. Names are auto-generated if omitted.

    stream_connect=<src_inst>.<src_port>:<dst_inst>.<dst_port>
      Examples:
        stream_connect=dma_in_0.axis_out:passthrough_0.axis_in
        stream_connect=traffic_producer_0.axis_out:eth_0.tx0
      Connects AXI-Stream ports between kernel instances and/or ethernet ports.

    sp=<inst>.<port>:<memory_target>
      Example: sp=vadd_0.m_axi_gmem0:HBM0
      Maps AXI4-Full memory ports to memory banks (HBM0-31, DDR0-3, MEM, HOST).

  [clock]  - Specify per-kernel clock frequencies (can be repeated)
    krnl=<instance_name>
    freqhz=<frequency_in_hz>
      Example: krnl=vadd_0
               freqhz=400000000

  [network]  - Enable Ethernet interfaces
    eth_<idx>=<0|1>
      Example: eth_0=1

  [user_region]  - Custom TCL scripts
    pre_synth=<path_to_tcl>
      Example: pre_synth=custom_constraints.tcl

  [debug]  - Debug net visibility
    net=<instance>.<port>
      Example: net=vadd_0.axis_out

  Lines starting with '#' or ';' are treated as comments.

Platform Selection:
  emu (emulation)  - Fast software-based execution for functional testing
  sim (simulation) - RTL simulation for detailed verification
  hw (hardware)    - Full FPGA bitstream generation for deployment

  WARNING: Hardware builds (-p hw) take significant time, ranging from
  minutes to hours depending on design complexity and machine resources.
  Use emulation for rapid development and testing.

Example:
  {sys.argv[0]} link -c config.cfg -k kernels/ip/accumulate/component.xml \\
    kernels/ip/increment/component.xml -o accelerator.vbin -p hw

Resource Directory:
  The linker uses resources from a resource directory. See top-level help
  ({sys.argv[0]} --help) for resource directory resolution.

Build Artifacts:
  A project directory (<output>.prj) will be created alongside the output
  VBIN archive, containing TCL scripts, Vivado projects, and build logs.
"""


class LinkerConfiguration(CommandConfiguration):

    @classmethod
    def populate_argument_parser(cls, ap: argparse.ArgumentParser):
        super().populate_argument_parser(ap)
        ap.description = "Link kernel IP cores into a complete design and build a VBIN archive for emulation, simulation, or hardware execution."
        ap.epilog = LINK_HELP_EPILOG
        ap.add_argument("-c", "--config", required=True, type=Path,
                        help="Path to the connectivity configuration file (e.g. config.cfg).")
        ap.add_argument("-k", "--kernels", required=True, type=Path, nargs="+",
                        help="List of component.xml files to load as kernel IP cores.")
        ap.add_argument("-o", "--out", required=True, type=Path,
                        help="Path to the final VBIN archive.")
        ap.add_argument("-p", "--platform", choices=["emu", "sim", "hw"],
                        default="emu", help="Target platform (hw, sim, or emu). Default: emu")
        ap.add_argument("--pre-synth-tcls", type=Path, nargs="*", default=[],
                        help="Paths to TCL scripts to run before synthesis (applies to hardware builds only).")
        ap.add_argument("--clock-hz", required=False,
                        type=Optional[int], default=None, help="Target clock frequency in MHz.")

    def __init__(self, args: argparse.Namespace):
        super().__init__(args)

        # ============
        # Set up paths
        # ============

        # Resolve and verify the configuration file
        configuration_file = args.config.expanduser().resolve()
        if not configuration_file.is_file():
            raise FileNotFoundError(configuration_file)

        # Resolve and verify the kernel component files
        self._kernel_component_paths: List[Path] = [
            path.expanduser().resolve() for path in args.kernels]
        for kernel in self._kernel_component_paths:
            if not kernel.is_file():
                raise FileNotFoundError(kernel)

        # Resolve the out path and remove the old output if necessary
        self._out_path: Path = args.out.expanduser().resolve()
        if self._out_path.is_file():
            self._out_path.unlink()

        # Resolve the build directory, clean up if necessary, and prepare it
        self._build_dir: Path = self._out_path.with_name(
            f"{self._out_path.name}.prj")
        if self._build_dir.is_dir():
            shutil.rmtree(self._build_dir)
        if self._build_dir.is_file():
            self._build_dir.unlink()
        self._build_dir.mkdir(parents=True)

        # Resolve and verify pre-synthesis TCLs (if any)
        self._pre_synth_tcls: List[Path] = []
        for path in args.pre_synth_tcls:
            path: Path = path.expanduser().resolve()
            if not path.is_file():
                raise FileNotFoundError(path)
            self._pre_synth_tcls.append(path)

        # Misc. arguments
        self._platform = Platform(args.platform)
        self._clock_hz: int = args.clock_hz

        # Sanitize the output file stem as the project name
        s2 = re.sub(r"[^A-Za-z0-9_]+", "_", str(self._out_path.stem).strip())
        if not s2:
            s2 = "proj"
        if s2[0].isdigit():
            s2 = "_" + s2
        self._project_name: str = s2

        # Resolve the Vitis include directory
        self._vitis_include_dir = _find_vitis_include()

        # =======================
        # Argument interpretation
        # =======================
        self._bd_ports: BlockDesignPorts = load_bd_ports_from_file(
            self.resources_dir / "bd_ports.txt")

        self._kernels: List[Kernel] = [parse_component_xml(
            kfile) for kfile in self.kernel_component_paths]

        self._configuration: ConnectivityConfig = parse_connectivity_file(
            configuration_file)
        self._kernel_instances: List[KernelInstance] = apply_config_to_instances(
            self.configuration, self.kernels)

    @property
    def block_design_ports(self) -> BlockDesignPorts:
        return self._bd_ports

    @property
    def configuration(self) -> ConnectivityConfig:
        return self._configuration

    @property
    def networking_enabled(self) -> bool:
        # TODO: Change to some sort of description for different service layers once available.
        return len(self.configuration.net.enabled_eth) > 0

    @property
    def out_path(self) -> Path:
        return self._out_path

    @property
    def platform(self) -> Platform:
        return self._platform

    @property
    def project_name(self) -> str:
        return self._project_name

    @property
    def kernel_component_paths(self) -> List[Path]:
        return self._kernel_component_paths

    @property
    def kernels(self) -> List[Kernel]:
        return self._kernels

    @property
    def kernel_instances(self) -> Dict[str, KernelInstance]:
        return self._kernel_instances

    @property
    def build_dir(self) -> Path:
        return self._build_dir

    @property
    def vitis_include_dir(self) -> Path:
        return self._vitis_include_dir

    @property
    def pre_synth_tcls(self) -> List[Path]:
        return self._pre_synth_tcls

    @property
    def clock_hz(self) -> Optional[int]:
        return self._clock_hz


INSTALL_HELP_EPILOG = f"""
Purpose:
  The 'install' subcommand prepares an abstract shell definition required for
  hardware builds. This is a one-time setup operation that creates base images
  used by the 'link' subcommand when targeting hardware (-p hw).

When to Use:
  - During initial installation of the V80++ linker
  - When the abstract shell definition needs to be regenerated

  Most users will NOT need to run this command regularly. It is only required
  during linker installation/setup.

What It Does:
  1. Builds the abstract shell base images from the resource directory
  2. Generates necessary Vivado synthesis artifacts
  3. Creates reusable partial designs for hardware linking

  WARNING: This operation involves full Vivado synthesis and implementation,
  which takes significant time (multiple hours depending on the system).

Resource Directory:
  The 'install' subcommand uses resources from a resource directory. See
  top-level help ({sys.argv[0]} --help) for resource directory resolution.

Build Artifacts:
  The build directory (--build-dir) will contain Vivado projects, checkpoints,
  and logs. This directory can be removed after successful installation.

Example:
  {sys.argv[0]} install --build-dir ./install.prj --jobs 16
"""


class InstallerConfiguration(CommandConfiguration):
    @classmethod
    def populate_argument_parser(cls, ap: argparse.ArgumentParser):
        super().populate_argument_parser(ap)
        ap.description = "Build and install base images for hardware builds."
        ap.epilog = INSTALL_HELP_EPILOG
        ap.add_argument("--build-dir", required=False, type=Optional[Path], default=Path(
            "./install.prj"), help="The build directory for the installer. Default: ./install_build")

    def __init__(self, args: argparse.Namespace):
        super().__init__(args)

        self._build_dir: Path = args.build_dir.expanduser().resolve()
        if self._build_dir.is_dir():
            shutil.rmtree(self._build_dir)
        self._build_dir.mkdir(parents=True)

    @property
    def project_name(self):
        return "slash_install"

    @property
    def build_dir(self):
        return self._build_dir
