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
#
# Submit-side half of the LSF example launcher: run a command on a compute node
# and block until it finishes, propagating its exit status. That is the whole
# of the SLASH_TOOL_LAUNCHER contract, so this is a drop-in value for it:
#
#   1. Per tool invocation. slashkit prefixes $SLASH_TOOL_LAUNCHER to every
#      tool command line it builds, so each one becomes its own job while the
#      Python orchestration stays local:
#
#        export SLASH_TOOL_LAUNCHER=$PWD/scripts/lsf/lsf-run.sh
#        slashkit link ...
#
#   2. Whole build. Submit everything as a single job, so one reservation
#      covers the entire flow:
#
#        scripts/lsf/lsf-run.sh slashkit link ...
#
# Arguments are passed as a real argv and shell-quoted into a generated job
# script before submission. Do not hand-roll a "bsub ... vivado ..." string
# instead: bsub flattens its argv into a single command string that the shell
# on the execution host parses again, so anything containing spaces or shell
# metacharacters is silently mangled.
#
# Everything cluster-specific is read from site.conf; see site.conf.example.
# The remaining knobs, all optional:
#
#   SLASH_LSF_JOB_NAME     LSF job name and log file prefix (default: slash-<task>)
#   SLASH_LSF_LOG_DIR      where job scripts and logs go    (default: $PWD/lsf_logs)
#   SLASH_LSF_BSUB_ARGS    extra bsub arguments, split on whitespace
#   SLASH_LSF_INTERACTIVE  1 to use "bsub -I" and stream output live instead of
#                          "bsub -K" writing it to the log files
#   SLASH_LSF_DRY_RUN      1 to write the job script and print the bsub command
#                          without submitting anything

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=site-conf.sh
source "$SCRIPT_DIR/site-conf.sh"

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi

# Whole-build mode leaves SLASH_TOOL_LAUNCHER exported inside the job, so every
# tool the build then runs would try to submit a job of its own -- from a
# compute node, where bsub may not even exist. One reservation is already held
# and the tool environment is already set up, so just run the command.
if [[ -n "${LSB_JOBID:-}" ]]; then
    exec "$@"
fi

slash_site_init "$SCRIPT_DIR"

RUNNER="$SCRIPT_DIR/tool-run.sh"
[[ -x "$RUNNER" ]] || slash_die "node-side runner is missing or not executable: $RUNNER"

QUEUE="$(slash_require QUEUE)"
MEM_MB="$(slash_require MEM_MB)"
CORES="$(slash_require CORES)"
WALLTIME="$(slash_require WALLTIME)"
OSTYPE_SELECT="$(slash_task_value OSTYPE)"
PROFILE="${SLASH_LSF_PROFILE:-}"

JOB_NAME="${SLASH_LSF_JOB_NAME:-slash-$SLASH_TASK}"
LOG_DIR="${SLASH_LSF_LOG_DIR:-$PWD/lsf_logs}"

if ! command -v bsub >/dev/null 2>&1 && [[ -n "$PROFILE" && -r "$PROFILE" ]]; then
    # A site profile legitimately runs commands that fail, so it must not be
    # sourced under "set -e" -- otherwise it kills this script silently.
    set +e
    # shellcheck disable=SC1090
    source "$PROFILE"
    set -e
fi
command -v bsub >/dev/null 2>&1 || slash_die \
    "bsub is not on PATH. Set SLASH_LSF_PROFILE to the profile script that provides it."

mkdir -p "$LOG_DIR"
job_script="$(mktemp "$LOG_DIR/${JOB_NAME}-XXXXXX.sh")"

{
    echo "#!/bin/bash"
    printf 'exec %q' "$RUNNER"
    printf ' %q' "$@"
    echo
} > "$job_script"
chmod +x "$job_script"

bsub_args=(
    -J "$JOB_NAME"
    -q "$QUEUE"
    -R "rusage[mem=$MEM_MB]"
    -n "$CORES"
    -W "$WALLTIME"
)

# Optional, because a homogeneous farm does not need it and an unsatisfiable
# selector leaves the job pending forever with no obvious reason.
[[ -n "$OSTYPE_SELECT" ]] && bsub_args+=(-R "select[ostype==$OSTYPE_SELECT]")

if [[ -n "${SLASH_LSF_BSUB_ARGS:-}" ]]; then
    # Deliberately unquoted: this is a list of extra bsub flags.
    # shellcheck disable=SC2206
    bsub_args+=(${SLASH_LSF_BSUB_ARGS})
fi

if [[ "${SLASH_LSF_INTERACTIVE:-0}" == "1" ]]; then
    # -I streams the job's output to this terminal and blocks. The job dies if
    # the submitting session goes away, so it is opt-in.
    bsub_args+=(-I)
else
    out_log="$LOG_DIR/${JOB_NAME}.%J.out"
    err_log="$LOG_DIR/${JOB_NAME}.%J.err"
    bsub_args+=(-K -oo "$out_log" -eo "$err_log")
    echo "LSF: job logs -> $out_log / $err_log" >&2
fi

echo "LSF: $SLASH_TASK -> queue $QUEUE, ${MEM_MB}MB, ${CORES} slots, ${WALLTIME}min" >&2
echo "LSF: job script -> $job_script" >&2

if [[ "${SLASH_LSF_DRY_RUN:-0}" == "1" ]]; then
    echo "LSF: dry run, not submitting:" >&2
    printf ' %q' bsub "${bsub_args[@]}" "$job_script" >&2
    echo >&2
    exit 0
fi

# -K makes bsub block and exit with the job's own status, which is what the
# launcher contract requires. Report a failure rather than letting "set -e"
# swallow it with no context.
set +e
bsub "${bsub_args[@]}" "$job_script"
status=$?
set -e

if [[ $status -ne 0 ]]; then
    echo "LSF: job $JOB_NAME exited with status $status" >&2
fi
exit $status
