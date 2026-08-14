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

"""
The DCMAC assets come from a git submodule that lives outside the slashkit
package, so they are the one part of a service-shell build that can silently
break when slashkit is installed from a wheel/deb/rpm rather than run from a
checkout. These tests pin the two properties that keep that working.
"""

from importlib import resources
from pathlib import Path

import pytest

from slashkit.emit.hw.service_region import service_layer_ctx
from slashkit.emit.hw.service_region.service_layer_ctx import (
    dcmac_paths,
    stage_versal_dcmac,
)


def test_dcmac_paths_stay_inside_the_build_dir(tmp_path):
    """Nothing emitted into the Tcl may point back at the slashkit install."""
    dcmac_dir = tmp_path / "build" / "dcmac"

    for key, value in dcmac_paths(dcmac_dir).items():
        assert Path(value).is_relative_to(dcmac_dir), \
            f"{key} escapes the build directory: {value}"


def test_stage_versal_dcmac_accepts_an_already_staged_copy(tmp_path, monkeypatch):
    """A packaged install carries the sources; export_package has copied them."""
    dest = tmp_path / "versal"
    (dest / "tcl").mkdir(parents=True)
    (dest / "tcl" / "dcmac.tcl").write_text("# staged\n")

    # No submodule anywhere - the staged copy has to be enough.
    absent = tmp_path / "absent"
    monkeypatch.setattr(service_layer_ctx, "_DCMAC_VERSAL", absent)

    assert stage_versal_dcmac(dest) == dest
    assert (dest / "tcl" / "dcmac.tcl").read_text() == "# staged\n"


def test_stage_versal_dcmac_falls_back_to_the_submodule(tmp_path, monkeypatch):
    """A source checkout copies hdl/ and tcl/ out of submodules/Versal-DCMAC."""
    submodule = tmp_path / "Versal-DCMAC"
    (submodule / "tcl").mkdir(parents=True)
    (submodule / "hdl").mkdir()
    (submodule / "tcl" / "dcmac.tcl").write_text("# submodule\n")
    (submodule / "hdl" / "clock_utils.v").write_text("// rtl\n")
    # Not needed by the build, and not worth shipping.
    (submodule / "images").mkdir()

    monkeypatch.setattr(service_layer_ctx, "_DCMAC_VERSAL", submodule)

    dest = tmp_path / "build" / "dcmac" / "versal"
    stage_versal_dcmac(dest)

    assert (dest / "tcl" / "dcmac.tcl").read_text() == "# submodule\n"
    assert (dest / "hdl" / "clock_utils.v").exists()
    assert not (dest / "images").exists()


def test_stage_versal_dcmac_reports_the_submodule_init_command(tmp_path, monkeypatch):
    absent = tmp_path / "absent"
    monkeypatch.setattr(service_layer_ctx, "_DCMAC_VERSAL", absent)

    with pytest.raises(FileNotFoundError) as excinfo:
        stage_versal_dcmac(tmp_path / "build" / "dcmac" / "versal")

    message = str(excinfo.value)
    assert "git submodule update --init submodules/Versal-DCMAC" in message


def test_dcmac_tcl_is_shipped_in_the_package():
    """slash_wrapper.tcl is package data; only versal/ is staged separately."""
    wrapper = (
        resources.files("slashkit.resources.dcmac.tcl")
        .joinpath("slash_wrapper.tcl")
        .read_text()
    )

    # The BD hierarchy the submodule actually builds - top.tcl assigns the CSR
    # windows against the same paths.
    assert "qsfp_0_n_1/dcmac_wrapper/dcmac_0/s_axi/Reg" in wrapper
    assert "qsfp_2_n_3/dcmac_wrapper/dcmac_1/s_axi/Reg" in wrapper
    assert "DCMAC_subsys" not in wrapper


def test_static_shell_tcl_has_no_stale_dcmac_hierarchy():
    """The old DCMAC_subsys hierarchy no longer exists in the block design."""
    for package, name in (
        ("slashkit.resources.base.service.scripts", "top.tcl"),
        ("slashkit.resources.base.service.scripts", "service_layer.tcl"),
        ("slashkit.resources.base.service.constraints", "opt.post.tcl"),
        ("slashkit.resources.base.service.constraints.service_layer.eth",
         "service_layer_eth.opt.post.tcl"),
    ):
        text = resources.files(package).joinpath(name).read_text()
        assert "DCMAC_subsys" not in text, name + " references DCMAC_subsys"
