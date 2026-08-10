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

"""Tests for slashkit static-shell-path shell type selection."""

import argparse
from contextlib import nullcontext

import slashkit.__main__ as slashkit_main


class FakeTraversable:
    def __init__(self, package, name=""):
        self.package = package
        self.name = name

    def __truediv__(self, name):
        return FakeTraversable(self.package, name)

    def is_file(self):
        return True

    def resolve(self):
        return f"/fake/{self.package}/{self.name}"


def _run_static_shell_path(monkeypatch, capsys, *, shelltype="service", nofpt=False):
    packages = []

    def fake_files(package):
        packages.append(package)
        return FakeTraversable(package)

    monkeypatch.setattr(slashkit_main.resources, "files", fake_files)
    monkeypatch.setattr(
        slashkit_main.resources,
        "as_file",
        lambda traversable: nullcontext(traversable),
    )

    slashkit_main.static_shell_path(
        argparse.Namespace(shell_type=shelltype, nofpt=nofpt)
    )

    return packages, capsys.readouterr().out.strip()


def test_static_shell_path_defaults_to_service(monkeypatch, capsys):
    packages, path = _run_static_shell_path(monkeypatch, capsys)

    assert packages == ["slashkit.resources.static_shell"]
    assert path == "/fake/slashkit.resources.static_shell/amd_v80_gen5x8_25.1.pdi"


def test_static_shell_path_selects_compute_nofpt(monkeypatch, capsys):
    packages, path = _run_static_shell_path(
        monkeypatch,
        capsys,
        shelltype="compute",
        nofpt=True,
    )

    assert packages == ["slashkit.resources.static_shell_compute"]
    assert path == (
        "/fake/slashkit.resources.static_shell_compute/"
        "amd_v80_gen5x8_25.1_nofpt.pdi"
    )
