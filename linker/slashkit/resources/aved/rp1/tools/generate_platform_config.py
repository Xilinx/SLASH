#!/usr/bin/env python3
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
"""Generate the RP1 platform contract from an R5_1 BSP xparameters.h."""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path


DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+(.+?)\s*$")
IDENT_RE = re.compile(r"^[A-Za-z_]\w*$")
INTEGER_RE = re.compile(r"^(0[xX][0-9A-Fa-f]+|\d+)(?:[uUlL]+)?$")


class MetadataError(RuntimeError):
    """BSP metadata is absent, ambiguous, or internally inconsistent."""


def parse_defines(path: Path) -> dict[str, str]:
    defines: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.split("//", 1)[0]
        line = line.split("/*", 1)[0]
        match = DEFINE_RE.match(line)
        if match:
            defines[match.group(1)] = match.group(2).strip()
    return defines


def resolve(defines: dict[str, str], name: str, seen: set[str] | None = None) -> int:
    if name not in defines:
        raise MetadataError(f"missing BSP macro {name}")
    seen = set() if seen is None else seen
    if name in seen:
        raise MetadataError(f"cyclic BSP macro alias at {name}")
    seen.add(name)

    value = defines[name].strip()
    while value.startswith("(") and value.endswith(")"):
        value = value[1:-1].strip()
    integer = INTEGER_RE.fullmatch(value)
    if integer:
        return int(integer.group(1), 0)
    if IDENT_RE.fullmatch(value):
        return resolve(defines, value, seen)
    raise MetadataError(f"{name} has unsupported value {defines[name]!r}")


# Vitis may mirror one generated header through several export directories.
# Accept multiple candidates only when byte-identical; different contents mean
# processor ownership is ambiguous and choosing the shortest path is unsafe.
def find_xparameters(root: Path) -> Path:
    candidates: list[Path] = []
    for path in root.rglob("xparameters.h"):
        defines = parse_defines(path)
        if "XPAR_XIPIPSU_NUM_INSTANCES" in defines and any(
            "CPU_CLK_FREQ_HZ" in name and "CORTEXR5" in name
            for name in defines
        ):
            candidates.append(path)
    if len(candidates) > 1:
        contents = {
            path.read_bytes()
            for path in candidates
        }
        if len(contents) == 1:
            return min(candidates, key=lambda path: (len(path.parts), str(path)))
    if len(candidates) != 1:
        rendered = ", ".join(str(path) for path in candidates) or "none"
        raise MetadataError(
            "expected exactly one R5 BSP xparameters.h under "
            f"{root}, found {len(candidates)}: {rendered}"
        )
    return candidates[0]


# Source selection first excludes unbuffered agents, then applies an optional
# instance override. Exactly one buffered R5-owned source must remain because
# its index selects the message-RAM row used by every PDI transaction.
def source_instance(
    defines: dict[str, str], requested: int | None
) -> tuple[int, int, int, int]:
    count = resolve(defines, "XPAR_XIPIPSU_NUM_INSTANCES")
    instances: list[tuple[int, int, int, int]] = []
    for index in range(count):
        prefix = f"XPAR_XIPIPSU_{index}"
        try:
            base = resolve(defines, f"{prefix}_BASE_ADDRESS")
        except MetadataError:
            base = resolve(defines, f"{prefix}_BASEADDR")
        mask = resolve(defines, f"{prefix}_BIT_MASK")
        buffer_index = resolve(defines, f"{prefix}_BUFFER_INDEX")
        if buffer_index <= 7:
            instances.append((index, base, mask, buffer_index))

    if requested is not None:
        instances = [entry for entry in instances if entry[0] == requested]
    if len(instances) != 1:
        names = ", ".join(str(entry[0]) for entry in instances) or "none"
        hint = " (use --source-instance only when the XSA intentionally maps several)"
        raise MetadataError(
            "R5_1 BSP must expose exactly one buffered source IPI; "
            f"eligible instances: {names}{hint}"
        )
    return instances[0]


# Resolve the PMC channel in two independent views: its target mask names the
# trigger bit, while the matching buffered index names its message-RAM column.
# Requiring both to be unique catches incomplete or mixed BSP metadata.
def pmc_target(defines: dict[str, str]) -> tuple[int, int]:
    target_names = [
        name
        for name in defines
        if re.fullmatch(r"XPAR_XIPIPS_TARGET_.*PMC_0_CH0_MASK", name)
    ]
    target_masks = {resolve(defines, name) for name in target_names}
    if len(target_masks) != 1:
        rendered = ", ".join(target_names) or "none"
        raise MetadataError(
            "could not identify one buffered PMC channel-0 target mask; "
            f"candidates: {rendered}"
        )
    target_mask = target_masks.pop()

    target_indices: set[int] = set()
    for name in defines:
        if not name.endswith("_BUFFER_INDEX") or "PMC" not in name:
            continue
        if "NOBUF" in name:
            continue
        peer = name[: -len("_BUFFER_INDEX")] + "_BIT_MASK"
        if peer not in defines:
            continue
        index = resolve(defines, name)
        if index <= 7 and resolve(defines, peer) == target_mask:
            target_indices.add(index)
    if len(target_indices) != 1:
        raise MetadataError(
            "could not identify one buffered PMC target buffer index "
            f"for mask 0x{target_mask:08X}"
        )
    return target_mask, target_indices.pop()


def r5_frequency(defines: dict[str, str], override: int | None) -> int:
    if override is not None:
        return override
    values = {
        resolve(defines, name)
        for name in defines
        if "CORTEXR5" in name and name.endswith("_CPU_CLK_FREQ_HZ")
    }
    if len(values) != 1:
        raise MetadataError(
            "could not derive one Cortex-R5 CPU frequency; "
            "pass --r5-frequency only for BSPs lacking the canonical macro"
        )
    return values.pop()


def message_ram_base(source_base: int, override: int | None) -> int:
    if override is not None:
        return override
    family = source_base & 0xFF000000
    if family == 0xFF000000:
        return 0xFF3F0000
    if family == 0xEB000000:
        return 0xEB3F0000
    raise MetadataError(
        "unrecognized IPI register family; pass --message-ram-base from "
        "the BSP's xipipsu_hw.h"
    )


# This is a compatibility fingerprint, not a security digest. It binds firmware
# publication to the processor, clock, IPI ownership, and derived message-RAM
# addresses so a host can detect a mixed platform contract.
def fnv1a32(values: list[int], processor: str) -> int:
    digest = 0x811C9DC5
    payload = b"RP1v4\0" + processor.encode("ascii") + b"\0"
    payload += b"".join(struct.pack("<I", value) for value in values)
    for byte in payload:
        digest ^= byte
        digest = (digest * 0x01000193) & 0xFFFFFFFF
    return digest or 1


def render(template: Path, output: Path, values: dict[str, int]) -> None:
    text = template.read_text(encoding="utf-8")
    for name, value in values.items():
        rendered = str(value) if name == "RP1_R5_FREQ_HZ" else f"0x{value:08X}"
        text = text.replace(f"@{name}@", rendered)
    leftovers = sorted(set(re.findall(r"@[A-Z0-9_]+@", text)))
    if leftovers:
        raise MetadataError(
            f"unexpanded template values: {', '.join(leftovers)}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    metadata = parser.add_mutually_exclusive_group(required=True)
    metadata.add_argument("--xparameters", type=Path)
    metadata.add_argument("--bsp-metadata", type=Path)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--processor", default="psv_cortexr5_1")
    parser.add_argument("--source-instance", type=int)
    parser.add_argument("--r5-frequency", type=lambda value: int(value, 0))
    parser.add_argument("--message-ram-base", type=lambda value: int(value, 0))
    args = parser.parse_args()

    xparameters = (
        args.xparameters
        if args.xparameters is not None
        else find_xparameters(args.bsp_metadata)
    )
    defines = parse_defines(xparameters)
    _, source_base, source_mask, source_index = source_instance(
        defines, args.source_instance
    )
    target_mask, target_index = pmc_target(defines)
    frequency = r5_frequency(defines, args.r5_frequency)
    message_base = message_ram_base(source_base, args.message_ram_base)

    if source_mask == 0 or target_mask == 0 or frequency == 0:
        raise MetadataError("IPI masks and R5 frequency must be non-zero")

    # Each source owns a 512-byte row and each buffered target a 64-byte cell.
    # PLM reserves the lower 32 bytes for the request and the upper 32 for its
    # response, matching the firmware's ordered request/acknowledgement flow.
    request = message_base + source_index * 512 + target_index * 64
    response = request + 32
    values = [
        frequency,
        source_base,
        source_mask,
        source_index,
        target_mask,
        target_index,
        request,
        response,
    ]
    config = {
        "RP1_PLATFORM_ID": fnv1a32(values, args.processor),
        "RP1_R5_FREQ_HZ": frequency,
        "RP1_PDI_IPI_SOURCE_BASE": source_base,
        "RP1_PDI_IPI_SOURCE_MASK": source_mask,
        "RP1_PDI_IPI_SOURCE_BUF_INDEX": source_index,
        "RP1_PDI_IPI_TARGET_MASK": target_mask,
        "RP1_PDI_IPI_TARGET_BUF_INDEX": target_index,
        "RP1_PDI_IPI_REQUEST_BASE": request,
        "RP1_PDI_IPI_RESPONSE_BASE": response,
        "RP1_PDI_IPI_TRIGGER_REG": source_base,
        "RP1_PDI_IPI_OBSERVATION_REG": source_base + 4,
    }
    render(args.template, args.output, config)
    print(
        f"generated {args.output} from {xparameters}: "
        f"source=0x{source_base:08X}/mask=0x{source_mask:08X}, "
        f"PMC mask=0x{target_mask:08X}, platform=0x{config['RP1_PLATFORM_ID']:08X}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except MetadataError as error:
        raise SystemExit(f"error: {error}")
