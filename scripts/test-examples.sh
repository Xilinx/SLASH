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

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
EXAMPLES_DIR="$REPO_ROOT/examples"

SANITIZE=false

usage() {
    echo "Usage: $0 [--sanitize] <hw|sim|emu> [BDF]"
    echo ""
    echo "  Build and run examples 0, 1, 2, and 4 for the specified platform."
    echo ""
    echo "  Options:"
    echo "    --sanitize   Build with AddressSanitizer and UBSan"
    echo ""
    echo "  Arguments:"
    echo "    hw|sim|emu   Target platform (hardware, simulation, or emulation)"
    echo "    BDF          Device BDF (optional, auto-detected via v80-smi if omitted)"
    echo ""
    exit 1
}

# Parse --sanitize flag
while [[ $# -gt 0 && "$1" == --* ]]; do
    case "$1" in
        --sanitize) SANITIZE=true; shift ;;
        *) echo "ERROR: Unknown option '$1'"; usage ;;
    esac
done

if [[ $# -lt 1 ]]; then
    usage
fi

PLATFORM="$1"

if [[ "$PLATFORM" != "hw" && "$PLATFORM" != "sim" && "$PLATFORM" != "emu" ]]; then
    echo "ERROR: Invalid platform '$PLATFORM'. Must be one of: hw, sim, emu"
    usage
fi

# Sanitizer cmake flags
CMAKE_EXTRA_ARGS=()
if [[ "$SANITIZE" == true ]]; then
    echo "=== AddressSanitizer + UBSan ENABLED ==="
    CMAKE_EXTRA_ARGS+=(
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
        -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
        -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
        -DENABLE_SANITIZERS=ON
    )
fi

# Determine BDF (only required for hw)
if [[ $# -ge 2 ]]; then
    BDF="$2"
else
    echo "=== Auto-detecting BDF via v80-smi ==="
    BDF=$(v80-smi list 2>/dev/null | grep -oP 'Board \K[0-9a-fA-F:.]+' | head -1 || true)
    if [[ -z "$BDF" ]]; then
        if [[ "$PLATFORM" == "hw" ]]; then
            echo "ERROR: Could not auto-detect BDF. Please provide it as the second argument."
            exit 1
        fi
        BDF="0000:00:00.0"
        echo "No device found, using default BDF: $BDF"
    else
        echo "Detected BDF: $BDF"
    fi
fi

# Examples to build and run: directory name, vbin prefix, executable name
EXAMPLES=(
    "00_axilite:axilite:00_axilite"
    "01_aximm:aximm:01_aximm"
    "02_chain:chain:02_chain"
    "04_freq:freq:04_freq"
)

# =========================================================================
#  Stage 1: Configure all examples
# =========================================================================
echo ""
echo "========================================================================"
echo "  Stage 1: Configure"
echo "========================================================================"

for entry in "${EXAMPLES[@]}"; do
    IFS=':' read -r dir vbin_prefix executable <<< "$entry"

    EXAMPLE_DIR="$EXAMPLES_DIR/$dir"
    BUILD_DIR="$EXAMPLE_DIR/build"

    echo "--- Configuring $dir ---"
    cmake -B "$BUILD_DIR" -S "$EXAMPLE_DIR" "${CMAKE_EXTRA_ARGS[@]+"${CMAKE_EXTRA_ARGS[@]}"}"
    echo ""
done

# =========================================================================
#  Stage 2: Build HLS kernels for all examples
# =========================================================================
echo ""
echo "========================================================================"
echo "  Stage 2: Build HLS kernels"
echo "========================================================================"

for entry in "${EXAMPLES[@]}"; do
    IFS=':' read -r dir vbin_prefix executable <<< "$entry"

    BUILD_DIR="$EXAMPLES_DIR/$dir/build"

    echo "--- Building HLS kernels for $dir ---"
    cmake --build "$BUILD_DIR" --target hls
    echo ""
done

# =========================================================================
#  Stage 3: Build VBIN targets for all examples
# =========================================================================
echo ""
echo "========================================================================"
echo "  Stage 3: Build VBIN targets ($PLATFORM)"
echo "========================================================================"

for entry in "${EXAMPLES[@]}"; do
    IFS=':' read -r dir vbin_prefix executable <<< "$entry"

    BUILD_DIR="$EXAMPLES_DIR/$dir/build"
    VBIN_TARGET="${vbin_prefix}_${PLATFORM}"

    echo "--- Building VBIN target: $VBIN_TARGET ---"
    cmake --build "$BUILD_DIR" --target "$VBIN_TARGET"
    echo ""
done

# =========================================================================
#  Stage 4: Build host applications for all examples
# =========================================================================
echo ""
echo "========================================================================"
echo "  Stage 4: Build host applications"
echo "========================================================================"

for entry in "${EXAMPLES[@]}"; do
    IFS=':' read -r dir vbin_prefix executable <<< "$entry"

    BUILD_DIR="$EXAMPLES_DIR/$dir/build"

    echo "--- Building application: $executable ---"
    cmake --build "$BUILD_DIR" --target "$executable"
    echo ""
done

# =========================================================================
#  Stage 5: Run all examples
# =========================================================================

# ASAN runtime options
if [[ "$SANITIZE" == true ]]; then
    export ASAN_OPTIONS="abort_on_error=1:detect_leaks=0"
    export UBSAN_OPTIONS="print_stacktrace=1"
fi

# For emu/sim, Vivado libraries are required at runtime
if [[ "$PLATFORM" == "emu" || "$PLATFORM" == "sim" ]]; then
    if [[ -z "${XILINX_VIVADO:-}" ]]; then
        echo "ERROR: XILINX_VIVADO is not set."
        echo "Please source the settings64.sh (or settings64.csh) file from your Vivado installation directory."
        exit 1
    fi
    export LD_LIBRARY_PATH="${XILINX_VIVADO}/lib/lnx64.o${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

echo ""
echo "========================================================================"
echo "  Stage 5: Run examples ($PLATFORM, BDF: $BDF)"
echo "========================================================================"

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
RESULTS=()

for entry in "${EXAMPLES[@]}"; do
    IFS=':' read -r dir vbin_prefix executable <<< "$entry"

    BUILD_DIR="$EXAMPLES_DIR/$dir/build"
    VBIN_TARGET="${vbin_prefix}_${PLATFORM}"
    VBIN_FILE="$BUILD_DIR/${VBIN_TARGET}.vbin"

    echo "--- Running: $executable $BDF $VBIN_FILE ---"
    if "$BUILD_DIR/$executable" "$BDF" "$VBIN_FILE"; then
        echo "PASS: $dir"
        RESULTS+=("$dir: PASSED")
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "FAIL: $dir (exit code: $?)"
        RESULTS+=("$dir: FAILED")
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
    echo ""
done

# =========================================================================
#  Summary
# =========================================================================
echo ""
echo "========================================================================"
echo "  Summary  |  Platform: $PLATFORM  |  BDF: $BDF"
echo "========================================================================"
for result in "${RESULTS[@]}"; do
    echo "  $result"
done
echo "------------------------------------------------------------------------"
echo "  Passed: $PASS_COUNT  |  Failed: $FAIL_COUNT  |  Skipped: $SKIP_COUNT"
echo "========================================================================"

if [[ $FAIL_COUNT -gt 0 ]]; then
    exit 1
fi
