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

set -euxo pipefail

# SLASH root
cd "$(dirname "$0")/.."

make -C linker/slashkit/resources/base/common/iprepo

pushd linker
INSTALL_ARGS=(install --out-dir slashkit/resources)
if [[ -n "${SLASH_ROOT_DESIGN_STAGE:-}" ]]; then
    INSTALL_ARGS+=(--stage "${SLASH_ROOT_DESIGN_STAGE}")
fi

# --vivado defaults to whatever `which vivado` finds on this machine, which is wrong when
# SLASH_TOOL_LAUNCHER is sending the tool invocations to another machine: the submit host
# need not have Vivado installed at all, and if it does, its path may not be the path the
# execution host uses. Passing a bare binary name instead of an absolute path tells
# slashkit to leave it unresolved and let PATH on the execution host decide.
# SLASH_VIVADO_BIN overrides the name, for installs where it is not simply "vivado".
if [[ -n "${SLASH_TOOL_LAUNCHER:-}" || -n "${SLASH_VIVADO_BIN:-}" ]]; then
    INSTALL_ARGS+=(--vivado "${SLASH_VIVADO_BIN:-vivado}")
fi

# 8 is slashkit's own default; naming it here makes it adjustable for machines sized
# differently from the developer workstation the default was chosen for.
INSTALL_ARGS+=(--jobs "${SLASH_ROOT_DESIGN_JOBS:-8}")
python3 -m slashkit "${INSTALL_ARGS[@]}" --shell-type service --build-dir install.prj
python3 -m slashkit "${INSTALL_ARGS[@]}" --shell-type compute --build-dir install.prj.compute
popd

# Vivado IP/synth logs capture the full environment (including RPM_BUILD_ROOT
# during rpmbuild), which trips RPM's check-buildroot. They have no runtime
# value, so drop them before they get packaged into the wheel.
find linker/slashkit/resources -type d -name logs -prune -exec rm -rf {} +
find linker/slashkit/resources \
    -type f \( -name '*.log' -o -name '*.jou' -o -name 'vivado*.str' \) -delete
