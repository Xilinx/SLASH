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
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
#  NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
#  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
#  DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 BDF [graph-examples-directory]

Runs the graph hardware acceptance sequence without resetting the board or
restarting vrtd. The optional directory defaults to examples/graph.

Path overrides:
  GRAPH_EXAMPLES_ROOT, MULTI_BIN, MULTI_VBIN_A, MULTI_VBIN_B
  EDGE_BIN, EDGE_VBIN, SHARPEN_BIN, SHARPEN_VBIN, V80_SMI

Run controls:
  ARTIFACT_DIR          New output directory (default: repo tmp/ with UTC run id)
  CASE_TIMEOUT          Per-application watchdog duration (default: 120s)
  KILL_GRACE            SIGTERM-to-SIGKILL grace (default: 10s)
  DIAGNOSTIC_TIMEOUT    rp1-dump watchdog duration (default: 10s)
  TIMEOUT_BIN           GNU timeout command/path (default: timeout)
  SHA256_BIN            sha256sum command/path (default: sha256sum)
  VRTD_SOCKET           Optional socket passed to each graph application
  RP1_BAR               RP1 DDR-window BAR (default: 4)
  RP1_CTRL_OFFSET       RP1 control-block BAR offset (default: 0x4000000)
EOF
}

if [[ ${1:-} == "--help" || ${1:-} == "-h" ]]; then
    usage
    exit 0
fi
if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)
BDF=$1
ROOT=${2:-${GRAPH_EXAMPLES_ROOT:-"$REPO_ROOT/examples/graph"}}

multi=${MULTI_BUILD_DIR:-"$ROOT/00_multi_image_pipeline/build"}
edges=${EDGE_BUILD_DIR:-"$ROOT/01_edge_detection/build"}
sharpen=${SHARPEN_BUILD_DIR:-"$ROOT/02_sharpen_loop/build"}

MULTI_BIN=${MULTI_BIN:-"$multi/multi_image_pipeline"}
MULTI_VBIN_A=${MULTI_VBIN_A:-"$multi/multi_image_pipeline_a_hw.vbin"}
MULTI_VBIN_B=${MULTI_VBIN_B:-"$multi/multi_image_pipeline_b_hw.vbin"}
EDGE_BIN=${EDGE_BIN:-"$edges/edge_detection"}
EDGE_VBIN=${EDGE_VBIN:-"$edges/edge_detection_hw.vbin"}
SHARPEN_BIN=${SHARPEN_BIN:-"$sharpen/sharpen_loop"}
SHARPEN_VBIN=${SHARPEN_VBIN:-"$sharpen/sharpen_loop_hw.vbin"}

V80_SMI=${V80_SMI:-v80-smi}
TIMEOUT_BIN=${TIMEOUT_BIN:-timeout}
SHA256_BIN=${SHA256_BIN:-sha256sum}
CASE_TIMEOUT=${CASE_TIMEOUT:-120s}
KILL_GRACE=${KILL_GRACE:-10s}
DIAGNOSTIC_TIMEOUT=${DIAGNOSTIC_TIMEOUT:-10s}
RP1_BAR=${RP1_BAR:-4}
RP1_CTRL_OFFSET=${RP1_CTRL_OFFSET:-0x4000000}
VRTD_SOCKET=${VRTD_SOCKET:-}
RUN_ID=${RUN_ID:-"$(date -u +%Y%m%dT%H%M%SZ)"}
ARTIFACT_DIR=${ARTIFACT_DIR:-"$REPO_ROOT/tmp/graph-hardware-acceptance/$RUN_ID"}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

resolve_command() {
    local requested=$1
    if [[ $requested == */* ]]; then
        [[ -x $requested ]] || die "command is not executable: $requested"
        printf '%s\n' "$requested"
    else
        command -v "$requested" || die "required command not found: $requested"
    fi
}

require_file() {
    [[ -f $1 ]] || die "missing hardware acceptance artifact: $1"
}

require_executable() {
    [[ -f $1 && -x $1 ]] || die "missing executable hardware acceptance artifact: $1"
}

require_executable "$MULTI_BIN"
require_file "$MULTI_VBIN_A"
require_file "$MULTI_VBIN_B"
require_executable "$EDGE_BIN"
require_file "$EDGE_VBIN"
require_executable "$SHARPEN_BIN"
require_file "$SHARPEN_VBIN"

V80_SMI_PATH=$(resolve_command "$V80_SMI")
TIMEOUT_PATH=$(resolve_command "$TIMEOUT_BIN")
SHA256_PATH=$(resolve_command "$SHA256_BIN")
ENV_PATH=$(resolve_command env)

[[ ! -e $ARTIFACT_DIR ]] ||
    die "artifact directory already exists; choose a new ARTIFACT_DIR: $ARTIFACT_DIR"
mkdir -p -- "$ARTIFACT_DIR"

STARTED_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
RESULT=FAIL

write_provenance() {
    {
        printf 'started_at=%s\n' "$STARTED_AT"
        printf 'bdf=%s\n' "$BDF"
        printf 'source_root=%s\n' "$REPO_ROOT"
        printf 'graph_examples_root=%s\n' "$ROOT"
        printf 'acceptance_script=%s\n' "${BASH_SOURCE[0]}"
        printf 'artifact_dir=%s\n' "$ARTIFACT_DIR"
        printf 'reset_policy=no-reset\n'
        printf 'case_timeout=%s\n' "$CASE_TIMEOUT"
        printf 'kill_grace=%s\n' "$KILL_GRACE"
        printf 'rp1_bar=%s\n' "$RP1_BAR"
        printf 'rp1_ctrl_offset=%s\n' "$RP1_CTRL_OFFSET"
        if git -C "$REPO_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
            printf 'source_revision=%s\n' "$(git -C "$REPO_ROOT" rev-parse HEAD)"
            printf 'source_branch=%s\n' "$(git -C "$REPO_ROOT" branch --show-current)"
            if [[ -n $(git -C "$REPO_ROOT" status --porcelain) ]]; then
                printf 'source_dirty=yes\n'
            else
                printf 'source_dirty=no\n'
            fi
            printf '\n[git-status]\n'
            git -C "$REPO_ROOT" status --short
        else
            printf 'source_revision=unavailable\n'
            printf 'source_branch=unavailable\n'
            printf 'source_dirty=unknown\n'
        fi
    } >"$ARTIFACT_DIR/provenance.txt"
}

write_input_hashes() {
    local inputs=(
        "${BASH_SOURCE[0]}"
        "$MULTI_BIN" "$MULTI_VBIN_A" "$MULTI_VBIN_B"
        "$EDGE_BIN" "$EDGE_VBIN"
        "$SHARPEN_BIN" "$SHARPEN_VBIN"
        "$V80_SMI_PATH"
    )
    local source
    for source in \
        "$ROOT/00_multi_image_pipeline/multi_image_pipeline.cpp" \
        "$ROOT/01_edge_detection/01_edge_detection.cpp" \
        "$ROOT/02_sharpen_loop/02_sharpen_loop.cpp" \
        "$REPO_ROOT/vrt/src/graph/device/fpga_device.cpp" \
        "$REPO_ROOT/smi/src/debug/rp1_probe.cpp" \
        "$REPO_ROOT/driver/libslash/include/slash/uapi/rp1_protocol.h"; do
        [[ ! -f $source ]] || inputs+=("$source")
    done
    "$SHA256_PATH" "${inputs[@]}" >"$ARTIFACT_DIR/input-sha256.txt"
}

on_exit() {
    local rc=$?
    trap - EXIT
    set +e
    {
        printf 'result=%s\n' "$RESULT"
        printf 'exit_code=%s\n' "$rc"
        printf 'started_at=%s\n' "$STARTED_AT"
        printf 'finished_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$ARTIFACT_DIR/result.txt"

    shopt -s nullglob
    local artifacts=(
        "$ARTIFACT_DIR"/*.log
        "$ARTIFACT_DIR"/commands.txt
        "$ARTIFACT_DIR"/input-sha256.txt
        "$ARTIFACT_DIR"/provenance.txt
        "$ARTIFACT_DIR"/result.txt
    )
    if ((${#artifacts[@]})); then
        "$SHA256_PATH" "${artifacts[@]}" >"$ARTIFACT_DIR/artifact-sha256.txt"
    fi
    printf 'Artifacts: %s\n' "$ARTIFACT_DIR"
    exit "$rc"
}
trap on_exit EXIT

write_provenance
write_input_hashes
: >"$ARTIFACT_DIR/commands.txt"

record_command() {
    local label=$1
    shift
    {
        printf '%s:' "$label"
        printf ' %q' "$@"
        printf '\n'
    } >>"$ARTIFACT_DIR/commands.txt"
}

run_foreground() {
    local label=$1
    local duration=$2
    local log=$3
    shift 3

    record_command "$label" "$@"
    {
        printf 'case=%s\n' "$label"
        printf 'watchdog=%s kill_grace=%s\n' "$duration" "$KILL_GRACE"
        printf 'command:'
        printf ' %q' "$@"
        printf '\n'
    } >"$log"

    local rc
    set +e
    "$TIMEOUT_PATH" --foreground --signal=TERM --kill-after="$KILL_GRACE" \
        "$duration" "$@" 2>&1 | tee -a "$log"
    rc=${PIPESTATUS[0]}
    set -e

    if ((rc == 124 || rc == 137)); then
        echo "ERROR: $label exceeded its $duration watchdog (exit $rc)" |
            tee -a "$log" >&2
        return 1
    fi
    if ((rc != 0)); then
        echo "ERROR: $label failed with exit $rc" | tee -a "$log" >&2
        return 1
    fi
}

verify_case_evidence() {
    local label=$1
    local log=$2
    local expected_pdi_traces=$3
    local pass_text=$4

    grep -Fq "$pass_text" "$log" ||
        die "$label did not report its host-reference PASS"
    grep -Eq '^\[rp1-trace\] written=[0-9]+ entries=[0-9]+' "$log" ||
        die "$label is missing VRT_RP1_TRACE output"
    if grep -Eq '^\[rp1-trace\].*([[:space:]]|^)overflow([[:space:]]|$)' "$log"; then
        die "$label overflowed the RP1 trace ring"
    fi
    grep -Eq 'event=GRAPH_DONE\([0-9]+\).*aux0=0x0([[:space:]]|$)' "$log" ||
        die "$label is missing successful RP1 GRAPH_DONE evidence"

    # Every diagnostic is one complete result line. Validate all fields rather
    # than accepting a success token detached from recovery, image, timing,
    # trace, or quiescence evidence.
    local result_count=0
    local last_result_seq=
    local line
    while IFS= read -r line; do
        [[ -n $line ]] || continue
        local fields
        read -r -a fields <<<"$line"
        ((${#fields[@]} == 10)) ||
            die "$label has a malformed RP1 result diagnostic: $line"
        [[ ${fields[0]} == "[rp1-result]" ]] ||
            die "$label has an invalid RP1 result prefix"

        local seq=${fields[1]#seq=}
        local outcome=${fields[2]#outcome=}
        local flags=${fields[3]#flags=}
        local image=${fields[4]#image=}
        local completed=${fields[5]#completed=}
        local graph_ticks=${fields[6]#graph_ticks=}
        local publish_ticks=${fields[7]#publish_ticks=}
        local trace_write_idx=${fields[8]#trace_write_idx=}
        local quiescence=${fields[9]#quiescence=}

        [[ $seq =~ ^[0-9]+$ ]] ||
            die "$label result has a non-numeric sequence"
        [[ $outcome == "SUCCESS(1)" ]] ||
            die "$label result outcome is $outcome, expected SUCCESS(1)"
        [[ $flags =~ ^0[xX][0-9a-fA-F]+$ ]] ||
            die "$label result has malformed flags: $flags"
        local flag_value=$((flags))
        (( (flag_value & 0xffffffc0) == 0 )) ||
            die "$label result reports unknown protocol-v6 flags: $flags"
        (( (flag_value & 0x08) != 0 )) ||
            die "$label result does not report trace-enabled execution"
        (( (flag_value & 0x17) == 0 )) ||
            die "$label result reports failure/recovery flags: $flags"
        [[ $image =~ ^KNOWN\(1\):([1-9][0-9]*)$ ]] ||
            die "$label result does not report a known non-zero image: $image"
        [[ $completed =~ ^[1-9][0-9]*$ ]] ||
            die "$label result has no completed operations"
        [[ $graph_ticks =~ ^[0-9]+$ &&
           $publish_ticks =~ ^[0-9]+$ &&
           $trace_write_idx =~ ^[1-9][0-9]*$ ]] ||
            die "$label result has malformed timing/trace fields"
        local publish_delta=$(((publish_ticks - graph_ticks) & 0xffffffff))
        ((publish_delta < 0x80000000)) ||
            die "$label result publication precedes graph completion"
        [[ $quiescence == "0/0/0" ]] ||
            die "$label result required quiescence: $quiescence"

        last_result_seq=$seq
        ((result_count += 1))
    done < <(grep -E '^\[rp1-result\]' "$log" || true)
    ((result_count > 0)) ||
        die "$label is missing VRT_RP1_RESULT output"
    CASE_LAST_RESULT_SEQ=$last_result_seq

    local pdi_trace_count
    pdi_trace_count=$(grep -Ec \
        'event=PDI_LOAD\([0-9]+\).*aux0=0x0[[:space:]]+aux1=0x[0-9a-fA-F]+([[:space:]]|$)' \
        "$log" || true)
    ((pdi_trace_count >= expected_pdi_traces)) ||
        die "$label has $pdi_trace_count successful PDI trace record(s), expected at least $expected_pdi_traces"
}

run_case() {
    local label=$1
    local expected_pdis=$2
    local pass_text=$3
    shift 3
    local log="$ARTIFACT_DIR/$label.log"
    run_foreground "$label" "$CASE_TIMEOUT" "$log" \
        "$ENV_PATH" VRT_RP1_TRACE=1 VRT_RP1_RESULT=1 "$@"
    verify_case_evidence "$label" "$log" "$expected_pdis" "$pass_text"
}

field_value() {
    local log=$1
    local field=$2
    awk -v field="$field" \
        '$1 == field && $2 == "=" { print $3; found = 1; exit }
         END { if (!found) exit 1 }' "$log"
}

is_zero_value() {
    [[ $1 =~ ^0+$ || $1 =~ ^0[xX]0+$ ]]
}

RP1_LAST_SEQ=
RP1_LAST_RESULT_SEQ=
CASE_LAST_RESULT_SEQ=
run_rp1_dump() {
    local label=$1
    local log="$ARTIFACT_DIR/$label.log"
    run_foreground "$label" "$DIAGNOSTIC_TIMEOUT" "$log" \
        "$V80_SMI_PATH" debug rp1-dump \
        -d "$BDF" -b "$RP1_BAR" --ctrl-offset "$RP1_CTRL_OFFSET"

    grep -Fq '(SQR1)' "$log" || die "$label did not find RP1 control-block magic"
    grep -Fq 'Protocol contract: compatible' "$log" ||
        die "$label found an incompatible RP1 firmware contract"
    grep -Eq '^Liveness: heartbeat advanced [0-9]+ -> [0-9]+ \(running\)$' "$log" ||
        die "$label did not observe an advancing RP1 heartbeat"
    grep -Eq 'terminal_error_node[[:space:]]+= 0x[fF]{8} \(none\)' "$log" ||
        die "$label found a latched terminal-error node"

    local version missing platform state error detail aux seq done_seq
    version=$(field_value "$log" version) || die "$label is missing protocol version"
    missing=$(field_value "$log" missing_capabilities) ||
        die "$label is missing the capability report"
    platform=$(field_value "$log" pdi_ipi_platform_id) ||
        die "$label is missing generated platform/IPI identity"
    state=$(field_value "$log" rp1_state) || die "$label is missing RP1 state"
    error=$(field_value "$log" rp1_error_code) || die "$label is missing RP1 error code"
    detail=$(field_value "$log" terminal_error_detail) ||
        die "$label is missing terminal error detail"
    aux=$(field_value "$log" terminal_error_aux) ||
        die "$label is missing terminal error auxiliary detail"
    seq=$(field_value "$log" graph_seq) || die "$label is missing graph_seq"
    done_seq=$(field_value "$log" graph_done_seq) ||
        die "$label is missing graph_done_seq"

    [[ $version == 6 ]] || die "$label reports protocol v$version, expected v6"
    is_zero_value "$missing" || die "$label reports missing capabilities: $missing"
    ! is_zero_value "$platform" || die "$label reports an unknown platform/IPI identity"
    [[ $state == 1 ]] || die "$label reports RP1 state $state, expected READY (1)"
    is_zero_value "$error" || die "$label reports RP1 error code $error"
    is_zero_value "$detail" || die "$label reports terminal error detail $detail"
    is_zero_value "$aux" || die "$label reports terminal error auxiliary detail $aux"
    [[ $seq =~ ^[0-9]+$ && $done_seq =~ ^[0-9]+$ ]] ||
        die "$label reports non-numeric graph sequence fields"
    [[ $seq == "$done_seq" ]] ||
        die "$label reports incomplete graph sequence: graph_seq=$seq graph_done_seq=$done_seq"

    local result_magic result_seq result_outcome result_flags
    local result_error result_node result_opcode result_detail result_aux
    local result_image result_image_state result_completed
    local result_graph_ticks result_publish_ticks result_trace_idx
    local result_quiescence
    result_magic=$(field_value "$log" result.magic) ||
        die "$label is missing graph-result magic"
    result_seq=$(field_value "$log" result.graph_seq) ||
        die "$label is missing graph-result sequence"
    result_outcome=$(field_value "$log" result.outcome) ||
        die "$label is missing graph-result outcome"
    result_flags=$(field_value "$log" result.flags) ||
        die "$label is missing graph-result flags"
    result_error=$(field_value "$log" result.error_code) ||
        die "$label is missing graph-result error code"
    result_node=$(field_value "$log" result.terminal_node) ||
        die "$label is missing graph-result terminal node"
    result_opcode=$(field_value "$log" result.terminal_opcode) ||
        die "$label is missing graph-result terminal opcode"
    result_detail=$(field_value "$log" result.error_detail) ||
        die "$label is missing graph-result error detail"
    result_aux=$(field_value "$log" result.error_aux) ||
        die "$label is missing graph-result error auxiliary detail"
    result_image=$(field_value "$log" result.active_image_id) ||
        die "$label is missing graph-result active image"
    result_image_state=$(field_value "$log" result.image_state) ||
        die "$label is missing graph-result image state"
    result_completed=$(field_value "$log" result.completed_operations) ||
        die "$label is missing graph-result completed-operation count"
    result_graph_ticks=$(field_value "$log" result.graph_elapsed_ticks) ||
        die "$label is missing graph-result graph timing"
    result_publish_ticks=$(field_value "$log" result.publish_elapsed_ticks) ||
        die "$label is missing graph-result publication timing"
    result_trace_idx=$(field_value "$log" result.trace_write_idx) ||
        die "$label is missing graph-result trace cursor"
    result_quiescence=$(field_value "$log" result.quiescence) ||
        die "$label is missing graph-result quiescence"

    if is_zero_value "$result_magic"; then
        [[ $done_seq == 0 ]] ||
            die "$label has no committed result for graph_done_seq=$done_seq"
        is_zero_value "$result_seq" ||
            die "$label has an uncommitted non-zero result sequence"
        is_zero_value "$result_outcome" ||
            die "$label has an uncommitted terminal result outcome"
    else
        [[ ${result_magic,,} == 0x52534c54 ]] ||
            die "$label has invalid graph-result magic: $result_magic"
        [[ $result_seq == "$done_seq" ]] ||
            die "$label result sequence $result_seq does not match graph_done_seq=$done_seq"
        [[ $result_outcome == 1 ]] ||
            die "$label result outcome is $result_outcome, expected SUCCESS (1)"
        [[ $result_flags =~ ^0[xX][0-9a-fA-F]+$ ]] ||
            die "$label has malformed graph-result flags: $result_flags"
        local result_flag_value=$((result_flags))
        (( (result_flag_value & 0xffffffc0) == 0 )) ||
            die "$label result reports unknown protocol-v6 flags: $result_flags"
        (( (result_flag_value & 0x17) == 0 )) ||
            die "$label result reports failure/recovery flags: $result_flags"
        if (( (result_flag_value & 0x08) != 0 )); then
            [[ $result_trace_idx =~ ^[1-9][0-9]*$ ]] ||
                die "$label traced result has no trace records"
        else
            is_zero_value "$result_trace_idx" ||
                die "$label untraced result carries trace cursor $result_trace_idx"
        fi
        is_zero_value "$result_error" ||
            die "$label result reports error code $result_error"
        [[ ${result_node,,} == 0xffffffff ]] ||
            die "$label result reports terminal node $result_node"
        [[ ${result_opcode,,} == 0xffffffff ]] ||
            die "$label result reports terminal opcode $result_opcode"
        is_zero_value "$result_detail" ||
            die "$label result reports error detail $result_detail"
        is_zero_value "$result_aux" ||
            die "$label result reports error auxiliary detail $result_aux"
        if [[ $result_image_state == 0 ]]; then
            is_zero_value "$result_image" ||
                die "$label result NONE image state carries id $result_image"
        elif [[ $result_image_state == 1 ]]; then
            ! is_zero_value "$result_image" ||
                die "$label result KNOWN image state carries id zero"
        else
            die "$label result reports unhealthy image state $result_image_state"
        fi
        [[ $result_completed =~ ^[1-9][0-9]*$ ]] ||
            die "$label result has no completed operations"
        [[ $result_graph_ticks =~ ^[0-9]+$ &&
           $result_publish_ticks =~ ^[0-9]+$ &&
           $result_trace_idx =~ ^[0-9]+$ ]] ||
            die "$label result has malformed timing/trace fields"
        local result_publish_delta=$((
            (result_publish_ticks - result_graph_ticks) & 0xffffffff
        ))
        ((result_publish_delta < 0x80000000)) ||
            die "$label result publication precedes graph completion"
        is_zero_value "$result_quiescence" ||
            die "$label result reports quiescence $result_quiescence"
    fi
    RP1_LAST_SEQ=$seq
    RP1_LAST_RESULT_SEQ=$result_seq
}

run_case_and_dump() {
    local label=$1
    local expected_pdis=$2
    local pass_text=$3
    shift 3
    local previous_seq=$RP1_LAST_SEQ
    run_case "$label" "$expected_pdis" "$pass_text" "$@"
    local case_result_seq=$CASE_LAST_RESULT_SEQ
    run_rp1_dump "$label.rp1"
    [[ $RP1_LAST_SEQ != "$previous_seq" ]] ||
        die "$label returned success without advancing graph_done_seq"
    [[ $RP1_LAST_RESULT_SEQ == "$case_result_seq" ]] ||
        die "$label VRT result seq=$case_result_seq disagrees with RP1 dump seq=$RP1_LAST_RESULT_SEQ"
}

socket_args=()
if [[ -n $VRTD_SOCKET ]]; then
    socket_args=(--socket "$VRTD_SOCKET")
fi

# This exact order is the acceptance contract. Nothing below resets the board,
# restarts vrtd, submits an SMI probe graph, or loads a PDI outside the examples.
run_rp1_dump preflight.rp1

run_case_and_dump \
    00-first 4 \
    'PASS: CPU + FPGA graph with two vbins and explicit reprogram nodes completed.' \
    "$MULTI_BIN" --bdf "$BDF" "${socket_args[@]}" \
    --vbin-a "$MULTI_VBIN_A" --vbin-b "$MULTI_VBIN_B" \
    --iterations 2 --elements 16 --input-offset 0

# Different loop count and generated input make stale first-run output fail the
# application's host reference even if RP1 state or buffers leak across runs.
run_case_and_dump \
    00-second 6 \
    'PASS: CPU + FPGA graph with two vbins and explicit reprogram nodes completed.' \
    "$MULTI_BIN" --bdf "$BDF" "${socket_args[@]}" \
    --vbin-a "$MULTI_VBIN_A" --vbin-b "$MULTI_VBIN_B" \
    --iterations 3 --elements 16 --input-offset 4096

run_case_and_dump \
    01-edge 1 \
    'PASS: illumination-normalized edge detection matches the host reference.' \
    "$EDGE_BIN" --bdf "$BDF" "${socket_args[@]}" \
    --vbin "$EDGE_VBIN" --k 16 --elements 16

run_case_and_dump \
    02-bright 4 \
    'PASS: iterative sharpening with adaptive gain matches the host reference.' \
    "$SHARPEN_BIN" --bdf "$BDF" "${socket_args[@]}" \
    --vbin "$SHARPEN_VBIN" --iterations 4 --alpha 1 \
    --threshold 0 --boost 2 --elements 16

run_case_and_dump \
    02-dark 4 \
    'PASS: iterative sharpening with adaptive gain matches the host reference.' \
    "$SHARPEN_BIN" --bdf "$BDF" "${socket_args[@]}" \
    --vbin "$SHARPEN_VBIN" --iterations 4 --alpha 1 \
    --threshold 1000000 --boost 2 --elements 16

RESULT=PASS
echo "PASS: graph hardware acceptance completed without a reset."
