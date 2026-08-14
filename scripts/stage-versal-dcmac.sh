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

# Copy the Versal-DCMAC submodule sources into the slashkit package tree.
#
# submodules/ sits outside linker/, so pyproject.toml's "resources/**/*" package
# data never picks it up and an installed wheel/deb/rpm cannot link a service
# shell design. Run this before building the wheel; the staged copy is what
# stage_versal_dcmac() (Python) and slash_resolve_versal_dcmac_root (Tcl) find
# first. The destination is gitignored.

set -euo pipefail

# SLASH root
cd "$(dirname "$0")/.."

src=submodules/Versal-DCMAC
dst=linker/slashkit/resources/dcmac/versal

if [[ ! -f "$src/tcl/dcmac.tcl" ]]; then
    echo "error: $src is not checked out. Run:" >&2
    echo "    git submodule update --init submodules/Versal-DCMAC" >&2
    exit 1
fi

rm -rf "$dst"
mkdir -p "$dst"
cp -a "$src/hdl" "$src/tcl" "$src/LICENSE" "$dst/"

echo "Staged $src -> $dst"
