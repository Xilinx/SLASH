# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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
"""
Optional dispatch of external tool invocations to a compute farm.

Every heavy tool the hardware flow runs -- Vivado, bootgen, the AVED firmware
build -- goes through :func:`run_tool`. By default it is an ordinary
``subprocess.run``. Setting ``SLASH_TOOL_LAUNCHER`` inserts a wrapper command
in front, which is how a build gets offloaded to a scheduler without slashkit
knowing anything about the scheduler.

The wrapper contract, deliberately scheduler-agnostic:

* it is a command prefix -- it receives the full tool argv and runs it;
* it blocks until the tool exits;
* it propagates the exit status, because every caller relies on ``check=True``;
* it reproduces the working directory named by ``SLASH_TOOL_CWD``;
* it may read ``SLASH_BUILD_TASK`` to size the resources it reserves.

``scripts/lsf/`` is a worked example for IBM Spectrum LSF.
"""
from __future__ import annotations

import logging
import os
import shlex
import subprocess
from pathlib import Path
from typing import Dict, List, Optional

logger = logging.getLogger(__name__)

LAUNCHER_ENV = "SLASH_TOOL_LAUNCHER"

# Renamed in favour of LAUNCHER_ENV once the hook grew beyond Vivado. Honouring
# the old name silently would be worse than ignoring it: a stale export in a
# shell profile would quietly move a twelve-hour synthesis back onto the local
# machine. Warn loudly instead. Drop this once the rename has shipped.
_LEGACY_LAUNCHER_ENV = "SLASH_VIVADO_LAUNCHER"

# Identifies the step to the launcher so it can size the reservation. Underscores
# throughout, because these end up as shell variable name suffixes.
TASK_STATIC_SHELL = "static_shell"
TASK_STATIC_SHELL_COMPUTE = "static_shell_compute"
TASK_RM_SLASH = "rm_slash"
TASK_RM_SERVICE_LAYER = "rm_service_layer"
TASK_BOOTGEN = "bootgen"
TASK_AVED = "aved"
# Emitted by cmake/BuildHLS.cmake rather than from here, since HLS kernels are
# built by make. Listed so that the set of task kinds has one home.
TASK_HLS = "hls"


def tool_launcher() -> List[str]:
    """Return the configured launcher prefix, or an empty list to run locally."""
    if _LEGACY_LAUNCHER_ENV in os.environ and LAUNCHER_ENV not in os.environ:
        logger.warning(
            "%s is set but ignored; it was renamed to %s. Tools will run locally.",
            _LEGACY_LAUNCHER_ENV, LAUNCHER_ENV,
        )
    return shlex.split(os.environ.get(LAUNCHER_ENV, ""))


def run_tool(
    cmd: List[str],
    *,
    task: str,
    cwd: Path,
    env: Optional[Dict[str, str]] = None,
) -> None:
    """
    Run one external tool invocation, optionally dispatched to a compute farm.

    Everything slashkit-specific is already staged on disk by the time this is
    called, so the launcher only ever sees a plain vendor tool argv. An
    execution host therefore needs nothing installed beyond the vendor
    toolchain and access to the shared filesystem.

    ``cwd`` is exported as ``SLASH_TOOL_CWD`` as well as being applied locally.
    Schedulers only reproduce the submission directory as a courtesy and some
    fall back to a temporary directory when the path does not exist on the
    execution host, which would silently break the tools that resolve their
    inputs relative to the process working directory.
    """
    child_env = dict(os.environ if env is None else env)
    child_env["SLASH_BUILD_TASK"] = task
    child_env["SLASH_TOOL_CWD"] = str(cwd)

    launcher = tool_launcher()
    if launcher:
        logger.info("Dispatching %s via %s", task, launcher[0])

    subprocess.run(launcher + [str(arg) for arg in cmd],
                   cwd=str(cwd), check=True, env=child_env)
