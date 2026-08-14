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
# Check that a compute node can actually run a SLASH build, in one short job,
# before committing hours of queue time to finding out the hard way.
#
#   scripts/lsf/preflight.sh              # the Vivado steps
#   scripts/lsf/preflight.sh aved         # the firmware build, which needs the most
#
# The optional argument is a task kind, so the probe runs with exactly the
# settings script and reservation that task would get. Probing an expensive
# task cheaply is a matter of overriding the sizing for the one run, since an
# exported value beats the per-task configuration:
#
#   SLASH_LSF_QUEUE=<short-queue> SLASH_LSF_MEM_MB=1000 \
#       scripts/lsf/preflight.sh aved
#
# Output goes to the job log under $SLASH_LSF_LOG_DIR, or straight to the
# terminal with SLASH_LSF_INTERACTIVE=1.

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export SLASH_BUILD_TASK="${1:-preflight}"
# The probe reports on the directory it lands in, so aim it at the checkout:
# if that is not visible from the node, nothing else is going to work either.
export SLASH_TOOL_CWD="${SLASH_TOOL_CWD:-$SCRIPT_DIR}"
export SLASH_LSF_JOB_NAME="${SLASH_LSF_JOB_NAME:-slash-preflight}"

exec "$SCRIPT_DIR/lsf-run.sh" "$SCRIPT_DIR/preflight-probe.sh"
