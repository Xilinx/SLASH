"""
Smoke test for the cluster launcher.

Drives a throwaway Vivado synthesis through exactly the machinery slashkit
uses for a real build -- launcher.run_tool, with the same environment the RM
builds get -- so that the plumbing can be checked in a couple of minutes
instead of discovering it is broken several hours into a real build.

  export SLASH_TOOL_LAUNCHER=$PWD/scripts/lsf/lsf-run.sh
  python3 scripts/lsf/smoke/run_dummy.py

Peaks around 3 GB. With SLASH_TOOL_LAUNCHER unset it runs locally instead,
which is the A/B to reach for when something breaks.

  SLASH_SMOKE_BUILD_DIR  throwaway project location (default: ./lsf_smoke)
  SLASH_SMOKE_VIVADO     Vivado binary; defaults to the bare name "vivado",
                         resolved on the execution host
"""
import os
import subprocess
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here.parents[2] / "linker"))

from slashkit.core.launcher import run_tool, tool_launcher  # noqa: E402
from slashkit.emit.hw.project_gen import (  # noqa: E402
    _environment_with_udev_ld_preload,
)

build_dir = Path(os.environ.get(
    "SLASH_SMOKE_BUILD_DIR", Path.cwd() / "lsf_smoke")).resolve()
build_dir.mkdir(parents=True, exist_ok=True)

# A bare name works when a launcher is set, since the execution host puts the
# real binary on PATH; running locally, it has to already be on this PATH.
vivado_bin = os.environ.get("SLASH_SMOKE_VIVADO", "vivado")

cmd = [
    str(vivado_bin),
    "-mode", "batch",
    "-nojournal",
    "-log", str(build_dir / "vivado.log"),
    "-source", str(here / "dummy_build.tcl"),
    "-tclargs",
    "dummy_prj",
    str(build_dir / "iprepo"),
    "all",
    os.environ.get("SLASH_LSF_CORES", "8"),
]

print("build dir:", build_dir)
print("launcher :", tool_launcher() or "<none, running locally>")
print("command  :", " ".join(cmd), flush=True)

# Deliberately the same call the real flow makes; "smoke" is an unconfigured
# task kind, so it picks up the site-wide sizing rather than a build-sized
# reservation.
try:
    run_tool(cmd, task="smoke", cwd=build_dir,
             env=_environment_with_udev_ld_preload())
    status = 0
except subprocess.CalledProcessError as exc:
    status = exc.returncode or 1

print(f"exit     : {status}")

artifact = build_dir / "dummy_prj" / "dummy_prj_synth.dcp"
if status == 0 and not artifact.is_file():
    # The launcher propagated a success it should not have: worth catching
    # here rather than in a build that trusts the artifact.
    print(f"FAILED   : Vivado reported success but {artifact} is missing")
    sys.exit(1)
if status == 0:
    print(f"OK       : {artifact}")
sys.exit(status)
