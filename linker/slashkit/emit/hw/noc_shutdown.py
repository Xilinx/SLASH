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
Partial device images that shut down the NoC ports of their region first.

write_device_image orders each SLR's part of a partial image as: the global
signal masks of the frames the new module uses ("unmask"), the NPI shutdown of
the region's NoC ports, the clearing of the region, its configuration, and the
masks again. The first step runs while the old module is still running with its
NoC ports open. Where the new layout covers the logic that drives one of those
ports, the old logic can raise a request that never completes; the shutdown
that follows then waits for the port until it times out, and the slave SLRs
wait for the master forever. Which pairs of modules do this depends only on
placement. On the V80 it was a service-layer module whose logic sat on the
sites of the default service layer's register slice in front of
NOC_NMU512_X1Y6; loading a module over itself never does it, since the frames
keep their contents.

The shutdown is defined by the static shell, the same for every module of a
region, and it can run twice: on a port that is already shut down it finds no
request pending and writes the same values again. So every SLR runs it once
more before anything else, and SSIT syncs keep the SLRs in step:

    master                          slave SLRs
    shutdown of its ports           wait
    sync      ------------------->  shutdown of their ports
    sync      <-------------------  sync
    the image as written            the image as written

The image is rebuilt with bootgen from the BIF and the raw partition files that
write_device_image leaves next to it.
"""
from __future__ import annotations

import logging
import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from slashkit.core.launcher import TASK_BOOTGEN, run_tool

logger = logging.getLogger(__name__)

#: CDO commands (PLM module 1): a slave SLR waits for the master; the master
#: waits for the slaves in its mask, then lets them go
SSIT_SYNC_MASTER = 0x0000010E
SSIT_SYNC_SLAVES = 0x0002010F
#: the timeout write_device_image gives its own master syncs
SSIT_SYNC_TIMEOUT = 0x2DC6C0
#: partition ID of the shutdown in each SLR's image
SHUTDOWN_PARTITION = 0x205
#: partition ID of the shutdown run first
EARLY_SHUTDOWN_PARTITION = 0x005

_CDO_MAGIC = 0x004F4443
_HEX_WORD = re.compile(r"^[0-9a-fA-F]{8}$")


def read_ascii_cdo(path: Path) -> Tuple[List[str], List[int]]:
    """An ASCII device image (.rcdo, .rnpi): its header lines and its words,
    the 5-word CDO header first."""
    lines = path.read_text(encoding="ascii").splitlines()
    i = 0
    while i < len(lines) and not _HEX_WORD.match(lines[i].strip()):
        i += 1
    words = [int(w, 16) for w in (l.strip() for l in lines[i:]) if w]
    if len(words) < 5 or words[1] != _CDO_MAGIC or words[0] != 4:
        raise ValueError(f"{path}: not a CDO device image")
    if words[3] != len(words) - 5:
        raise ValueError(
            f"{path}: CDO length {words[3]} does not match {len(words) - 5} words")
    return lines[:i], words


def cdo_body(words: List[int]) -> List[int]:
    return words[5:]


def write_ascii_cdo(path: Path, header: List[str], version: int, body: List[int]) -> None:
    """The ASCII device image of @p body, with its CDO header (length,
    checksum) and the Bits line made to match."""
    head = [4, _CDO_MAGIC, version, len(body)]
    words = head + [~sum(head) & 0xFFFFFFFF] + body
    out = []
    for line in header:
        if line.startswith("Bits:"):
            line = f"Bits:        \t{32 * len(words)}"
        out.append(line)
    out += [f"{w:08x}" for w in words]
    path.write_text("\n".join(out) + "\n", encoding="ascii")


def _sections(bif: str) -> Dict[str, Tuple[int, int]]:
    """Top-level BIF sections, name -> (start, end) in the text."""
    out = {}
    for m in re.finditer(r"^(\w+):\s*\{", bif, re.M):
        depth, i = 0, m.end() - 1
        while i < len(bif):
            if bif[i] == "{":
                depth += 1
            elif bif[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out[m.group(1)] = (m.start(), i + 1)
    return out


def _partitions(text: str) -> List[Dict[str, str]]:
    out = []
    for m in re.finditer(r"partition\s*\{([^{}]*)\}", text):
        fields = dict(re.findall(r"(\w+)\s*=\s*(\S+)", m.group(1)))
        fields["_start"] = str(m.start())
        out.append(fields)
    return out


def _resolve(p: str, cwd: Path) -> Path:
    path = Path(p)
    return path if path.is_absolute() else cwd / path


def rewrite_bif(bif_path: Path, cwd: Path) -> Optional[Path]:
    """
    Write @p bif_path's image with the shutdown run first in every SLR: new
    partition files and a new BIF next to it, whose path is returned (None when
    the BIF has no layout this knows). Relative file names in the BIF are
    resolved against @p cwd, as bootgen does.
    """
    bif = bif_path.read_text()
    sections = _sections(bif)
    master = None
    for name, (a, b) in sections.items():
        if "CONFIG_MASTER" in bif[a:b]:
            master = name
    if master is None:
        return None
    a, b = sections[master]
    parts = _partitions(bif[a:b])
    combined = [p for p in parts if p.get(
        "file", "").endswith("_combined_0.rcdo")]
    if len(combined) != 1:
        return None
    combined_file = _resolve(combined[0]["file"], cwd)
    stem = combined_file.name[: -len("_combined_0.rcdo")]
    out_dir = bif_path.parent
    # slave SLR -> its section
    slaves = {int(p["slr"]): p["section"] for p in parts
              if "section" in p and "slr" in p and int(p["slr"]) > 0}

    edits = []   # (position in bif, text to insert)
    for slr, sec in sorted(slaves.items()):
        if sec not in sections:
            return None
        sa, sb = sections[sec]
        sparts = _partitions(bif[sa:sb])
        if not sparts:
            return None
        body: List[int] = []
        version = 0x200
        header = ["Xilinx ASCII NPI Deviceimage", "Bits:        \t0"]
        down = [p for p in sparts if int(
            p.get("id", "0"), 16) == SHUTDOWN_PARTITION]
        if down:
            header, words = read_ascii_cdo(_resolve(down[0]["file"], cwd))
            version, body = words[2], cdo_body(words)
        f = out_dir / f"{stem}_noc_shutdown_{slr}.rnpi"
        write_ascii_cdo(f, header, version, [
                        SSIT_SYNC_MASTER] + body + [SSIT_SYNC_MASTER])
        first = sa + int(sparts[0]["_start"])
        edits.append((first, "partition\n  {\n   id = 0x%03x\n   type = cdo\n   file = %s\n  }\n  "
                      % (EARLY_SHUTDOWN_PARTITION, f)))

    header, words = read_ascii_cdo(combined_file)
    pre: List[int] = []
    shutdown0 = combined_file.with_name(f"{stem}_shutdown_0.rnpi")
    if shutdown0.is_file():
        pre = cdo_body(read_ascii_cdo(shutdown0)[1])
    if slaves:
        mask = sum(1 << (slr - 1) for slr in slaves)
        pre += [SSIT_SYNC_SLAVES, mask, SSIT_SYNC_TIMEOUT] * 2
    new_combined = out_dir / f"{stem}_combined_0_noc_shutdown.rcdo"
    write_ascii_cdo(new_combined, header, words[2], pre + cdo_body(words))

    out = bif
    for pos, text in sorted(edits, reverse=True):
        out = out[:pos] + text + out[pos:]
    out = out.replace(combined[0]["file"], str(new_combined))
    new_bif = out_dir / f"{bif_path.stem}_noc_shutdown.bif"
    new_bif.write_text(out)
    return new_bif


def rebuild_partial(pdi: Path, cwd: Path) -> bool:
    """
    Rebuild the partial image @p pdi so that every SLR shuts down the region's
    NoC ports before anything else (see the module doc). Its BIF and raw
    partitions are next to it; @p cwd is where write_device_image ran, which
    relative names in the BIF are relative to. False, with a warning, when the
    image is left as it is.
    """
    bif = pdi.with_suffix(".bif")
    if not bif.is_file():
        logger.warning(
            "%s: no BIF next to it, NoC ports not shut down first", pdi)
        return False
    new_bif = rewrite_bif(bif, cwd)
    if new_bif is None:
        logger.warning(
            "%s: unknown image layout, NoC ports not shut down first", pdi)
        return False
    logger.info("Rebuilding %s with the NoC ports shut down first", pdi.name)
    run_tool(["bootgen", "-arch", "versal", "-image", str(new_bif), "-w", "-o", str(pdi)],
             task=TASK_BOOTGEN, cwd=cwd)
    return True
