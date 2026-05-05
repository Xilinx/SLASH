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

if [[ $# -ne 1 ]]; then
    echo "Must provide one argument: DESTDIR" 1>&2
    exit 1
fi

# Install smi, vrt, vrtd, libvrt*, libslash
DESTDIR="$1" cmake --build pbuild/smi --target install

# Install CMake toolchain modules (SlashTools)
DESTDIR="$1" cmake --build pbuild/cmake-tools --target install

python3 -m pip install --no-deps --root $1 linker/dist/slashkit-*.whl
if [ -f $1/usr/local/bin/slashkit ]; then
    mv $1/usr/local/bin/slashkit $1/usr/bin/
    mv $1/usr/local/lib/python3* $1/usr/lib/
fi
