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

"""Tests for the LD_PRELOAD environment helper in emit.hw.project_gen."""

from pathlib import Path

from slashkit.emit.hw import project_gen

LIBUDEV = "/lib/x86_64-linux-gnu/libudev.so.1"
LIBCAP = "/lib/x86_64-linux-gnu/libcap.so.2"


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
