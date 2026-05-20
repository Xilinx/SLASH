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

from pathlib import Path
import json
import logging
import os
import re
import shlex
import shutil
import subprocess
import tarfile
from typing import Iterable

from slashkit.emit.hls_meta import infer_hls_json_from_component_xml
from slashkit.core.command_config import LinkerConfiguration

logger = logging.getLogger(__name__)


def _find_vitis_include() -> Path:
    env_candidates = [
        os.environ.get("XILINX_VITIS"),
        os.environ.get("VITIS_HOME"),
        os.environ.get("VITIS"),
    ]
    for base in env_candidates:
        if not base:
            continue
        cand = Path(base) / "include"
        if cand.exists():
            return cand

    vitis_bin = shutil.which("vitis")
    if vitis_bin:
        return Path(vitis_bin).resolve().parents[1] / "include"

    raise FileNotFoundError(
        "Could not locate Vitis include path. Set XILINX_VITIS/VITIS_HOME "
        "or ensure 'vitis' is on PATH."
    )


def _collect_kernel_cpp(config: LinkerConfiguration) -> list[Path]:
    cpp_files: list[Path] = []
    seen: set[Path] = set()

    for kpath in config.kernel_component_paths:
        if not kpath.exists():
            raise FileNotFoundError(f"Kernel component.xml not found: {kpath}")
        # component.xml -> ip -> impl -> <solution>
        sol_dir = kpath.parents[2]
        build_dir = sol_dir.parent

        # Prefer the original kernel sources in the build dir (e.g., build_increment.../*.cpp).
        candidates = list(build_dir.glob("*.cpp"))
        if not candidates:
            # Fallbacks for other layouts.
            candidates = list(sol_dir.glob("*.cpp"))
        if not candidates:
            candidates = list((kpath.parent.parent).glob("*.cpp"))

        for cpp in sorted(candidates):
            if cpp not in seen:
                seen.add(cpp)
                cpp_files.append(cpp)

    return cpp_files


_LOCAL_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])\s*([^">]+?)\s*[">]')
_USER_HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inc"}
_USER_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".C"}


def _dedupe_paths(paths: Iterable[Path]) -> list[Path]:
    out: list[Path] = []
    seen: set[Path] = set()
    for p in paths:
        rp = p.resolve()
        if not rp.exists():
            continue
        if rp in seen:
            continue
        seen.add(rp)
        out.append(rp)
    return out


def _extract_include_dirs_from_cfg(cfg_path: Path) -> list[Path]:
    """
    Parse syn.cflags from an HLS cfg file and extract include directories.
    Relative paths are resolved relative to cfg_path.parent (the HLS build dir).
    """
    include_dirs: list[Path] = []
    if not cfg_path.exists():
        return include_dirs

    def _append_dir(raw: str) -> None:
        raw = raw.strip()
        if not raw:
            return
        p = Path(raw)
        if not p.is_absolute():
            p = (cfg_path.parent / p).resolve()
        if p.exists():
            include_dirs.append(p)

    for raw_line in cfg_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.strip() != "syn.cflags":
            continue
        try:
            toks = shlex.split(value)
        except ValueError:
            toks = value.split()

        i = 0
        while i < len(toks):
            t = toks[i]
            if t == "-I":
                if i + 1 < len(toks):
                    _append_dir(toks[i + 1])
                    i += 2
                    continue
            elif t.startswith("-I") and len(t) > 2:
                _append_dir(t[2:])
            elif t in ("-isystem", "-iquote"):
                if i + 1 < len(toks):
                    _append_dir(toks[i + 1])
                    i += 2
                    continue
            i += 1

    return _dedupe_paths(include_dirs)


def _resolve_hls_c_sources(hls_json_path: Path) -> list[Path]:
    """
    Resolve original C/C++ sources from hls_data.json Files.CSource.
    Falls back to build-dir .cpp scan if metadata is missing/partial.
    """
    sources: list[Path] = []
    try:
        d = json.loads(hls_json_path.read_text(encoding="utf-8"))
        for rel in d.get("Files", {}).get("CSource", []) or []:
            p = (hls_json_path.parent / rel).resolve()
            if p.suffix in _USER_SOURCE_SUFFIXES and p.exists():
                sources.append(p)
    except Exception as e:  # pragma: no cover - best-effort discovery
        logger.debug("Failed to parse HLS metadata %s: %s", hls_json_path, e)

    if sources:
        return _dedupe_paths(sources)

    # Legacy fallback: component.xml -> ip -> impl -> <solution> ; build dir contains copied kernel .cpp
    try:
        # hls_data.json lives in <build>/<solution>/hls_data.json
        sol_dir = hls_json_path.parent
        build_dir = sol_dir.parent
        for p in sorted(build_dir.glob("*.cpp")):
            sources.append(p.resolve())
    except Exception:  # pragma: no cover - defensive fallback
        pass
    return _dedupe_paths(sources)


def _collect_user_headers_from_sources(sources: Iterable[Path], include_dirs: Iterable[Path]) -> list[Path]:
    """
    Recursively walk local include graphs starting from user C/C++ sources and
    collect reachable user headers. These headers are force-included when
    compiling emu tb.cpp so user-defined types in HLS top signatures are visible.
    """
    roots = _dedupe_paths([p.parent for p in sources] + list(include_dirs))
    header_list: list[Path] = []
    seen_headers: set[Path] = set()
    visited_files: set[Path] = set()

    def _resolve_local_include(inc_name: str, cur_dir: Path) -> Path | None:
        candidates = [cur_dir] + roots
        for base in candidates:
            p = (base / inc_name).resolve()
            if p.exists() and p.is_file():
                return p
        return None

    def _walk_file(path: Path) -> None:
        rp = path.resolve()
        if rp in visited_files or not rp.exists() or not rp.is_file():
            return
        visited_files.add(rp)

        try:
            lines = rp.read_text(
                encoding="utf-8", errors="ignore").splitlines()
        except OSError:
            return

        for line in lines:
            m = _LOCAL_INCLUDE_RE.match(line)
            if not m:
                continue
            _delim, inc_name = m.groups()
            inc = _resolve_local_include(inc_name.strip(), rp.parent)
            if inc is None:
                continue
            if inc.suffix in _USER_HEADER_SUFFIXES:
                if inc not in seen_headers:
                    seen_headers.add(inc)
                    header_list.append(inc)
                _walk_file(inc)
            elif inc.suffix in _USER_SOURCE_SUFFIXES:
                _walk_file(inc)

    for src in _dedupe_paths(sources):
        _walk_file(src)

    return header_list


def _collect_emu_compile_inputs(config: LinkerConfiguration) -> tuple[list[Path], list[Path], list[Path]]:
    """
    Returns:
      (user_cpp_sources, user_include_dirs, force_include_headers)
    """
    cpp_files: list[Path] = []
    include_dirs: list[Path] = []
    force_headers: list[Path] = []

    for kxml in config.kernel_component_paths:
        kpath = Path(kxml).resolve()
        if not kpath.exists():
            raise FileNotFoundError(f"Kernel component.xml not found: {kpath}")

        hls_json = infer_hls_json_from_component_xml(kpath)
        srcs = _resolve_hls_c_sources(hls_json)
        cpp_files.extend(srcs)

        # component.xml -> ip -> impl -> <solution>; cfg is copied into build_dir by BuildHLS.cmake
        sol_dir = kpath.parents[2]
        build_dir = sol_dir.parent
        cfgs = sorted(build_dir.glob("*.cfg"))
        for cfg in cfgs:
            include_dirs.extend(_extract_include_dirs_from_cfg(cfg))

        include_dirs.extend([p.parent for p in srcs])
        force_headers.extend(
            _collect_user_headers_from_sources(srcs, include_dirs))

    return (
        _dedupe_paths(cpp_files) or _collect_kernel_cpp(
            config.kernel_component_paths),
        _dedupe_paths(include_dirs),
        _dedupe_paths(force_headers),
    )


def build_emu_project(config: LinkerConfiguration) -> None:
    tb_path = config.build_dir / "tb.cpp"
    if not tb_path.exists():
        raise FileNotFoundError(f"tb.cpp not found: {tb_path}")

    kernel_cpps, user_include_dirs, force_headers = _collect_emu_compile_inputs(
        config)
    cpp_files = [tb_path] + kernel_cpps
    if not cpp_files:
        raise FileNotFoundError(
            "No C++ sources found to build emulation executable.")

    vitis_include = _find_vitis_include()
    vpp_emu_path = config.build_dir / "vpp_emu"

    include_flags = []
    for inc in user_include_dirs:
        include_flags += ["-I", str(inc)]

    force_include_flags = []
    for hdr in force_headers:
        force_include_flags += ["-include", str(hdr)]

    cmd = (
        ["g++", "-O3"]
        + include_flags
        + force_include_flags
        + [str(p) for p in cpp_files]
        + ["-o", str(vpp_emu_path), "-I", str(vitis_include),
           "-lzmq", "-I", "/usr/include/jsoncpp/", "-ljsoncpp"]
    )
    if force_headers:
        logger.info("EMU compile force-including %d user header(s)",
                    len(force_headers))
    if user_include_dirs:
        logger.info("EMU compile adding %d user include dir(s)",
                    len(user_include_dirs))
    logger.info("Building emulation executable: %s", " ".join(cmd))
    subprocess.run(cmd, cwd=str(config.build_dir), check=True)
    logger.info("Emulation build outputs in %s", config.build_dir)


def package_emu_artifacts(config: LinkerConfiguration) -> Path:
    system_map = config.build_dir / "system_map.xml"
    vpp_emu = config.build_dir / "vpp_emu"
    emu_manifest = config.build_dir / "emu_manifest.json"

    if not system_map.exists():
        raise FileNotFoundError(f"system_map.xml not found: {system_map}")
    if not vpp_emu.exists():
        raise FileNotFoundError(f"vpp_emu not found: {vpp_emu}")

    with tarfile.open(config.out_path, mode="w") as tf:
        tf.add(system_map, arcname="system_map.xml")
        tf.add(vpp_emu, arcname="vpp_emu")
        if emu_manifest.exists():
            tf.add(emu_manifest, arcname="emu_manifest.json")

    logger.info("Emulation vbin in %s", config.out_path)
    return config.out_path
