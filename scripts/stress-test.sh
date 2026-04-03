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

# Examples: directory:vbin_prefix:executable
EXAMPLES=(
    "00_axilite:axilite:00_axilite"
    "01_aximm:aximm:01_aximm"
    "02_chain:chain:02_chain"
    "04_freq:freq:04_freq"
)

NUM_EXAMPLES=${#EXAMPLES[@]}

# Action names for reporting
ACTION_NAMES=("run_example" "reset" "program" "validate")
NUM_ACTIONS=${#ACTION_NAMES[@]}

# Counters
declare -A ACTION_PASS
declare -A ACTION_FAIL
for name in "${ACTION_NAMES[@]}"; do
    ACTION_PASS[$name]=0
    ACTION_FAIL[$name]=0
done
ACCURACY_FAIL=0
ITERATIONS_DONE=0
START_TIME=$(date +%s)

# =========================================================================
#  Usage
# =========================================================================
usage() {
    echo "Usage: $0 <BDF> [--iterations N]"
    echo ""
    echo "  Stress test a V80 board by randomly running examples, resets,"
    echo "  programming vbins, and memory validation."
    echo ""
    echo "  Arguments:"
    echo "    BDF              Device BDF address (e.g. 0000:e2:00.0)"
    echo "    --iterations N   Number of iterations (default: 100)"
    echo ""
    echo "  Pre-built vbins and executables must exist in examples/*/build/."
    exit 1
}

# =========================================================================
#  Summary
# =========================================================================
print_summary() {
    local end_time
    end_time=$(date +%s)
    local elapsed=$((end_time - START_TIME))

    echo ""
    echo "========================================================================"
    echo "  Stress Test Summary"
    echo "========================================================================"
    echo "  Iterations completed: $ITERATIONS_DONE"
    echo "  Elapsed time:         ${elapsed}s"
    echo "  Accuracy failures:    $ACCURACY_FAIL"
    echo "------------------------------------------------------------------------"
    printf "  %-15s  %6s  %6s\n" "Action" "Pass" "Fail"
    echo "------------------------------------------------------------------------"
    for name in "${ACTION_NAMES[@]}"; do
        printf "  %-15s  %6d  %6d\n" "$name" "${ACTION_PASS[$name]}" "${ACTION_FAIL[$name]}"
    done
    echo "========================================================================"
}

# =========================================================================
#  Helpers
# =========================================================================
timestamp() {
    date "+%Y-%m-%d %H:%M:%S"
}

log() {
    echo "[$(timestamp)] $*"
}

# Derive bdf_base from BDF by stripping the PCI function suffix (e.g. 0000:1b:00.0 -> 0000:1b:00)
bdf_to_base() {
    echo "${1%.*}"
}

check_device() {
    log "v80-smi list -j"
    local json
    json=$(v80-smi list -j)
    echo "$json"

    local bdf_base
    bdf_base=$(bdf_to_base "$BDF")

    local board_status
    board_status=$(echo "$json" | jq -r --arg b "$bdf_base" '.boards[] | select(.bdf_base == $b) | .status')

    if [[ -z "$board_status" ]]; then
        log "ERROR: Board $bdf_base not found in v80-smi list output"
        return 1
    fi

    if [[ "$board_status" != "OK" ]]; then
        log "ERROR: Board $bdf_base status is '$board_status' (expected 'OK')"
        return 1
    fi

    log "Board $bdf_base status: OK"
}

random_delay() {
    local delay=$((RANDOM % 11))
    if [[ $delay -gt 0 ]]; then
        log "Sleeping ${delay}s..."
        sleep "$delay"
    fi
}

pick_random_example() {
    local idx=$((RANDOM % NUM_EXAMPLES))
    echo "${EXAMPLES[$idx]}"
}

# =========================================================================
#  Parse arguments
# =========================================================================
if [[ $# -lt 1 ]]; then
    usage
fi

BDF="$1"
shift

ITERATIONS=100
NO_RESET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --iterations)
            if [[ $# -lt 2 ]]; then
                echo "ERROR: --iterations requires a value"
                usage
            fi
            ITERATIONS="$2"
            shift 2
            ;;
        --no-reset)
            NO_RESET=1
            shift
            ;;
        *)
            echo "ERROR: Unknown argument '$1'"
            usage
            ;;
    esac
done

if [[ "$NO_RESET" -eq 1 ]]; then
    ACTION_NAMES=("run_example" "program" "validate")
    NUM_ACTIONS=${#ACTION_NAMES[@]}
fi

# =========================================================================
#  Pre-flight checks
# =========================================================================
log "Starting stress test: BDF=$BDF, iterations=$ITERATIONS"
echo ""

log "Checking device visibility..."
if ! check_device; then
    echo "ERROR: Device check failed. Is the device present and vrtd running?"
    exit 1
fi
echo ""

MISSING=0
for entry in "${EXAMPLES[@]}"; do
    IFS=':' read -r dir vbin_prefix executable <<< "$entry"

    vbin_file="$EXAMPLES_DIR/$dir/build/${vbin_prefix}_hw.vbin"
    exec_file="$EXAMPLES_DIR/$dir/build/$executable"

    if [[ ! -f "$vbin_file" ]]; then
        echo "ERROR: Missing vbin: $vbin_file"
        MISSING=1
    fi
    if [[ ! -f "$exec_file" ]]; then
        echo "ERROR: Missing executable: $exec_file"
        MISSING=1
    fi
done

if [[ $MISSING -ne 0 ]]; then
    echo ""
    echo "Pre-built vbins and executables are required. Build them first:"
    echo "  ./scripts/test-examples.sh hw $BDF"
    exit 1
fi

log "All pre-flight checks passed."
echo ""

# =========================================================================
#  Signal handler
# =========================================================================
trap 'log "Interrupted."; print_summary; exit 130' INT TERM

# =========================================================================
#  Main loop
# =========================================================================
for ((i = 1; i <= ITERATIONS; i++)); do
    action_idx=$((RANDOM % NUM_ACTIONS))
    action="${ACTION_NAMES[$action_idx]}"

    echo ""
    echo "========================================================================"
    log "Iteration $i/$ITERATIONS — action: $action"
    echo "========================================================================"

    case "$action" in
        run_example)
            IFS=':' read -r dir vbin_prefix executable <<< "$(pick_random_example)"
            vbin_file="$EXAMPLES_DIR/$dir/build/${vbin_prefix}_hw.vbin"
            exec_file="$EXAMPLES_DIR/$dir/build/$executable"

            log "Running: $exec_file $BDF $vbin_file"
            rc=0
            "$exec_file" "$BDF" "$vbin_file" || rc=$?
            if [[ $rc -eq 2 ]]; then
                log "ACCURACY FAILURE: $action ($dir) at iteration $i (continuing)"
                ACCURACY_FAIL=$((ACCURACY_FAIL + 1))
            elif [[ $rc -ne 0 ]]; then
                log "FAILED: $action ($dir) at iteration $i"
                ACTION_FAIL[$action]=$((ACTION_FAIL[$action] + 1))
                ITERATIONS_DONE=$i
                print_summary
                exit 1
            fi
            ACTION_PASS[$action]=$((ACTION_PASS[$action] + 1))
            if ! check_device; then
                log "FAILED: device check after $action ($dir) at iteration $i"
                ITERATIONS_DONE=$i
                print_summary
                exit 1
            fi
            ;;

        reset)
            log "Running: v80-smi reset -d $BDF"
            if ! v80-smi reset -d "$BDF"; then
                log "FAILED: $action at iteration $i"
                ACTION_FAIL[$action]=$((ACTION_FAIL[$action] + 1))
                ITERATIONS_DONE=$i
                print_summary
                exit 1
            fi
            ACTION_PASS[$action]=$((ACTION_PASS[$action] + 1))
            if ! check_device; then
                log "FAILED: device check after $action at iteration $i"
                ITERATIONS_DONE=$i
                print_summary
                exit 1
            fi
            ;;

        program)
            IFS=':' read -r dir vbin_prefix executable <<< "$(pick_random_example)"
            vbin_file="$EXAMPLES_DIR/$dir/build/${vbin_prefix}_hw.vbin"

            log "Running: v80-smi program -d $BDF $vbin_file"
            if ! v80-smi program -d "$BDF" "$vbin_file"; then
                log "FAILED: $action ($dir) at iteration $i"
                ACTION_FAIL[$action]=$((ACTION_FAIL[$action] + 1))
                ITERATIONS_DONE=$i
                print_summary
                exit 1
            fi
            ACTION_PASS[$action]=$((ACTION_PASS[$action] + 1))
            if ! check_device; then
                log "FAILED: device check after $action ($dir) at iteration $i"
                ITERATIONS_DONE=$i
                print_summary
                exit 1
            fi
            # TODO: Add "v80-smi query -d $BDF -j" here once the query bug is fixed
            ;;

        validate)
            threads=$((RANDOM % 64 + 1))
            NO_RESET_FLAG=""
            if [[ "$NO_RESET" -eq 1 ]]; then NO_RESET_FLAG="--no-reset"; fi
            log "Running: v80-smi validate -d $BDF -j $threads $NO_RESET_FLAG"
            if ! v80-smi validate -d "$BDF" -j "$threads" $NO_RESET_FLAG; then
                log "FAILED: $action (threads=$threads) at iteration $i"
                ACTION_FAIL[$action]=$((ACTION_FAIL[$action] + 1))
                ITERATIONS_DONE=$i
                print_summary
                exit 1
            fi
            ACTION_PASS[$action]=$((ACTION_PASS[$action] + 1))
            if ! check_device; then
                log "FAILED: device check after $action (threads=$threads) at iteration $i"
                ITERATIONS_DONE=$i
                print_summary
                exit 1
            fi
            ;;
    esac

    ITERATIONS_DONE=$i
    random_delay
done

# =========================================================================
#  Final summary
# =========================================================================
log "Stress test completed successfully."
print_summary
