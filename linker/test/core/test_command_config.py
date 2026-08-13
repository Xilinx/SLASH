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

"""Tests for core.command_config — pre-synth Tcl resolution and merging."""

import argparse
import textwrap
from pathlib import Path
from unittest import mock

import pytest

from slashkit.core import command_config
from slashkit.core.command_config import InstallerConfiguration, LinkerConfiguration

_FIXTURE_COMPONENT = (
    Path(__file__).resolve().parents[1]
    / "fixtures" / "dma_in" / "hls" / "impl" / "ip" / "component.xml"
)


def _make_args(tmp_path: Path, *, config: Path, cli_pre_synth):
    vivado = tmp_path / "vivado"
    vivado.write_text("#!/bin/sh\n")
    vivado.chmod(0o755)
    return argparse.Namespace(
        vivado=vivado,
        jobs=8,
        config=config,
        kernels=[_FIXTURE_COMPONENT],
        out=tmp_path / "out.vbin",
        platform="hw",
        pre_synth_tcls=list(cli_pre_synth),
        clock_hz=None,
    )


def _build_config(tmp_path: Path, *, config: Path, cli_pre_synth):
    """Construct a LinkerConfiguration with the heavy, environment-dependent
    lookups stubbed out, so only the pre-synth resolution/merging is exercised."""
    args = _make_args(tmp_path, config=config, cli_pre_synth=cli_pre_synth)
    with mock.patch.object(command_config, "_find_vitis_include",
                           return_value=tmp_path), \
            mock.patch.object(command_config, "apply_config_to_instances",
                              return_value=[]):
        return LinkerConfiguration(args)


def _write_cfg(tmp_path: Path, body: str) -> Path:
    cfg = tmp_path / "config.cfg"
    cfg.write_text(textwrap.dedent(body))
    return cfg


def test_installer_parser_keeps_stage_and_shell_type(tmp_path):
    """The merged installer exposes independent stage and shell selectors."""
    parser = argparse.ArgumentParser()
    InstallerConfiguration.populate_argument_parser(parser)

    args = parser.parse_args(
        [
            "--out-dir",
            str(tmp_path),
            "--stage",
            "rp1-firmware",
            "--shell-type",
            "compute",
        ]
    )

    assert args.stage == "rp1-firmware"
    assert args.shell_type == "compute"


def test_base_shell_stage_preserves_existing_build_directory(tmp_path):
    """A base-shell rerun reuses its AVED checkout and implementation state."""
    vivado = tmp_path / "vivado"
    vivado.write_text("#!/bin/sh\n")
    build_dir = tmp_path / "install.prj"
    build_dir.mkdir()
    marker = build_dir / "keep"
    marker.touch()
    out_dir = tmp_path / "resources"
    out_dir.mkdir()

    config = InstallerConfiguration(
        argparse.Namespace(
            vivado=vivado,
            jobs=8,
            ignore_timing_failure=False,
            stage="base-shell",
            build_dir=build_dir,
            aved_repo="unused",
            aved_ref="unused",
            shell_type="service",
            out_dir=out_dir,
        )
    )

    assert config.build_dir == build_dir.resolve()
    assert marker.exists()


def test_config_cfg_pre_synth_reaches_pre_synth_tcls(tmp_path):
    """[user_region] pre_synth= entries from the config file must be honoured."""
    a = tmp_path / "a.tcl"
    a.write_text("# a\n")
    cfg = _write_cfg(tmp_path, """
        [connectivity]
        [user_region]
        pre_synth=a.tcl
    """)
    config = _build_config(tmp_path, config=cfg, cli_pre_synth=[])
    assert config.pre_synth_tcls == [a.resolve()]


def test_config_cfg_and_cli_pre_synth_are_merged_in_order(tmp_path):
    """Config-declared scripts run first, then CLI ones."""
    a = tmp_path / "a.tcl"
    a.write_text("# a\n")
    b = tmp_path / "b.tcl"
    b.write_text("# b\n")
    cli = tmp_path / "cli.tcl"
    cli.write_text("# cli\n")
    cfg = _write_cfg(tmp_path, """
        [connectivity]
        [user_region]
        pre_synth=a.tcl
        pre_synth=b.tcl
    """)
    config = _build_config(tmp_path, config=cfg, cli_pre_synth=[cli])
    assert config.pre_synth_tcls == [a.resolve(), b.resolve(), cli.resolve()]


def test_pre_synth_duplicates_are_de_duplicated(tmp_path):
    """A script named in both the config and on the CLI is sourced once."""
    a = tmp_path / "a.tcl"
    a.write_text("# a\n")
    cfg = _write_cfg(tmp_path, """
        [connectivity]
        [user_region]
        pre_synth=a.tcl
    """)
    config = _build_config(tmp_path, config=cfg, cli_pre_synth=[a])
    assert config.pre_synth_tcls == [a.resolve()]


def test_missing_config_cfg_pre_synth_raises(tmp_path):
    """A non-existent [user_region] pre_synth path fails fast."""
    cfg = _write_cfg(tmp_path, """
        [connectivity]
        [user_region]
        pre_synth=does_not_exist.tcl
    """)
    with pytest.raises(FileNotFoundError):
        _build_config(tmp_path, config=cfg, cli_pre_synth=[])
