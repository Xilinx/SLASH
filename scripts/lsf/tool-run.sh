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
# Node-side half of the LSF example launcher.
#
# LSF runs a submitted command as a plain, non-interactive bash script on the
# execution host, so the tool environment has to be established there. This
# script does that and then execs the command line it was handed -- the full
# vendor tool argv, exactly as slashkit assembled it.
#
# Nothing SLASH-specific is installed or needed here. Every input the tool
# touches was staged onto shared storage by the submitting process, so a node
# only needs the vendor toolchain and a mount of that storage.
#
# Environment, both set by slashkit:
#   SLASH_BUILD_TASK  which step this is; selects the sizing and settings script
#   SLASH_TOOL_CWD    directory to run in

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=site-conf.sh
source "$SCRIPT_DIR/site-conf.sh"

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi

slash_site_init "$SCRIPT_DIR"
SETTINGS="$(slash_require SETTINGS)"

# Several tools resolve their inputs against the process working directory --
# bootgen against the paths inside a .bif, v++ against "--work_dir ." -- and a
# scheduler only reproduces the submission directory as a courtesy. Some fall
# back to a temporary directory when the path does not exist on the node, which
# turns a missing mount into a baffling "file not found" from the tool.
if [[ -n "${SLASH_TOOL_CWD:-}" ]]; then
    cd "$SLASH_TOOL_CWD" || slash_die \
        "SLASH_TOOL_CWD is not reachable from $(hostname): $SLASH_TOOL_CWD
Is it on storage that is shared with the submit host?"
fi

# Source the bash flavour. settings64.csh is csh syntax and cannot be used
# here: LSF runs the job under the submitting user's login shell, and under
# bash it dies with "setenv: command not found" followed by a syntax error on
# the first if/else. A missing settings script is fatal rather than a warning,
# because the alternative is picking up whatever tool the copied submission
# environment happened to point at and reporting success from the wrong build.
[[ -r "$SETTINGS" ]] || slash_die \
    "tool settings script is not readable on $(hostname): $SETTINGS"
# shellcheck disable=SC1090
source "$SETTINGS"

# LD_PRELOAD is set by slashkit to work around a Vivado-in-container issue on
# the submit host, and LSF copies it here, where those library paths do not
# exist. The dynamic loader then complains on every single process the tool
# spawns. Keep only the entries that are actually present on this node.
if [[ -n "${LD_PRELOAD:-}" ]]; then
    LD_PRELOAD="$(slash_prune_pathlist "$LD_PRELOAD")"
    if [[ -z "$LD_PRELOAD" ]]; then
        unset LD_PRELOAD
    else
        export LD_PRELOAD
    fi
fi

echo "=== SLASH cluster job: ${SLASH_TASK} ==="
echo "host    : $(hostname)"
echo "os      : $(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null)"
echo "cwd     : $(pwd)"
echo "settings: $SETTINGS"
echo "command : $*"
echo "========================================"

exec "$@"
