#!/usr/bin/env python3
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
"""Patch XilPLMI 2025.1 to demultiplex sparse SDT IPI target masks."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


# Relative suffix of the Versal implementation copied into an Empyro BSP.
PLATFORM_SOURCE_SUFFIX = Path(
    "xilplmi/src/versal/server/xplmi_plat.c").parts

# Marker emitted only by the patched implementation.
PATCH_MARKER = "IpiInstance->Config.TargetCount"

# Stable declaration immediately before XilPLMI's faulty dispatch loop.
DECLARATION = re.compile(
    r"^(?P<indent>[ \t]*)u32 IpiIndexMask;$",
    re.MULTILINE,
)

# XilPLMI 2025.1 masks all asserted sources, then incorrectly treats the
# cardinality of a sparse target list as the highest valid IPI bit.
FAULTY_LOOP = re.compile(
    r"(?P<indent>[ \t]*)for \(IpiIndex = 0U; "
    r"IpiIndex < XPLMI_IPI_MASK_COUNT; \+\+IpiIndex\) \{\s*"
    r"IpiIndexMask = \(u32\)1U << IpiIndex;\s*"
    r"if \(\(\(IpiIntrVal & IpiIndexMask\) != 0U\) &&\s*"
    r"\(\(IpiMaskVal & IpiIndexMask\) == 0U\)\) \{\s*"
    r"XPlmi_GicIntrAddTask\(XPlmi_GetIpiIntrId\(IpiIndex\)\);\s*"
    r"\}\s*\}",
    re.MULTILINE,
)


def find_platform_source(bsp_root: Path) -> Path:
    """Return the one Versal XilPLMI platform source beneath @p bsp_root."""
    matches = [
        path
        for path in bsp_root.rglob("xplmi_plat.c")
        if path.parts[-len(PLATFORM_SOURCE_SUFFIX):]
        == PLATFORM_SOURCE_SUFFIX
    ]
    if len(matches) != 1:
        rendered = ", ".join(str(path) for path in matches) or "none"
        raise RuntimeError(
            "expected one Versal XilPLMI platform source, found "
            f"{len(matches)}: {rendered}"
        )
    return matches[0]


def patch_platform_source(path: Path) -> bool:
    """Patch @p path in place; return false when it is already patched."""
    text = path.read_text(encoding="utf-8")
    if PATCH_MARKER in text:
        return False
    declarations = list(DECLARATION.finditer(text))
    if len(declarations) != 1:
        raise RuntimeError(
            f"XilPLMI declaration context changed in {path}")

    match = FAULTY_LOOP.search(text)
    if match is None:
        raise RuntimeError(
            f"XilPLMI sparse IPI dispatch loop not found in {path}")

    indent = match.group("indent")
    replacement = (
        f"{indent}IpiInstance = XPlmi_GetIpiInstance();\n"
        f"{indent}/* SDT target masks are sparse; TargetCount is a "
        "cardinality, not a bit bound. */\n"
        f"{indent}for (IpiIndex = 0U;\n"
        f"{indent}     IpiIndex < IpiInstance->Config.TargetCount;\n"
        f"{indent}     ++IpiIndex) {{\n"
        f"{indent} IpiIndexMask =\n"
        f"{indent}  IpiInstance->Config.TargetList[IpiIndex].Mask;\n"
        f"{indent} if (((IpiIntrVal & IpiIndexMask) != 0U) &&\n"
        f"{indent}     ((IpiMaskVal & IpiIndexMask) == 0U)) {{\n"
        f"{indent}  XPlmi_GicIntrAddTask(XPlmi_GetIpiIntrId(\n"
        f"{indent}   IpiInstance->Config.TargetList[IpiIndex]."
        "BufferIndex));\n"
        f"{indent} }}\n"
        f"{indent}}}"
    )

    text = text[:match.start()] + replacement + text[match.end():]
    text = DECLARATION.sub(
        lambda declaration: (
            declaration.group(0)
            + "\n"
            + declaration.group("indent")
            + "XIpiPsu *IpiInstance;"
        ),
        text,
        count=1,
    )
    path.write_text(text, encoding="utf-8")
    return True


def main() -> int:
    """Patch the generated BSP selected on the command line."""
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--bsp-root",
        type=Path,
        required=True,
        help="Empyro BSP directory containing libsrc/xilplmi",
    )
    args = parser.parse_args()

    source = find_platform_source(args.bsp_root)
    changed = patch_platform_source(source)
    print(f"{'patched' if changed else 'already patched'} {source}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
