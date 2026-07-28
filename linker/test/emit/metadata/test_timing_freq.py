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

from slashkit.emit.metadata import timing_freq


def _write_system_map(path: Path, clock_hz: int) -> None:
    path.write_text(
        f'<?xml version="1.0"?>\n<SystemMap>\n'
        f'  <ClockFrequency>{clock_hz}</ClockFrequency>\n</SystemMap>\n',
        encoding="utf-8",
    )


def _write_timing_report(path: Path, wns_ns: float) -> None:
    # Minimal shape matching what extract_design_wns_ns() scans for: a
    # "Design Timing Summary" section, a WNS(ns)/TNS(ns) header, then a data row
    # whose first value is WNS and third is WHS.
    path.write_text(
        textwrap.dedent(
            f"""\
            Design Timing Summary
            ---------------------

              WNS(ns)      TNS(ns)  WHS(ns)  THS(ns)
              -------      -------  -------  -------
              {wns_ns:.3f}      0.000    0.100    0.000
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
