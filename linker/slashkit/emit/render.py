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

from __future__ import annotations
from jinja2 import Environment, PackageLoader, StrictUndefined
from pathlib import Path
from importlib import resources
import shutil


def render_template(template: str | Path, out_path: str | Path, context: dict) -> None:
    env = Environment(
        loader=PackageLoader("slashkit.resources"),
        undefined=StrictUndefined,
        trim_blocks=True,
        lstrip_blocks=True,
    )
    env.filters["zip"] = lambda a, b: zip(a, b)
    tmpl = env.get_template(template)
    Path(out_path).write_text(tmpl.render(**context), encoding="utf-8")


def export_package(package, out_dir: str | Path) -> None:
    def impl(traversable, out_path: Path) -> None:
        if traversable.is_file():
            with resources.as_file(traversable) as in_path:
                shutil.copy(in_path, out_path)
        elif traversable.is_dir():
            out_path.mkdir()
            for sub_traversable in traversable.iterdir():
                impl(sub_traversable, out_path / sub_traversable.name)

    impl(resources.files(package), out_dir)
