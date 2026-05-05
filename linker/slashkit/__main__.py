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

import argparse
import logging
import os
import threading
import time

from slashkit.emit.hw.tcl_gen import generate_tcl
from slashkit.emit.hw.project_gen import (
    build_service_layer_rm,
    build_slash_rm,
    generate_util_report,
    install_static_shell,
)
from slashkit.emit.sim.tcl_gen import generate_sim_tcl
from slashkit.emit.emu.tcl_gen import generate_emu_tcl
from slashkit.emit.sim.project_gen import create_sim_project, build_sim_project
from slashkit.emit.emu.project_gen import build_emu_project, package_emu_artifacts

from slashkit.emit.metadata.prog_image import build_vbin
from slashkit.core.command_config import LinkerConfiguration, Platform, InstallerConfiguration, CommandConfiguration


def _format_duration(seconds: float) -> str:
    total = int(round(seconds))
    hours = total // 3600
    minutes = (total % 3600) // 60
    secs = total % 60
    return f"{hours:02d}:{minutes:02d}:{secs:02d}"


def profiled(func) -> None:
    return lambda: run_with_profiling(func.__name__, func)


def run_with_profiling(label: str, func) -> None:
    start_wall = time.perf_counter()
    start_cpu = time.process_time()
    start_rusage = None
    cores = os.cpu_count() or 1
    peak_cpu_pct = None
    stop_event = threading.Event()

    def _sample_cpu_peak() -> None:
        nonlocal peak_cpu_pct
        last_wall = time.perf_counter()
        last_cpu = time.process_time()
        while not stop_event.wait(0.2):
            now_wall = time.perf_counter()
            now_cpu = time.process_time()
            delta_wall = now_wall - last_wall
            if delta_wall > 0:
                delta_cpu = now_cpu - last_cpu
                cpu_pct = (delta_cpu / delta_wall) * 100.0
                if peak_cpu_pct is None or cpu_pct > peak_cpu_pct:
                    peak_cpu_pct = cpu_pct
            last_wall = now_wall
            last_cpu = now_cpu

    try:
        import resource
        start_rusage = resource.getrusage(resource.RUSAGE_SELF)
    except Exception:
        start_rusage = None

    sampler = threading.Thread(target=_sample_cpu_peak, daemon=True)
    sampler.start()
    try:
        func()
    finally:
        stop_event.set()
        sampler.join(timeout=1.0)
        end_wall = time.perf_counter()
        end_cpu = time.process_time()
        cpu_str = _format_duration(end_cpu - start_cpu)
        wall_str = _format_duration(end_wall - start_wall)
        avg_cpu_pct = 0.0
        elapsed = end_wall - start_wall
        if elapsed > 0:
            avg_cpu_pct = ((end_cpu - start_cpu) / elapsed) * 100.0
        rss_part = ""
        if start_rusage is not None:
            try:
                import resource
                end_rusage = resource.getrusage(resource.RUSAGE_SELF)
                # ru_maxrss is in kilobytes on Linux; convert to MB.
                rss_mb = end_rusage.ru_maxrss / 1024.0
                rss_part = f" ; max_rss = {rss_mb:.1f} MB"
            except Exception:
                rss_part = ""
        peak_part = ""
        if peak_cpu_pct is not None:
            peak_part = f" ; cpu_peak_pct = {peak_cpu_pct:.1f}"
        print(
            f"{label}: Time (s): cpu = {cpu_str} ; elapsed = {wall_str}"
            f" ; cpu_avg_pct = {avg_cpu_pct:.1f}{peak_part} ; cores = {cores}{rss_part}"
        )


def link(config: LinkerConfiguration) -> None:
    if config.platform == Platform.SIMULATION:
        generate_sim_tcl(config)
    elif config.platform == Platform.EMULATION:
        generate_emu_tcl(config)
    else:
        generate_tcl(config)

    if config.platform == Platform.SIMULATION:
        create_sim_project(config)
        build_sim_project(config)
    elif config.platform == Platform.EMULATION:
        build_emu_project(config)
    else:
        run_with_profiling("build_slash", lambda: build_slash_rm(config))
        # Only build a service layer if ethernet is enabled
        # Will be changed once more service layers become available
        if config.networking_enabled:
            run_with_profiling("build_service_layer",
                               lambda: build_service_layer_rm(config))

    if config.platform == Platform.SIMULATION:
        pass
    elif config.platform == Platform.EMULATION:
        package_emu_artifacts(config)
    else:
        generate_util_report(config)
        build_vbin(config)


MAIN_HELP_EPILOG = """
Typical Workflow:
  Most users will use the 'link' subcommand to link kernel IP cores into
  an emulation, simulation, or hardware build image.

  The 'install' subcommand is only used during the installation of the linker.
  It prepares an static shell definition, which is later used by the 'link'
  subcommand to create hardware images.
"""


def main():
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s:%(funcName)s: %(message)s",
    )

    ap = argparse.ArgumentParser(description="Utility to link VRT binaries (VBINs) from user IP cores.", conflict_handler="resolve", epilog=MAIN_HELP_EPILOG,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub_parsers = ap.add_subparsers(required=True)

    link_parser = sub_parsers.add_parser("link")
    LinkerConfiguration.populate_argument_parser(link_parser)
    link_parser.set_defaults(config_class=LinkerConfiguration, operation=link)

    install_parser = sub_parsers.add_parser("install")
    InstallerConfiguration.populate_argument_parser(install_parser)
    install_parser.set_defaults(
        config_class=InstallerConfiguration, operation=install_static_shell)

    args = ap.parse_args()

    config = args.config_class(args)
    args.operation(config)


if __name__ == "__main__":
    main()
