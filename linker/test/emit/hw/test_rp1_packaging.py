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

from importlib import resources
from pathlib import Path
from types import SimpleNamespace
import subprocess
import sys

import pytest

from slashkit.emit.hw import project_gen


def test_service_shell_keeps_rp1_mailbox_and_bar_window():
    """The service topology exposes the mailbox and BAR space required by RP1."""
    top_tcl = (
        resources.files("slashkit.resources.base.service.scripts")
        .joinpath("top.tcl")
        .read_text()
    )

    assert "xilinx.com:ip:pcie_qdma_mailbox:1.0" in top_tcl
    assert "assign_bd_address -offset 0x80200000 -range 0x00200000" in top_tcl
    assert "CONFIG.APERTURES {{0x204_0000_0000 64M}}" in top_tcl
    assert "CPM_PCIE1_PF2_BAR4_QDMA_PREFETCHABLE {1}" in top_tcl
    assert "CPM_PCIE1_PF2_BAR4_QDMA_SIZE {128}" in top_tcl
    assert "M04_INI {read_bw {500} write_bw {500} initial_boot {true}}" in top_tcl
    assert "assign_bd_address -offset 0x020200600000 -range 0x00200000" in top_tcl


def _assert_no_generated_dirs(root):
    for entry in root.iterdir():
        if not entry.is_dir():
            continue
        assert entry.name != "__pycache__"
        assert entry.name != "build"
        assert not entry.name.startswith("build-")
        _assert_no_generated_dirs(entry)


def test_rp1_package_resources_stage_without_generated_dirs(tmp_path):
    root = resources.files("slashkit.resources.aved").joinpath("rp1")
    required = (
        "CMakeLists.txt",
        "build-rp1.sh",
        "config/rp1_platform_config.h.in",
        "include/slash/uapi/rp1_protocol.h",
        "tools/generate_platform_config.py",
        "tools/generate_r5_bsp.tcl",
    )
    assert all(root.joinpath(*name.split("/")).is_file()
               for name in required)
    _assert_no_generated_dirs(root)

    aved_dir = tmp_path / "AVED"
    project_gen._copy_rp1_sources_to_aved(aved_dir)
    staged = aved_dir / "fw" / "RP1"

    assert all((staged / name).is_file() for name in required)
    _assert_no_generated_dirs(staged)


def test_packaged_rp1_protocol_header_is_synchronized():
    repo_root = Path(__file__).resolve().parents[4]
    subprocess.run(
        [
            sys.executable,
            str(repo_root / "scripts/stage-rp1-protocol-header.py"),
            "--check",
        ],
        check=True,
    )


def test_missing_rp1_package_resources_fail_clearly(monkeypatch):
    monkeypatch.setattr(
        project_gen, "_RP1_RESOURCE_PACKAGE",
        "slashkit.resources.aved.missing_rp1")
    with pytest.raises(FileNotFoundError, match="packaged RP1"):
        project_gen._rp1_resource_root()


def test_rp1_repack_installs_fpt_and_nofpt_outputs(tmp_path, monkeypatch):
    build_dir = tmp_path / "build"
    aved_dir = build_dir / "AVED"
    hw_dir = aved_dir / "hw" / project_gen.AVED_DESIGN_NAME
    aved_build_dir = hw_dir / "build"
    fpt_dir = hw_dir / "fpt"
    static_shell_dir = tmp_path / "static_shell"

    required = (
        aved_build_dir / "top_wrapper.pdi",
        aved_build_dir / f"{project_gen.AVED_DESIGN_NAME}.xsa",
        aved_build_dir / "amc.elf",
        aved_build_dir / "fpt.bin",
        fpt_dir / "pdi_combine.bif",
        fpt_dir / "fpt_pdi_gen.py",
    )
    for path in required:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"input")

    def fake_run(command, cwd, **_kwargs):
        if command[:2] == ["bash", "build-rp1.sh"]:
            output = Path(cwd) / "build" / "rp1.elf"
        elif command[0] == "bootgen":
            output = Path(command[command.index("-o") + 1])
        else:
            output = Path(command[command.index("--output") + 1])
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(output.name.encode())

    monkeypatch.setattr(project_gen.subprocess, "run", fake_run)

    config = SimpleNamespace(build_dir=build_dir)
    project_gen._install_static_shell_rp1_firmware(
        config, static_shell_dir)

    wrapped = static_shell_dir / f"{project_gen.AVED_DESIGN_NAME}.pdi"
    nofpt = static_shell_dir / \
        f"{project_gen.AVED_DESIGN_NAME}_nofpt.pdi"
    assert wrapped.read_bytes() == wrapped.name.encode()
    assert nofpt.read_bytes() == nofpt.name.encode()
