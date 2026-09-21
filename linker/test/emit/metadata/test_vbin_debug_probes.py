# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
"""
A vbin for a design with [debug] nets must carry its debug probe file.

Without the .ltx the vbin still programs and the ILA still arms and triggers -
the debug hub is self-describing, so Vivado finds the core either way - but there
is no probe metadata, so the waveform never populates. That presents as a
hardware or trigger problem, which is the wrong place to look, so a missing probe
file has to fail the build loudly rather than drop out of the archive quietly.
"""
from pathlib import Path
from types import SimpleNamespace
import tarfile

import pytest

from slashkit.emit.metadata import prog_image


def _make_config(tmp_path: Path, *, debug_nets, with_ltx: bool):
    project = "proj"
    build_dir = tmp_path / "build"
    images = build_dir / "images"
    images.mkdir(parents=True)

    # The artifacts build_vbin always requires.
    prefix = f"top_i_slash_slash_{project}_inst_0"
    (images / f"{prefix}_partial.pdi").write_text("pdi")
    (build_dir / f"report_utilization_{project}.xml").write_text("<x/>")
    (build_dir / "system_map.xml").write_text("<x/>")
    if with_ltx:
        (images / f"{prefix}_hw_probes.ltx").write_text("ltx")

    return SimpleNamespace(
        build_dir=build_dir,
        project_name=project,
        out_path=tmp_path / "out.vbin",
        networking_enabled=False,
        configuration=SimpleNamespace(
            debug=SimpleNamespace(nets=list(debug_nets))),
    )


def _members(vbin: Path):
    with tarfile.open(vbin, "r:gz") as tf:
        return [Path(m.name).name for m in tf.getmembers()]


def test_debug_design_with_probe_file_ships_it(tmp_path):
    config = _make_config(tmp_path, debug_nets=["a", "b"], with_ltx=True)
    out = prog_image.build_vbin(config)
    assert any(name.endswith("hw_probes.ltx") for name in _members(out))


def test_debug_design_without_probe_file_fails_loudly(tmp_path):
    """The regression this guards: it used to build a vbin with no probes and
    say nothing, leaving an ILA that triggers but shows an empty waveform."""
    config = _make_config(tmp_path, debug_nets=["a", "b"], with_ltx=False)
    with pytest.raises(FileNotFoundError) as excinfo:
        prog_image.build_vbin(config)
    message = str(excinfo.value)
    assert "hw_probes.ltx" in message
    assert "2 [debug] net" in message
    # The message should point at the symptom, so the reader connects the two.
    assert "waveform" in message.lower()


def test_design_without_debug_nets_needs_no_probe_file(tmp_path):
    config = _make_config(tmp_path, debug_nets=[], with_ltx=False)
    out = prog_image.build_vbin(config)
    assert not any(name.endswith(".ltx") for name in _members(out))


def test_probe_file_is_shipped_even_without_declared_nets(tmp_path):
    """If the build produced one anyway, do not silently discard it."""
    config = _make_config(tmp_path, debug_nets=[], with_ltx=True)
    out = prog_image.build_vbin(config)
    assert any(name.endswith("hw_probes.ltx") for name in _members(out))
