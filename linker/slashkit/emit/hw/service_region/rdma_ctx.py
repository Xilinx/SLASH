# SPDX-License-Identifier: MIT
"""
The RDMA service of the service layer ([network] rdma=eth_<n>): staging of its
sources and of the RoCE v2 stack IP into the build directory, and the template
context of service_layer.tcl.

The stack is the packaged IP rdma_200 of fpga-network-stack (submodule
submodules/fpga-network-stack, `make rdma_200`). It is looked up, in order, in
  1. $SLASH_RDMA_IPREPO/rdma_200            (an explicit IP repository)
  2. slashkit/resources/rdma/iprepo/rdma_200 (staged by scripts/stage-rdma.sh,
                                             also what a packaged install carries)
  3. submodules/fpga-network-stack/build/iprepo/rdma_200 (a source checkout
                                             built in place)
"""
from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import Any, Dict, Optional

from slashkit.emit.render import export_package

_SLASH_ROOT = Path(__file__).resolve().parents[5]
_FNS = _SLASH_ROOT / "submodules" / "fpga-network-stack"
_RESOURCES = Path(__file__).resolve().parents[3] / "resources" / "rdma"

RDMA_IP = "rdma_200"
#! BUILD register of rdma_service: raise it when the service's RTL changes.
RDMA_BUILD = 2
#! DCMAC hierarchy per Ethernet port
_QSFP = {0: "qsfp_0_n_1", 2: "qsfp_2_n_3"}

_RDMA_IP_MISSING = (
    "The RDMA stack IP {ip} was not found (looked in: {looked}). Build it from "
    "the fpga-network-stack submodule and stage it:\n"
    "    git submodule update --init submodules/fpga-network-stack\n"
    "    scripts/stage-rdma.sh\n"
    "or point SLASH_RDMA_IPREPO at an IP repository that holds {ip}/."
)


def rdma_ip_candidates() -> list[Path]:
    out = []
    env = os.environ.get("SLASH_RDMA_IPREPO")
    if env:
        out.append(Path(env).expanduser() / RDMA_IP)
    out.append(_RESOURCES / "iprepo" / RDMA_IP)
    out.append(_FNS / "build" / "iprepo" / RDMA_IP)
    return out


def find_rdma_ip() -> Path:
    candidates = rdma_ip_candidates()
    for c in candidates:
        if (c / "component.xml").is_file():
            return c
    raise FileNotFoundError(_RDMA_IP_MISSING.format(
        ip=RDMA_IP, looked=", ".join(str(c) for c in candidates)))


def stage_rdma(dest: Path) -> Path:
    """
    Copy the RDMA service resources (hdl/, tcl/, constraints/) to *dest*, with
    the stack IP under dest/iprepo/rdma_200, so the build directory does not
    point back at the checkout or the installed package.
    """
    ip = find_rdma_ip()
    if dest.exists():
        shutil.rmtree(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    export_package("slashkit.resources.rdma", dest)
    ip_dest = dest / "iprepo" / RDMA_IP
    if not (ip_dest / "component.xml").is_file():
        shutil.copytree(ip, ip_dest, dirs_exist_ok=True)
    return dest


def build_service_rdma_ctx(net, rdma_dir: Optional[Path] = None) -> Dict[str, Any]:
    rdma = getattr(net, "rdma", None)
    if rdma is None:
        return {"rdma_enabled": False}
    if rdma_dir is None:
        raise ValueError("build_service_rdma_ctx: the staged rdma directory is needed")
    return {
        "rdma_enabled": True,
        "rdma_eth": rdma,
        "rdma_qsfp": _QSFP[rdma],
        "rdma_build": RDMA_BUILD,
        "rdma_dir": str(rdma_dir),
        "rdma_tcl": str(rdma_dir / "tcl" / "rdma_service.tcl"),
    }


def write_rdma_opt_post(build_dir: Path, base_hook: Path, rdma: int) -> Path:
    """
    The service-layer build takes one opt_design post hook: SLASH's, then the
    RDMA port's constraints (resources/rdma/constraints/rdma.opt.post.tcl).
    service_layer_build.tcl finds the hook by its file name, so it keeps
    SLASH's name, in a directory of its own.
    """
    out = build_dir / "rdma" / "opt_post" / "service_layer_eth.opt.post.tcl"
    out.parent.mkdir(parents=True, exist_ok=True)
    hook = build_dir / "rdma" / "constraints" / "rdma.opt.post.tcl"
    out.write_text(
        f"source {{{base_hook}}}\n"
        f"set ::slash_rdma_eth {rdma}\n"
        f"source {{{hook}}}\n",
        encoding="utf-8")
    return out
