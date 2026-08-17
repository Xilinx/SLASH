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

"""Tests for the optional compute-farm dispatch of external tool invocations."""

import logging
import os

import pytest

from slashkit.core import launcher


@pytest.fixture(autouse=True)
def _clean_launcher_env(monkeypatch):
    monkeypatch.delenv(launcher.LAUNCHER_ENV, raising=False)
    monkeypatch.delenv(launcher._LEGACY_LAUNCHER_ENV, raising=False)


@pytest.fixture
def captured_run(monkeypatch):
    """Replace subprocess.run and hand back the call it received."""
    calls = []
    monkeypatch.setattr(launcher.subprocess, "run",
                        lambda argv, **kwargs: calls.append((argv, kwargs)))
    return calls


def test_no_launcher_by_default():
    assert launcher.tool_launcher() == []


def test_launcher_is_split_like_a_shell_would(monkeypatch):
    monkeypatch.setenv(launcher.LAUNCHER_ENV, "submit --queue 'a queue'")
    assert launcher.tool_launcher() == ["submit", "--queue", "a queue"]


def test_legacy_variable_is_ignored_but_warns(monkeypatch, caplog):
    monkeypatch.setenv(launcher._LEGACY_LAUNCHER_ENV, "submit")
    with caplog.at_level(logging.WARNING):
        assert launcher.tool_launcher() == []
    assert launcher.LAUNCHER_ENV in caplog.text


def test_new_variable_silences_the_legacy_warning(monkeypatch, caplog):
    monkeypatch.setenv(launcher._LEGACY_LAUNCHER_ENV, "old")
    monkeypatch.setenv(launcher.LAUNCHER_ENV, "new")
    with caplog.at_level(logging.WARNING):
        assert launcher.tool_launcher() == ["new"]
    assert caplog.text == ""


def test_runs_locally_when_unset(captured_run, tmp_path):
    launcher.run_tool(["vivado", "-version"],
                      task=launcher.TASK_BOOTGEN, cwd=tmp_path)
    argv, kwargs = captured_run[0]
    assert argv == ["vivado", "-version"]
    assert kwargs["cwd"] == str(tmp_path)
    assert kwargs["check"] is True


def test_launcher_is_prefixed_and_task_exported(captured_run, tmp_path, monkeypatch):
    monkeypatch.setenv(launcher.LAUNCHER_ENV, "submit -K")
    launcher.run_tool(["bootgen", "-arch", "versal"],
                      task=launcher.TASK_BOOTGEN, cwd=tmp_path)
    argv, kwargs = captured_run[0]
    assert argv == ["submit", "-K", "bootgen", "-arch", "versal"]
    assert kwargs["env"]["SLASH_BUILD_TASK"] == "bootgen"
    assert kwargs["env"]["SLASH_TOOL_CWD"] == str(tmp_path)


def test_arguments_are_stringified(captured_run, tmp_path):
    launcher.run_tool(["tool", tmp_path / "in.bif", 8],
                      task=launcher.TASK_AVED, cwd=tmp_path)
    argv, _ = captured_run[0]
    assert argv == ["tool", str(tmp_path / "in.bif"), "8"]


def test_caller_environment_is_not_mutated(captured_run, tmp_path):
    supplied = {"KEEP": "yes"}
    launcher.run_tool(["tool"], task=launcher.TASK_AVED,
                      cwd=tmp_path, env=supplied)
    _, kwargs = captured_run[0]
    assert kwargs["env"]["KEEP"] == "yes"
    assert kwargs["env"]["SLASH_BUILD_TASK"] == "aved"
    # The dict the caller passed in, and the real environment, stay clean.
    assert supplied == {"KEEP": "yes"}
    assert "SLASH_BUILD_TASK" not in os.environ


def test_supplied_environment_replaces_rather_than_extends(captured_run, tmp_path, monkeypatch):
    """A scrubbed env (see _clean_cross_build_env) must not be re-polluted."""
    monkeypatch.setenv("CFLAGS", "-fstack-clash-protection")
    launcher.run_tool(["bash", "build_all.sh"], task=launcher.TASK_AVED,
                      cwd=tmp_path, env={"PATH": "/usr/bin"})
    _, kwargs = captured_run[0]
    assert "CFLAGS" not in kwargs["env"]
