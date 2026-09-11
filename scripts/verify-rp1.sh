#!/usr/bin/env bash
# Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Verify protocol-v6 firmware liveness and its last committed graph result.
# Usage: verify_rp1.sh [control_block_base] [bdf]
#   control_block_base: DDR base of RP1 control block (default: 0x30000000)
#   bdf:     PCIe BDF of the device (default: auto-detect)
set -euo pipefail

CTRL_BASE_TEXT=${1:-0x30000000}
CTRL_BASE=$((CTRL_BASE_TEXT))
STATE_ADDR=$((CTRL_BASE + 0x30))
HEARTBEAT_ADDR=$((CTRL_BASE + 0x3C))
SEQUENCE_ADDR=$((CTRL_BASE + 0x20))
RESULT_ADDR=$((CTRL_BASE + 0x80))
CONTRACT_ADDR=$((CTRL_BASE + 0x64))

BDF_ARGS=()
if [[ -n ${2:-} ]]; then
    BDF_ARGS=(--device "$2")
fi

# Print one fatal verification diagnostic and exit.
fail() {
    echo "ERROR: $*" >&2
    exit 1
}

# read_words ADDRESS COUNT DESCRIPTION populates WORDS with exact uint32 output.
WORDS=()
read_words() {
    local address=$1
    local count=$2
    local description=$3
    local output
    output=$(v80-smi debug mem-poke "${BDF_ARGS[@]}" \
        --region RAW --read --hex "$address" \
        --word-size 4 --count "$count" 2>&1) ||
        fail "Failed to read $description at $address: $output"
    mapfile -t WORDS < <(
        printf '%s\n' "$output" |
            awk '/^0[xX][0-9a-fA-F]+$/ { print }'
    )
    ((${#WORDS[@]} == count)) ||
        fail "$description returned ${#WORDS[@]} words, expected $count: $output"
}

# Return true when a hexadecimal mem-poke word is zero.
is_zero_word() {
    (($1 == 0))
}

printf 'Checking RP1 control block at 0x%x (result 0x%x, heartbeat 0x%x)...\n' \
    "$CTRL_BASE" "$RESULT_ADDR" "$HEARTBEAT_ADDR"

# Protocol v6 retains five CQ-era words as zero-only ABI reservations.
read_words "$(printf '0x%x' "$CTRL_BASE")" 12 "control-block header"
magic=${WORDS[0],,}
version=${WORDS[1]}
node_count=${WORDS[2]}
reserved_0c=${WORDS[3]}
reserved_18=${WORDS[6]}
reserved_1c=${WORDS[7]}
reserved_28=${WORDS[10]}
reserved_2c=${WORDS[11]}
printf 'Header: magic=%s version=%u node_count=%u\n' \
    "$magic" "$((version))" "$((node_count))"
printf 'Reserved: [0x0c]=%s [0x18]=%s [0x1c]=%s [0x28]=%s [0x2c]=%s\n' \
    "$reserved_0c" "$reserved_18" "$reserved_1c" \
    "$reserved_28" "$reserved_2c"
[[ $magic == 0x53515231 ]] ||
    fail "RP1 control magic is $magic, expected 0x53515231 (SQR1)"
((version == 6)) ||
    fail "RP1 protocol is v$((version)), expected v6"
for offset_and_word in \
    "0x0c:$reserved_0c" "0x18:$reserved_18" "0x1c:$reserved_1c" \
    "0x28:$reserved_28" "0x2c:$reserved_2c"; do
    offset=${offset_and_word%%:*}
    word=${offset_and_word#*:}
    is_zero_word "$word" ||
        fail "protocol-v6 reserved control word $offset is non-zero: $word"
done

# Required capabilities and generated IPI identity commit graph-result behavior.
read_words "$(printf '0x%x' "$CONTRACT_ADDR")" 2 "protocol capabilities"
capabilities=${WORDS[0]}
platform_id=${WORDS[1]}
printf 'Contract: capabilities=%s platform_id=%s\n' \
    "$capabilities" "$platform_id"
(( (capabilities & 0x7b) == 0x7b )) ||
    fail "RP1 is missing required protocol-v6 capabilities: $capabilities"
! is_zero_word "$platform_id" ||
    fail "RP1 reports an unknown platform/IPI identity"

# READY plus a zero live error record is required for safe reuse.
read_words "$(printf '0x%x' "$STATE_ADDR")" 4 "state diagnostics"
state=${WORDS[0]}
error_code=${WORDS[1]}
current_node=${WORDS[2]}
heartbeat_before=${WORDS[3]}
printf 'State: state=%u error=%u current_node=%s heartbeat=%u\n' \
    "$((state))" "$((error_code))" "$current_node" "$((heartbeat_before))"
((state == 1)) || fail "RP1 state is $((state)), expected READY (1)"
is_zero_word "$error_code" ||
    fail "RP1 live error code is $((error_code)), expected zero"

# Exact sequence equality releases one immutable graph-result snapshot.
read_words "$(printf '0x%x' "$SEQUENCE_ADDR")" 2 "graph sequences"
graph_seq=${WORDS[0]}
graph_done_seq=${WORDS[1]}
((graph_seq == graph_done_seq)) ||
    fail "graph_seq=$((graph_seq)) does not match graph_done_seq=$((graph_done_seq))"

read_words "$(printf '0x%x' "$RESULT_ADDR")" 16 "graph result"
result_magic=${WORDS[0],,}
result_seq=${WORDS[1]}
result_outcome=${WORDS[2]}
result_flags=${WORDS[3]}
result_error=${WORDS[4]}
result_node=${WORDS[5],,}
result_opcode=${WORDS[6],,}
result_detail=${WORDS[7]}
result_aux=${WORDS[8]}
result_image=${WORDS[9]}
result_image_state=${WORDS[10]}
result_completed=${WORDS[11]}
result_graph_ticks=${WORDS[12]}
result_publish_ticks=${WORDS[13]}
result_trace_idx=${WORDS[14]}
result_quiescence=${WORDS[15]}

printf 'Result: magic=%s seq=%u outcome=%u flags=%s error=%u node=%s opcode=%s\n' \
    "$result_magic" "$((result_seq))" "$((result_outcome))" "$result_flags" \
    "$((result_error))" "$result_node" "$result_opcode"
printf '        detail=%s aux=%s image_state=%u image_id=%u completed=%u\n' \
    "$result_detail" "$result_aux" "$((result_image_state))" \
    "$((result_image))" "$((result_completed))"
printf '        graph_ticks=%u publish_ticks=%u trace_write_idx=%u quiescence=%s\n' \
    "$((result_graph_ticks))" "$((result_publish_ticks))" \
    "$((result_trace_idx))" "$result_quiescence"

if is_zero_word "$result_magic"; then
    is_zero_word "$graph_done_seq" ||
        fail "graph_done_seq is non-zero but no result is committed"
    is_zero_word "$result_seq" ||
        fail "uncommitted result carries sequence $((result_seq))"
    is_zero_word "$result_outcome" ||
        fail "uncommitted result carries outcome $((result_outcome))"
    echo "Result status: no graph has completed since firmware boot."
else
    [[ $result_magic == 0x52534c54 ]] ||
        fail "graph-result magic is $result_magic, expected 0x52534c54 (RSLT)"
    ((result_seq == graph_done_seq)) ||
        fail "result seq=$((result_seq)) does not match graph_done_seq=$((graph_done_seq))"
    ((result_outcome == 1)) ||
        fail "last graph outcome is $((result_outcome)), expected SUCCESS (1)"
    (( (result_flags & 0xffffffc0) == 0 )) ||
        fail "last graph reports unknown protocol-v6 flags: $result_flags"
    (( (result_flags & 0x17) == 0 )) ||
        fail "last graph reports failure/recovery flags: $result_flags"
    if (( (result_flags & 0x08) != 0 )); then
        ((result_trace_idx > 0)) ||
            fail "trace-enabled result reports no trace records"
    else
        is_zero_word "$result_trace_idx" ||
            fail "untraced result carries trace cursor $((result_trace_idx))"
    fi
    is_zero_word "$result_error" ||
        fail "successful result carries error code $((result_error))"
    [[ $result_node == 0xffffffff ]] ||
        fail "successful result carries terminal node $result_node"
    [[ $result_opcode == 0xffffffff ]] ||
        fail "successful result carries terminal opcode $result_opcode"
    is_zero_word "$result_detail" ||
        fail "successful result carries error detail $result_detail"
    is_zero_word "$result_aux" ||
        fail "successful result carries error auxiliary detail $result_aux"
    if ((result_image_state == 0)); then
        is_zero_word "$result_image" ||
            fail "NONE image state carries id $((result_image))"
    elif ((result_image_state == 1)); then
        ! is_zero_word "$result_image" ||
            fail "KNOWN image state carries id zero"
    else
        fail "last graph reports unhealthy image state $((result_image_state))"
    fi
    ((result_completed > 0)) ||
        fail "successful result reports no completed operations"
    publish_delta=$((
        (result_publish_ticks - result_graph_ticks) & 0xffffffff
    ))
    ((publish_delta < 0x80000000)) ||
        fail "result publication timing precedes graph completion"
    is_zero_word "$result_quiescence" ||
        fail "successful result required quiescence: $result_quiescence"
fi

# Heartbeat liveness is independent of whether a graph has run.
sleep 1
read_words "$(printf '0x%x' "$HEARTBEAT_ADDR")" 1 "heartbeat"
heartbeat_after=${WORDS[0]}
((heartbeat_after != heartbeat_before)) ||
    fail "RP1 heartbeat is unchanged at $((heartbeat_before))"

printf 'PASS: RP1 protocol-v6 result is healthy; heartbeat advanced %u -> %u.\n' \
    "$((heartbeat_before))" "$((heartbeat_after))"
