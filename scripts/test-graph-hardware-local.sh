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

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
ACCEPTANCE="$SCRIPT_DIR/extra/test-graph-hardware.sh"

git -C "$REPO_ROOT" check-ignore -q tmp ||
    {
        echo "ERROR: $REPO_ROOT/tmp must be gitignored before running local tests" >&2
        exit 2
    }
mkdir -p -- "$REPO_ROOT/tmp"
WORK=$(mktemp -d --tmpdir="$REPO_ROOT/tmp" graph-hardware-local.XXXXXX)

cleanup() {
    local rc=$?
    if [[ ${KEEP_TEST_TMP:-0} == 1 ]]; then
        echo "Local test scratch retained at $WORK"
    elif [[ $WORK == "$REPO_ROOT/tmp/"* ]]; then
        rm -rf -- "$WORK"
    fi
    exit "$rc"
}
trap cleanup EXIT

FAKE_ROOT="$WORK/examples/graph"
FAKE_BIN="$WORK/bin"
FAKE_STATE="$WORK/state"
mkdir -p -- \
    "$FAKE_ROOT/00_multi_image_pipeline/build" \
    "$FAKE_ROOT/01_edge_detection/build" \
    "$FAKE_ROOT/02_sharpen_loop/build" \
    "$FAKE_BIN" "$FAKE_STATE"

cat >"$FAKE_BIN/graph-app" <<'EOF'
#!/bin/bash
set -euo pipefail

: "${FAKE_STATE_DIR:?}"
[[ ${VRT_RP1_TRACE:-} == 1 ]] || {
    echo "VRT_RP1_TRACE was not enabled" >&2
    exit 91
}
[[ ${VRT_RP1_RESULT:-} == 1 ]] || {
    echo "VRT_RP1_RESULT was not enabled" >&2
    exit 92
}

name=$(basename -- "$0")
printf '%s' "$name" >>"$FAKE_STATE_DIR/invocations"
printf ' %q' "$@" >>"$FAKE_STATE_DIR/invocations"
printf '\n' >>"$FAKE_STATE_DIR/invocations"

iterations=1
input_offset=
threshold=
while (($#)); do
    case $1 in
    --iterations)
        iterations=$2
        shift 2
        ;;
    --input-offset)
        input_offset=$2
        shift 2
        ;;
    --threshold)
        threshold=$2
        shift 2
        ;;
    *)
        shift
        ;;
    esac
done

if [[ ${FAKE_MODE:-success} == timeout ]]; then
    while :; do :; done
fi

case $name in
multi_image_pipeline)
    multi_count=$(<"$FAKE_STATE_DIR/multi-count")
    if ((multi_count == 0)); then
        [[ $iterations == 2 && $input_offset == 0 ]] || exit 93
    elif ((multi_count == 1)); then
        [[ $iterations == 3 && $input_offset == 4096 ]] || exit 94
    else
        exit 95
    fi
    printf '%s\n' "$((multi_count + 1))" >"$FAKE_STATE_DIR/multi-count"
    pdis=$((2 * iterations))
    pass='PASS: CPU + FPGA graph with two vbins and explicit reprogram nodes completed.'
    ;;
edge_detection)
    pdis=1
    pass='PASS: illumination-normalized edge detection matches the host reference.'
    ;;
sharpen_loop)
    [[ $threshold == 0 || $threshold == 1000000 ]] || exit 96
    pdis=$iterations
    pass='PASS: iterative sharpening with adaptive gain matches the host reference.'
    ;;
*)
    exit 97
    ;;
esac

seq=$(<"$FAKE_STATE_DIR/seq")
printf '%s\n' "$((seq + 1))" >"$FAKE_STATE_DIR/seq"

echo "$pass"
if [[ ${FAKE_MODE:-success} == missing-pdi ]]; then
    pdis=0
fi

trace_suffix=
if [[ ${FAKE_MODE:-success} == overflow ]]; then
    trace_suffix=' overflow'
fi
result_flags=0x8
if [[ $name == sharpen_loop ]]; then
    # The unchosen conditional branch remains pending on successful hardware.
    result_flags=0x28
fi
printf '%s\n' "$result_flags" >"$FAKE_STATE_DIR/result-flags"
if [[ ${FAKE_MODE:-success} != missing-result ]]; then
    result_outcome='SUCCESS(1)'
    if [[ ${FAKE_MODE:-success} == failed-result ]]; then
        result_outcome='FAILED(2)'
    fi
    echo "[rp1-result] seq=$((seq + 1)) outcome=$result_outcome flags=$result_flags image=KNOWN(1):1 completed=$((pdis + 1)) graph_ticks=$((pdis + 10)) publish_ticks=$((pdis + 12)) trace_write_idx=$((pdis + 2)) quiescence=0/0/0"
fi

echo "[rp1-trace] written=$((pdis + 2)) entries=$((pdis + 2))$trace_suffix"
for ((i = 0; i < pdis; ++i)); do
    echo "  trace[$i] t=$((i + 1)) event=PDI_LOAD(9) node=$i aux0=0x0 aux1=0x0"
done
if [[ ${FAKE_MODE:-success} != missing-graph ]]; then
    echo "  trace[$((pdis + 1))] t=$((pdis + 2)) event=GRAPH_DONE(11) node=65535 aux0=0x0 aux1=0x1"
fi
EOF
chmod +x "$FAKE_BIN/graph-app"

cat >"$FAKE_BIN/v80-smi" <<'EOF'
#!/bin/bash
set -euo pipefail

: "${FAKE_STATE_DIR:?}"
[[ ${1:-} == debug && ${2:-} == rp1-dump ]] || {
    echo "Only the read-only rp1-dump command is permitted in this test" >&2
    exit 98
}

seq=$(<"$FAKE_STATE_DIR/seq")
dump_count=$(<"$FAKE_STATE_DIR/dump-count")
printf '%s\n' "$((dump_count + 1))" >"$FAKE_STATE_DIR/dump-count"
hb=$((1000 + dump_count * 10))

result_magic=0x00000000
result_outcome=0
result_flags=0x00000000
result_image=0
result_image_state=0
result_completed=0
result_graph_ticks=0
result_publish_ticks=0
result_trace_idx=0
if ((seq != 0)); then
    result_magic=0x52534c54
    result_outcome=1
    result_flags=$(<"$FAKE_STATE_DIR/result-flags")
    result_image=1
    result_image_state=1
    result_completed=$((seq + 1))
    result_graph_ticks=$((seq + 10))
    result_publish_ticks=$((seq + 12))
    result_trace_idx=$((seq + 2))
fi

cat <<DUMP
RP1 control block @ R5 0x30000000 (BAR4 + 0x4000000):
  magic            = 0x53515231 (SQR1)
  version          = 6
  capabilities     = 0x0000007b
  required_capabilities = 0x0000007b
  missing_capabilities  = 0x00000000
  pdi_ipi_platform_id   = 0x1234abcd (generated platform/IPI identity)
  graph_seq        = $seq
  graph_done_seq   = $seq
  rp1_state        = 1 (READY)
  rp1_error_code   = 0
  rp1_current_node = 4294967295
  terminal_error_node   = 0xffffffff (none)
  terminal_error_detail = 0x00000000
  terminal_error_aux    = 0x00000000
  heartbeat        = $hb
Graph result:
  result.magic              = $result_magic
  result.graph_seq          = $seq
  result.outcome            = $result_outcome
  result.flags              = $result_flags
  result.error_code         = 0
  result.terminal_node      = 0xffffffff (none)
  result.terminal_opcode    = 0xffffffff (NONE)
  result.error_detail       = 0x00000000
  result.error_aux          = 0x00000000
  result.active_image_id    = $result_image
  result.image_state        = $result_image_state
  result.completed_operations = $result_completed
  result.graph_elapsed_ticks  = $result_graph_ticks
  result.publish_elapsed_ticks = $result_publish_ticks
  result.trace_write_idx      = $result_trace_idx
  result.quiescence           = 0x00000000
Protocol contract: compatible
Liveness: heartbeat advanced $hb -> $((hb + 1)) (running)
DUMP
EOF
chmod +x "$FAKE_BIN/v80-smi"

ln -s "$FAKE_BIN/graph-app" \
    "$FAKE_ROOT/00_multi_image_pipeline/build/multi_image_pipeline"
ln -s "$FAKE_BIN/graph-app" \
    "$FAKE_ROOT/01_edge_detection/build/edge_detection"
ln -s "$FAKE_BIN/graph-app" \
    "$FAKE_ROOT/02_sharpen_loop/build/sharpen_loop"

printf 'fake multi A vbin\n' \
    >"$FAKE_ROOT/00_multi_image_pipeline/build/multi_image_pipeline_a_hw.vbin"
printf 'fake multi B vbin\n' \
    >"$FAKE_ROOT/00_multi_image_pipeline/build/multi_image_pipeline_b_hw.vbin"
printf 'fake edge vbin\n' \
    >"$FAKE_ROOT/01_edge_detection/build/edge_detection_hw.vbin"
printf 'fake sharpen vbin\n' \
    >"$FAKE_ROOT/02_sharpen_loop/build/sharpen_loop_hw.vbin"

reset_state() {
    printf '0\n' >"$FAKE_STATE/seq"
    printf '0\n' >"$FAKE_STATE/multi-count"
    printf '0\n' >"$FAKE_STATE/dump-count"
    printf '0x0\n' >"$FAKE_STATE/result-flags"
    : >"$FAKE_STATE/invocations"
}

run_acceptance() {
    local mode=$1
    local artifacts=$2
    local case_timeout=${3:-5s}
    env \
        FAKE_MODE="$mode" \
        FAKE_STATE_DIR="$FAKE_STATE" \
        V80_SMI="$FAKE_BIN/v80-smi" \
        ARTIFACT_DIR="$artifacts" \
        CASE_TIMEOUT="$case_timeout" \
        KILL_GRACE=1s \
        DIAGNOSTIC_TIMEOUT=2s \
        "$ACCEPTANCE" 0000:01:00.0 "$FAKE_ROOT"
}

assert_contains() {
    local text=$1
    local file=$2
    grep -Fq -- "$text" "$file" || {
        echo "ERROR: expected '$text' in $file" >&2
        exit 1
    }
}

bash -n "$ACCEPTANCE"
bash -n "${BASH_SOURCE[0]}"

reset_state
HAPPY="$WORK/artifacts-happy"
run_acceptance success "$HAPPY" >"$WORK/happy.stdout" 2>&1
assert_contains 'result=PASS' "$HAPPY/result.txt"
assert_contains 'reset_policy=no-reset' "$HAPPY/provenance.txt"
assert_contains '--iterations 2 --elements 16 --input-offset 0' "$FAKE_STATE/invocations"
assert_contains '--iterations 3 --elements 16 --input-offset 4096' "$FAKE_STATE/invocations"
assert_contains '--threshold 0' "$FAKE_STATE/invocations"
assert_contains '--threshold 1000000' "$FAKE_STATE/invocations"
[[ $(wc -l <"$FAKE_STATE/invocations") == 5 ]] ||
    {
        echo "ERROR: expected exactly five graph application invocations" >&2
        exit 1
    }
sha256sum -c "$HAPPY/input-sha256.txt" >/dev/null
sha256sum -c "$HAPPY/artifact-sha256.txt" >/dev/null

expect_failure() {
    local mode=$1
    local name=$2
    local timeout=${3:-5s}
    local artifacts="$WORK/artifacts-$name"
    reset_state
    if run_acceptance "$mode" "$artifacts" "$timeout" >"$WORK/$name.stdout" 2>&1; then
        echo "ERROR: $name acceptance scenario unexpectedly passed" >&2
        exit 1
    fi
    assert_contains 'result=FAIL' "$artifacts/result.txt"
    [[ -s $artifacts/artifact-sha256.txt ]] ||
        {
            echo "ERROR: $name failure did not produce artifact hashes" >&2
            exit 1
        }
}

expect_failure missing-pdi missing-pdi
expect_failure missing-result missing-result
expect_failure failed-result failed-result
expect_failure missing-graph missing-graph
expect_failure overflow trace-overflow
expect_failure timeout watchdog 1s

echo "PASS: local graph hardware acceptance-script tests completed."
