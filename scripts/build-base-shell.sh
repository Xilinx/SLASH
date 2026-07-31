#!/bin/bash

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

# Build the FPGA base shell (the "static shell") via a full Vivado synth/impl run.
#
# This is the single, standalone entry point for the base-shell build. It is the slowest
# part of packaging (~5h) and, unlike the rest of the build, only needs Vivado/Vitis -- not
# the SLASHKit software dependencies. That makes it the natural unit to dispatch to a build
# farm while the rest of packaging runs on a local machine.
#
# Two ways for expert users to redirect just this step to a scheduler (e.g. LSF):
#
#   1. Set SLASH_BASE_SHELL_LAUNCHER to a synchronous job-launcher prefix, e.g.
#          export SLASH_BASE_SHELL_LAUNCHER="bsub -K -q fpgasynthesis -n 16 -M 120G"
#      The -K (synchronous) flag is REQUIRED so this script blocks until the shell is built
#      and linker/slashkit/resources/static_shell/ exists before packaging continues.
#      Leave it unset to build on the local machine.
#
#   2. Build the shell separately on the farm, then run packaging with
#          SLASH_PKG_SKIP_ROOT_DESIGN_BUILD=1
#      to reuse the existing static_shell/ artifacts (see below).

set -euo pipefail

# SLASH root
cd "$(dirname "$0")/.."

# If the caller has already produced (or wishes to reuse) the base shell, skip the build
# entirely. This keeps the call site in the packaging scripts unconditional -- this script
# is the one place that decides whether the Vivado run happens.
if [[ -n "${SLASH_PKG_SKIP_ROOT_DESIGN_BUILD:-}" ]]; then
    echo "SLASH_PKG_SKIP_ROOT_DESIGN_BUILD is set; skipping base-shell (Vivado) build." >&2
    echo "Reusing existing linker/slashkit/resources/static_shell/ if present." >&2
    exit 0
fi

# Check build prerequisites (only relevant for the Vivado base-shell build).
_prereq_ok=1

if ! command -v v++ >/dev/null 2>&1; then
    echo "ERROR: v++ not found in PATH. Source Vitis 2025.1 before building:" >&2
    echo "  source <path-to-vitis>/settings64.sh" >&2
    echo "See docs/tutorials/admin/platform-setup.rst for details." >&2
    _prereq_ok=0
fi

if ! compgen -G 'linker/slashkit/resources/base/iprepo/smbus*/' >/dev/null 2>&1; then
    echo "ERROR: SMBus IP (xilinx.com:ip:smbus:1.1) not found in linker/slashkit/resources/base/iprepo/." >&2
    echo "Download it from https://www.xilinx.com/member/v80.html and place the IP" >&2
    echo "directory into linker/slashkit/resources/base/iprepo/ before building." >&2
    echo "See docs/tutorials/admin/platform-setup.rst for details." >&2
    _prereq_ok=0
fi

if [[ "${_prereq_ok}" -eq 0 ]]; then
    exit 1
fi

set -x

bash scripts/root-design-clean.sh

# Optional job-launcher prefix; empty by default (build locally). See the header comment.
SLASH_BASE_SHELL_LAUNCHER="${SLASH_BASE_SHELL_LAUNCHER:-}"
${SLASH_BASE_SHELL_LAUNCHER} bash scripts/root-design-build.sh
