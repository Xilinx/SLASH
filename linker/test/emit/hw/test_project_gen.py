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

"""Tests for hardware project generation helpers."""

from contextlib import nullcontext
from pathlib import Path
from types import SimpleNamespace

import pytest

from slashkit.core import launcher
from slashkit.core.command_config import ShellType
from slashkit.emit.hw import project_gen, tcl_gen

LIBUDEV = "/lib/x86_64-linux-gnu/libudev.so.1"
LIBCAP = "/lib/x86_64-linux-gnu/libcap.so.2"


@pytest.mark.parametrize(
    ("shell_type", "directory"),
    [
        (ShellType.SERVICE, "static_shell"),
        (ShellType.COMPUTE, "static_shell_compute"),
    ],
)
@pytest.mark.parametrize(
    ("stage", "expected"),
    [
        ("all", ["base", "firmware"]),
        ("base-shell", ["base"]),
        ("firmware", ["firmware"]),
        ("rp1-firmware", ["rp1-firmware"]),
    ],
)
def test_install_dispatches_selected_stage_to_selected_shell(
    monkeypatch, tmp_path, shell_type, directory, stage, expected
):
    """Each stage writes only to the package selected by --shell-type."""
    calls = []
    config = SimpleNamespace(
        out_dir=tmp_path,
        shell_type=shell_type,
        stage=stage,
    )

    monkeypatch.setattr(
        project_gen,
        "_install_static_shell_base",
        lambda _config, path: calls.append(("base", path)),
    )
    monkeypatch.setattr(
        project_gen,
        "_install_static_shell_firmware",
        lambda _config, path: calls.append(("firmware", path)),
    )
    monkeypatch.setattr(
        project_gen,
        "_install_static_shell_rp1_firmware",
        lambda _config, path: calls.append(("rp1-firmware", path)),
    )

    project_gen.install_static_shell(config)

    assert [stage_name for stage_name, _path in calls] == expected
    assert all(path == tmp_path / directory for _stage, path in calls)


def test_compute_shell_rejects_debug_hub():
    """Compute builds fail before emitting references to a missing debug hub."""
    config = SimpleNamespace(
        block_design_ports=None,
        configuration=SimpleNamespace(
            streams=[],
            debug=SimpleNamespace(nets=[object()]),
        ),
        kernel_instances=[],
        kernels=[],
        shell_type=ShellType.COMPUTE,
    )

    with pytest.raises(ValueError, match=r"\[debug\] requires shell=service"):
        tcl_gen.generate_tcl(config)


def test_compute_build_forwards_job_count(monkeypatch, tmp_path):
    """The compute Tcl receives --jobs after its optional action argument."""
    tcl_path = tmp_path / "create_project.tcl"
    tcl_path.touch()
    commands = []
    config = SimpleNamespace(
        build_dir=tmp_path,
        vivado_bin=Path("/opt/vivado"),
        project_name="test",
        ip_repository=tmp_path / "iprepo",
        n_jobs=23,
    )

    monkeypatch.setattr(
        project_gen.resources,
        "path",
        lambda _package, _name: nullcontext(tcl_path),
    )
    monkeypatch.setattr(
        project_gen,
        "_environment_with_udev_ld_preload",
        lambda: {},
    )
    # Stubbed out so the build-ID lookup does not shell out to git: its calls
    # would otherwise land in `commands` ahead of the Vivado invocation.
    monkeypatch.setattr(
        project_gen,
        "_compute_build_id_env",
        lambda: {},
    )
    monkeypatch.setattr(
        project_gen.subprocess,
        "run",
        lambda command, **_kwargs: commands.append(command),
    )

    project_gen.create_build_project_compute(config, action="build")

    assert commands[0][-2:] == ["build", "23"]


def _stub_git(monkeypatch, sha, dirty=False):
    """Make the build-ID helper see a fixed commit and clean/dirty state."""
    monkeypatch.setattr(
        project_gen.subprocess,
        "run",
        lambda _command, **_kwargs: SimpleNamespace(
            stdout=f"{sha}\n", returncode=1 if dirty else 0
        ),
    )


def test_build_id_leaves_shell_type_bit_clear(monkeypatch):
    """The shell-type bit[28] is owned by each shell's create_project.tcl.

    This helper is shell-agnostic, so it must leave bit[28] clear for the Tcl
    to set: a compute build ORs it on, a service build masks it off.
    """
    _stub_git(monkeypatch, "204620aada6eeaf1234567890abcdef012345678")

    env = project_gen._compute_build_id_env()

    assert env["SLASH_BUILD_ID_LO"] == "0xada6eeaf"
    assert env["SLASH_BUILD_ID_HI"] == "0x0204620a"

    hi = int(env["SLASH_BUILD_ID_HI"], 16)
    assert hi & 0x0FFFFFFF == 0x0204620A          # hash bits preserved
    assert (hi >> 28) & 0x1 == 0                  # shell type left to the Tcl
    assert (hi >> 29) & 0x3 == 0                  # reserved bits stay clear
    assert (hi >> 31) & 0x1 == 0                  # clean tree


def test_build_id_sets_dirty_bit(monkeypatch):
    """A dirty tree sets bit[31] without disturbing the hash or shell bits."""
    _stub_git(monkeypatch, "204620aada6eeaf1234567890abcdef012345678", dirty=True)

    env = project_gen._compute_build_id_env()

    assert env["SLASH_BUILD_ID_HI"] == "0x8204620a"


def _only_present(monkeypatch, present):
    """Make Path.is_file report True only for the given set of paths."""
    present_set = {str(Path(p)) for p in present}
    monkeypatch.setattr(
        project_gen.Path, "is_file",
        lambda self: str(self) in present_set, raising=True)


def test_preload_includes_libcap_when_libudev_present(monkeypatch):
    """On a 24.04-like host, libudev is preloaded together with libcap."""
    _only_present(monkeypatch, [LIBUDEV, LIBCAP])
    monkeypatch.delenv("LD_PRELOAD", raising=False)
    env = project_gen._environment_with_udev_ld_preload()
    assert env["LD_PRELOAD"] == f"{LIBUDEV}:{LIBCAP}"


def test_preload_libudev_only_when_libcap_absent(monkeypatch):
    """On a 22.04-like host libudev has no libcap dependency, so libudev alone."""
    _only_present(monkeypatch, [LIBUDEV])
    monkeypatch.delenv("LD_PRELOAD", raising=False)
    env = project_gen._environment_with_udev_ld_preload()
    assert env["LD_PRELOAD"] == LIBUDEV


def test_no_preload_when_libudev_absent(monkeypatch):
    """With no libudev the workaround does not apply and LD_PRELOAD is untouched."""
    _only_present(monkeypatch, [LIBCAP])
    monkeypatch.delenv("LD_PRELOAD", raising=False)
    env = project_gen._environment_with_udev_ld_preload()
    assert "LD_PRELOAD" not in env


def _rm_config(tmp_path):
    return SimpleNamespace(
        build_dir=tmp_path,
        ip_repository=tmp_path / "iprepo",
        kernels=[],
        shell_type=ShellType.SERVICE,
        pre_synth_tcls=[],
        project_name="test",
        n_jobs=1,
        vivado_bin=Path("/usr/bin/true"),
    )


def _stub_rm_build(monkeypatch, tmp_path, on_run):
    """Neuter everything an RM build touches except the tool invocation itself."""
    def run_vivado(argv, **kwargs):
        on_run(argv, kwargs)
        images = tmp_path / "images"
        (images / "top_i_slash_slash_test_inst_0_partial.pdi").touch()
        (images / "top_i_service_layer_service_layer_test_inst_0_partial.pdi").touch()

    monkeypatch.setattr(
        project_gen.resources,
        "path",
        lambda _package, name: nullcontext(tmp_path / name),
    )
    # The RM build reaches the tool through launcher.run_tool, so that is where
    # the interception has to happen.
    monkeypatch.setattr(launcher.subprocess, "run", run_vivado)


def test_service_networking_reuses_common_ip_repository(monkeypatch, tmp_path):
    """Building slash and service-layer RMs exports common IP only once."""
    config = _rm_config(tmp_path)
    export_calls = []

    def export_once(_package, out_dir):
        export_calls.append(out_dir)
        out_dir.mkdir()

    monkeypatch.setattr(project_gen, "export_package", export_once)
    _stub_rm_build(monkeypatch, tmp_path, lambda _argv, _kwargs: None)

    project_gen.build_slash_rm(config)
    project_gen.build_service_layer_rm(config)

    assert export_calls == [config.ip_repository / "slash_base"]


def test_rm_builds_are_offloaded_under_distinct_task_kinds(monkeypatch, tmp_path):
    """One `slashkit link` runs both RMs, so they must be distinguishable."""
    monkeypatch.setenv(launcher.LAUNCHER_ENV, "submit")
    config = _rm_config(tmp_path)
    seen = []

    monkeypatch.setattr(project_gen, "export_package",
                        lambda _package, out_dir: out_dir.mkdir())
    _stub_rm_build(monkeypatch, tmp_path,
                   lambda argv, kwargs: seen.append((argv[0], kwargs["env"])))

    project_gen.build_slash_rm(config)
    project_gen.build_service_layer_rm(config)

    assert [prefix for prefix, _ in seen] == ["submit", "submit"]
    assert [env["SLASH_BUILD_TASK"] for _, env in seen] == [
        launcher.TASK_RM_SLASH, launcher.TASK_RM_SERVICE_LAYER]
    assert all(env["SLASH_TOOL_CWD"] == str(tmp_path) for _, env in seen)
