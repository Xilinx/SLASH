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

"""Tests for apply_timing_frequency_cap — achieved-frequency capping at target."""

import textwrap
from pathlib import Path

import pytest

from slashkit.emit.metadata import timing_freq


def _write_system_map(path: Path, clock_hz: int) -> None:
    path.write_text(
        f'<?xml version="1.0"?>\n<SystemMap>\n'
        f'  <ClockFrequency>{clock_hz}</ClockFrequency>\n</SystemMap>\n',
        encoding="utf-8",
    )


def _write_timing_report(path: Path, wns_ns: float, whs_ns: float = 0.100) -> None:
    # The full column set Vivado emits, reproduced verbatim from a V80 static
    # shell run. The endpoint-count columns interleaved between the slacks are
    # the point: an abridged WNS/TNS/WHS/THS table puts WHS at offset 2 and lets
    # an offset-based parser look correct while reading the wrong column off a
    # real report.
    path.write_text(
        textwrap.dedent(
            f"""\
            | Design Timing Summary
            | ---------------------

                WNS(ns)      TNS(ns)  TNS Failing Endpoints  TNS Total Endpoints      WHS(ns)      THS(ns)  THS Failing Endpoints  THS Total Endpoints     WPWS(ns)     TPWS(ns)  TPWS Failing Endpoints  TPWS Total Endpoints
                -------      -------  ---------------------  -------------------      -------      -------  ---------------------  -------------------     --------     --------  ----------------------  --------------------
                 {wns_ns:.3f}       -31.539                   2049              1704696        {whs_ns:.3f}        0.000                      0              1700856        0.000        0.000                       0                520312
            """
        ),
        encoding="utf-8",
    )


def test_positive_slack_is_capped_at_target(tmp_path):
    # Target 250 MHz (4 ns). Positive WNS (headroom) would allow a higher
    # frequency, but the result must be capped at the requested target.
    target_hz = 250_000_000
    system_map = tmp_path / "system_map.xml"
    report = tmp_path / "report_timing_proj.txt"
    _write_system_map(system_map, target_hz)
    _write_timing_report(report, wns_ns=1.0)  # achievable ~333 MHz

    result = timing_freq.apply_timing_frequency_cap(
        project_name="proj",
        system_map_path=system_map,
        timing_report=report,
    )

    assert result == target_hz
    assert timing_freq.read_system_map_clock_hz(system_map) == target_hz


def test_negative_slack_lowers_below_target(tmp_path):
    # Target 250 MHz (4 ns period). Negative WNS means timing failed: the
    # achievable frequency is 1e9 / (4 - (-1)) = 200 MHz, below the target.
    target_hz = 250_000_000
    system_map = tmp_path / "system_map.xml"
    report = tmp_path / "report_timing_proj.txt"
    _write_system_map(system_map, target_hz)
    _write_timing_report(report, wns_ns=-1.0)

    result = timing_freq.apply_timing_frequency_cap(
        project_name="proj",
        system_map_path=system_map,
        timing_report=report,
    )

    assert result == 200_000_000
    assert timing_freq.read_system_map_clock_hz(system_map) == 200_000_000


def test_missing_report_keeps_target(tmp_path):
    target_hz = 300_000_000
    system_map = tmp_path / "system_map.xml"
    _write_system_map(system_map, target_hz)

    result = timing_freq.apply_timing_frequency_cap(
        project_name="proj",
        system_map_path=system_map,
        timing_report=tmp_path / "does_not_exist.txt",
    )

    assert result == target_hz
    assert timing_freq.read_system_map_clock_hz(system_map) == target_hz


def test_missed_target_warns_on_stderr(tmp_path, capsys):
    # Missing the target is unintuitive enough to warrant more than a log line:
    # an over-constrained run routes worse, so the derated result can come out
    # below what a lower --clock-hz would have delivered. Verify the user is
    # told, and that the message names both frequencies.
    target_hz = 250_000_000
    system_map = tmp_path / "system_map.xml"
    report = tmp_path / "report_timing_proj.txt"
    _write_system_map(system_map, target_hz)
    _write_timing_report(report, wns_ns=-1.0)

    timing_freq.apply_timing_frequency_cap(
        project_name="proj",
        system_map_path=system_map,
        timing_report=report,
    )

    err = capsys.readouterr().err
    assert "user clock target not met" in err
    assert str(target_hz) in err
    assert "200000000" in err
    assert "--clock-hz" in err


def test_met_target_does_not_warn(tmp_path, capsys):
    # The warning must stay quiet when the design closes, otherwise it becomes
    # noise that gets filtered out.
    target_hz = 250_000_000
    system_map = tmp_path / "system_map.xml"
    report = tmp_path / "report_timing_proj.txt"
    _write_system_map(system_map, target_hz)
    _write_timing_report(report, wns_ns=1.0)

    timing_freq.apply_timing_frequency_cap(
        project_name="proj",
        system_map_path=system_map,
        timing_report=report,
    )

    assert "target not met" not in capsys.readouterr().err


def test_slacks_come_from_the_named_columns(tmp_path):
    # WHS must be read from the WHS(ns) column, not from a fixed offset. On a
    # real report offset 2 is "TNS Failing Endpoints", so an offset-based parser
    # reports the failing-endpoint count as the hold slack.
    report = tmp_path / "report_timing_proj.txt"
    _write_timing_report(report, wns_ns=-0.030, whs_ns=0.000)

    slacks = timing_freq.extract_design_timing_slacks_ns(
        report.read_text(encoding="utf-8"))

    assert slacks == (-0.030, 0.000)


def test_hold_violation_fails_the_gate(tmp_path):
    # A design that meets setup but violates hold must not pass. This is the
    # case the offset bug hid: the count it returned instead of WHS is never
    # negative, so the whs >= 0 check could never fire.
    build_dir = tmp_path
    report = build_dir / "report_timing_proj.txt"
    _write_timing_report(report, wns_ns=0.500, whs_ns=-0.012)

    assert timing_freq.extract_design_timing_slacks_ns(
        report.read_text(encoding="utf-8")) == (0.500, -0.012)
    assert not timing_freq.design_timing_met(0.500, -0.012)

    with pytest.raises(RuntimeError, match="timing failed"):
        timing_freq.require_static_shell_timing_or_confirm(
            build_dir=build_dir,
            project_name="proj",
            ignore_failure=False,
            noninteractive=True,
        )
