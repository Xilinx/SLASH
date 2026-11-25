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

from slashkit.core import launcher
from slashkit.emit.hw import project_gen


def test_service_shell_keeps_rp1_bar_window():
    """The service topology exposes the BAR space required by RP1."""
    top_tcl = (
        resources.files("slashkit.resources.base.service.scripts")
        .joinpath("top.tcl")
        .read_text()
    )

    assert "CPM_PCIE1_PF2_BAR4_QDMA_PREFETCHABLE {1}" in top_tcl
    assert "CPM_PCIE1_PF2_BAR4_QDMA_SIZE {128}" in top_tcl
    assert "0x20404000000 0x00030000000 0x04000000" in top_tcl
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
    )
    assert all(root.joinpath(*name.split("/")).is_file()
               for name in required)
    build_script = root.joinpath("build-rp1.sh").read_text()
    assert "sdtgen set_dt_param" in build_script
    assert "empyro create_bsp" in build_script
    assert "xsct" not in build_script
    _assert_no_generated_dirs(root)

    aved_dir = tmp_path / "AVED"
    project_gen._copy_rp1_sources_to_aved(aved_dir)
    staged = aved_dir / "fw" / "RP1"

    assert all((staged / name).is_file() for name in required)
    _assert_no_generated_dirs(staged)


def test_plm_package_resources_patch_sparse_ipi_dispatch(tmp_path):
    """The staged custom PLM replaces the faulty sparse-mask demultiplexer."""
    aved_resources = resources.files("slashkit.resources.aved")
    root = aved_resources.joinpath("plm")
    required = ("build-plm.sh", "tools/patch_xilplmi.py")
    assert all(root.joinpath(*name.split("/")).is_file()
               for name in required)
    build_all = aved_resources.joinpath("build_all.sh").read_text()
    bif = aved_resources.joinpath("pdi_combine.bif").read_text()
    assert "build-plm.sh" not in build_all
    assert "type=bootloader, file=./build/plm.elf" not in bif
    _assert_no_generated_dirs(root)

    aved_dir = tmp_path / "AVED"
    project_gen._copy_plm_sources_to_aved(aved_dir)
    staged = aved_dir / "fw" / "PLM"
    assert all((staged / name).is_file() for name in required)

    platform_source = (
        tmp_path / "bsp" / "libsrc" / "xilplmi" / "src" /
        "versal" / "server" / "xplmi_plat.c"
    )
    platform_source.parent.mkdir(parents=True)
    platform_source.write_text(
        """
\tu32 IpiIndex;
\tu32 IpiIndexMask;
\tfor (IpiIndex = 0U; IpiIndex < XPLMI_IPI_MASK_COUNT; ++IpiIndex) {
\t\tIpiIndexMask = (u32)1U << IpiIndex;
\t\tif (((IpiIntrVal & IpiIndexMask) != 0U) &&
\t\t\t((IpiMaskVal & IpiIndexMask) == 0U)) {
\t\t\tXPlmi_GicIntrAddTask(XPlmi_GetIpiIntrId(IpiIndex));
\t\t}
\t}
""",
        encoding="utf-8",
    )
    subprocess.run(
        [
            sys.executable,
            str(staged / "tools" / "patch_xilplmi.py"),
            "--bsp-root",
            str(tmp_path / "bsp"),
        ],
        check=True,
    )

    patched = platform_source.read_text(encoding="utf-8")
    assert "IpiInstance->Config.TargetCount" in patched
    assert "TargetList[IpiIndex].Mask" in patched
    assert "TargetList[IpiIndex].BufferIndex" in patched
    assert "1U << IpiIndex" not in patched


def test_patched_plm_build_reuses_existing_amc_sdt(tmp_path, monkeypatch):
    """Firmware-only repacks avoid regenerating SDT metadata when possible."""
    aved_dir = tmp_path / "AVED"
    plm_dir = aved_dir / "fw" / "PLM"
    plm_dir.mkdir(parents=True)
    (plm_dir / "build-plm.sh").write_text("# fixture\n")
    xsa = tmp_path / "shell.xsa"
    xsa.write_bytes(b"xsa")
    amc_sdt = (
        aved_dir / "fw" / "AMC" / "amc_bsp" /
        "versal_sdt" / "system-top.dts"
    )
    amc_sdt.parent.mkdir(parents=True)
    amc_sdt.write_text("/dts-v1/;\n")
    calls = []

    def fake_run(command, cwd, env, check):
        calls.append((command, Path(cwd), env, check))
        output = plm_dir / "build" / "plm.elf"
        output.parent.mkdir(parents=True)
        output.write_bytes(b"plm")

    monkeypatch.setattr(project_gen.subprocess, "run", fake_run)

    output = project_gen._build_patched_plm(aved_dir, xsa)

    assert output.read_bytes() == b"plm"
    assert calls[0][0] == ["bash", "build-plm.sh"]
    assert calls[0][1] == plm_dir
    assert calls[0][2]["XSA"] == str(xsa)
    assert calls[0][2]["PLM_SDT"] == str(amc_sdt)
    assert calls[0][3] is True


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
    # The repack goes through run_tool, which prepends the launcher to the argv
    # that fake_run below matches on. Pin the local path, or this fails for
    # anyone who has the variable exported for real builds.
    monkeypatch.delenv(launcher.LAUNCHER_ENV, raising=False)
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
        aved_build_dir / "plm.elf",
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
