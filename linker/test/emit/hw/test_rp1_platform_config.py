# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

from contextlib import ExitStack
from importlib import resources
import subprocess
import sys


BSP_TEXT = """
#define XPAR_XIPIPSU_NUM_INSTANCES 1U
#define XPAR_R5_1_IPI_BASE 0xFF380000U
#define XPAR_R5_1_IPI_MASK 0x00000080U
#define XPAR_R5_1_IPI_INDEX 7U
#define XPAR_XIPIPSU_0_BASE_ADDRESS XPAR_R5_1_IPI_BASE
#define XPAR_XIPIPSU_0_BIT_MASK XPAR_R5_1_IPI_MASK
#define XPAR_XIPIPSU_0_BUFFER_INDEX XPAR_R5_1_IPI_INDEX
#define XPAR_PSV_IPI_PMC_BIT_MASK 0x00000002U
#define XPAR_PSV_IPI_PMC_BUFFER_INDEX 1U
#define XPAR_XIPIPS_TARGET_PSV_PMC_0_CH0_MASK XPAR_PSV_IPI_PMC_BIT_MASK
#define XPAR_CPU_CORTEXR5_0_CPU_CLK_FREQ_HZ 600000000U
"""


def test_rp1_platform_config_is_derived_from_bsp(tmp_path):
    rp1_root = resources.files("slashkit.resources.aved").joinpath("rp1")
    xparameters = tmp_path / "xparameters.h"
    output = tmp_path / "rp1_platform_config.h"
    xparameters.write_text(BSP_TEXT, encoding="utf-8")

    with ExitStack() as stack:
        generator = stack.enter_context(resources.as_file(
            rp1_root.joinpath("tools", "generate_platform_config.py")))
        template = stack.enter_context(resources.as_file(
            rp1_root.joinpath("config", "rp1_platform_config.h.in")))
        subprocess.run(
            [
                sys.executable,
                str(generator),
                "--xparameters",
                str(xparameters),
                "--template",
                str(template),
                "--output",
                str(output),
            ],
            check=True,
        )

    generated = output.read_text(encoding="utf-8")
    assert "RP1_PLATFORM_CONFIG_GENERATED 1u" in generated
    assert "RP1_R5_FREQ_HZ                600000000u" in generated
    assert "RP1_PDI_IPI_SOURCE_BASE       0xFF380000u" in generated
    assert "RP1_PDI_IPI_SOURCE_MASK       0x00000080u" in generated
    assert "RP1_PDI_IPI_SOURCE_BUF_INDEX  0x00000007u" in generated
    assert "RP1_PDI_IPI_TARGET_MASK       0x00000002u" in generated
    assert "RP1_PDI_IPI_REQUEST_BASE      0xFF3F0E40u" in generated
    assert "RP1_PDI_IPI_RESPONSE_BASE     0xFF3F0E60u" in generated
    assert "RP1_PDI_IPI_TRIGGER_REG       0xFF380000u" in generated
    assert "RP1_PDI_IPI_OBSERVATION_REG   0xFF380004u" in generated


def test_rp1_platform_config_accepts_identical_exported_bsp_copies(tmp_path):
    rp1_root = resources.files("slashkit.resources.aved").joinpath("rp1")
    metadata = tmp_path / "metadata"
    first = metadata / "workspace" / "include" / "xparameters.h"
    second = metadata / "export" / "include" / "xparameters.h"
    first.parent.mkdir(parents=True)
    second.parent.mkdir(parents=True)
    first.write_text(BSP_TEXT, encoding="utf-8")
    second.write_text(BSP_TEXT, encoding="utf-8")
    output = tmp_path / "rp1_platform_config.h"

    with ExitStack() as stack:
        generator = stack.enter_context(resources.as_file(
            rp1_root.joinpath("tools", "generate_platform_config.py")))
        template = stack.enter_context(resources.as_file(
            rp1_root.joinpath("config", "rp1_platform_config.h.in")))
        subprocess.run(
            [
                sys.executable,
                str(generator),
                "--bsp-metadata",
                str(metadata),
                "--template",
                str(template),
                "--output",
                str(output),
            ],
            check=True,
        )

    assert "RP1_PDI_IPI_SOURCE_BASE       0xFF380000u" in output.read_text(
        encoding="utf-8")
