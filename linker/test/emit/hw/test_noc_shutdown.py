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
from pathlib import Path

import pytest

from slashkit.emit.hw import noc_shutdown as ns

STEM = "top_i_service_layer_service_layer_x_inst_0_partial"

# The layout write_device_image gives a partial of a region on three SLRs
BIF = """bitstream_1:
{{
 id_code = 0x14d2f093
 type = slr-config
 image
 {{
  name = pl_cfi
  id = 0x18700002
  partition
  {{
   id = 0x103
   type = cdo
   file = images/{s}_unmask_1.rcdo
  }}
  partition
  {{
   id = 0x205
   type = cdo
   file = images/{s}_shutdown_1.rnpi
  }}
 }}
}}

bitstream_2:
{{
 type = slr-config
 image
 {{
  name = pl_cfi
  id = 0x18700002
  partition
  {{
   id = 0x103
   type = cdo
   file = images/{s}_unmask_2.rcdo
  }}
  partition
  {{
   id = 0x205
   type = cdo
   file = images/{s}_shutdown_2.rnpi
  }}
 }}
}}

bitstream_master:
{{
 id = 0x2
 image
 {{
  name = CONFIG_MASTER
  id = 0x18700002
  type = slr-config
  partition
  {{
   id = 0x116
   slr = 1
   section = bitstream_1
  }}
  partition
  {{
   id = 0x116
   slr = 2
   section = bitstream_2
  }}
  partition
  {{
   id = 0x100
   type = cdo
   slr = 0
   file = images/{s}_combined_0.rcdo
  }}
 }}
}}
""".format(s=STEM)

HEADER = ["Xilinx ASCII NPI Deviceimage", "Design name: \ttop_wrapper;PARTIAL=TRUE",
          "Bits:        \t0"]


def _cdo(path: Path, body):
    ns.write_ascii_cdo(path, HEADER, 0x200, body)


def _image(tmp_path):
    images = tmp_path / "images"
    images.mkdir()
    (images / f"{STEM}.bif").write_text(BIF)
    _cdo(images / f"{STEM}_shutdown_0.rnpi", [0xA0, 0xA1])
    _cdo(images / f"{STEM}_shutdown_1.rnpi", [0xB0, 0xB1, 0xB2])
    _cdo(images / f"{STEM}_shutdown_2.rnpi", [0xC0])
    _cdo(images / f"{STEM}_combined_0.rcdo", [0x10F, 0x11, 0x22])
    (images / f"{STEM}.pdi").write_bytes(b"\0")
    return images


def test_ascii_cdo_checksum_matches_write_device_image(tmp_path):
    # the header of a combined_0.rcdo that write_device_image wrote
    f = tmp_path / "x.rcdo"
    ns.write_ascii_cdo(f, HEADER, 0x200, [0] * 0x35244C)
    _, words = ns.read_ascii_cdo(f)
    assert words[:5] == [0x4, 0x004F4443, 0x200, 0x35244C, 0xFF7B956C]


def test_ascii_cdo_round_trip(tmp_path):
    f = tmp_path / "x.rnpi"
    _cdo(f, [1, 2, 3])
    header, words = ns.read_ascii_cdo(f)
    assert ns.cdo_body(words) == [1, 2, 3]
    assert words[3] == 3
    assert words[4] == ~sum(words[:4]) & 0xFFFFFFFF
    assert header[-1] == f"Bits:        \t{32 * 8}"


def test_ascii_cdo_with_a_wrong_length_is_refused(tmp_path):
    f = tmp_path / "x.rnpi"
    _cdo(f, [1, 2, 3])
    f.write_text(f.read_text() + "00000004\n")
    with pytest.raises(ValueError):
        ns.read_ascii_cdo(f)


def test_slaves_wait_then_shut_down_then_sync(tmp_path):
    images = _image(tmp_path)
    new_bif = ns.rewrite_bif(images / f"{STEM}.bif", tmp_path)
    for slr, body in ((1, [0xB0, 0xB1, 0xB2]), (2, [0xC0])):
        _, words = ns.read_ascii_cdo(
            images / f"{STEM}_noc_shutdown_{slr}.rnpi")
        assert ns.cdo_body(words) == [
            ns.SSIT_SYNC_MASTER] + body + [ns.SSIT_SYNC_MASTER]
    # first in each slave's image
    text = new_bif.read_text()
    for slr in (1, 2):
        sec = text[text.index(f"bitstream_{slr}:"):]
        first = sec[sec.index("partition"):]
        assert first.index(
            f"{STEM}_noc_shutdown_{slr}.rnpi") < first.index("0x103")


def test_master_shuts_down_then_releases_the_slaves_twice(tmp_path):
    images = _image(tmp_path)
    new_bif = ns.rewrite_bif(images / f"{STEM}.bif", tmp_path)
    _, words = ns.read_ascii_cdo(
        images / f"{STEM}_combined_0_noc_shutdown.rcdo")
    sync = [ns.SSIT_SYNC_SLAVES, 0x3, ns.SSIT_SYNC_TIMEOUT]
    assert ns.cdo_body(words) == [0xA0, 0xA1] + \
        sync + sync + [0x10F, 0x11, 0x22]
    text = new_bif.read_text()
    assert f"{STEM}_combined_0_noc_shutdown.rcdo" in text
    assert f"images/{STEM}_combined_0.rcdo" not in text
    # the original files stay as they were
    _, orig = ns.read_ascii_cdo(images / f"{STEM}_combined_0.rcdo")
    assert ns.cdo_body(orig) == [0x10F, 0x11, 0x22]


def test_a_slave_without_a_shutdown_still_syncs(tmp_path):
    images = _image(tmp_path)
    bif = images / f"{STEM}.bif"
    bif.write_text(bif.read_text().replace(
        f"   id = 0x205\n   type = cdo\n   file = images/{STEM}_shutdown_2.rnpi",
        f"   id = 0x203\n   type = cdo\n   file = images/{STEM}_shutdown_2.rnpi"))
    ns.rewrite_bif(bif, tmp_path)
    _, words = ns.read_ascii_cdo(images / f"{STEM}_noc_shutdown_2.rnpi")
    assert ns.cdo_body(words) == [ns.SSIT_SYNC_MASTER, ns.SSIT_SYNC_MASTER]


def test_unknown_layout_is_left_alone(tmp_path):
    images = tmp_path / "images"
    images.mkdir()
    (images / "a.bif").write_text("all:\n{\n image\n {\n  name = x\n }\n}\n")
    assert ns.rewrite_bif(images / "a.bif", tmp_path) is None


def test_rebuild_partial_rebuilds_the_image_with_bootgen(tmp_path, monkeypatch):
    images = _image(tmp_path)
    calls = []
    monkeypatch.setattr(ns, "run_tool", lambda cmd, **
                        kw: calls.append((cmd, kw)))
    pdi = images / f"{STEM}.pdi"
    assert ns.rebuild_partial(pdi, tmp_path)
    (cmd, kw), = calls
    assert cmd[:4] == ["bootgen", "-arch", "versal", "-image"]
    assert cmd[4].endswith(f"{STEM}_noc_shutdown.bif")
    assert cmd[-2:] == ["-o", str(pdi)]
    assert kw["cwd"] == tmp_path and kw["task"] == ns.TASK_BOOTGEN


def test_rebuild_partial_without_a_bif_leaves_the_image(tmp_path, monkeypatch):
    monkeypatch.setattr(ns, "run_tool", lambda *a, **
                        k: pytest.fail("bootgen ran"))
    pdi = tmp_path / "x.pdi"
    pdi.write_bytes(b"\0")
    assert not ns.rebuild_partial(pdi, tmp_path)
