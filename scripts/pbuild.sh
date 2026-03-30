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

if [[ -z "${SLASH_PKG_SKIP_ROOT_DESIGN_BUILD:-}" ]]; then
    bash scripts/root-design-clean.sh
fi

# Vendor jinja2 + markupsafe into linker/src/vendor/ (always, even when
# root-design-build is skipped — the linker package needs these at runtime).
find_python() {
    if command -v python3 > /dev/null 2>&1; then
        ver=$(python3 -c 'import sys; print(sys.version_info.minor)')
        if [ "$ver" -ge 10 ] 2>/dev/null; then
            echo python3
            return
        fi
    fi
    for minor in 13 12 11 10; do
        if command -v "python3.${minor}" > /dev/null 2>&1; then
            echo "python3.${minor}"
            return
        fi
    done
    echo "ERROR: pbuild requires Python >= 3.10" >&2
    exit 1
}
PYTHON=$(find_python)
rm -rf linker/src/vendor/jinja2 linker/src/vendor/markupsafe
"$PYTHON" -m pip install jinja2 markupsafe --target=linker/src/vendor/ --no-deps

cmake --build pbuild/smi

if [[ -z "${SLASH_PKG_SKIP_ROOT_DESIGN_BUILD:-}" ]]; then
    bash scripts/root-design-build.sh
fi
