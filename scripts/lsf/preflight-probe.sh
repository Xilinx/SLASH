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
# Runs on the execution host, under tool-run.sh, so the settings script has
# already been sourced by the time this starts. Reports what the node can
# offer and fails if anything the current task needs is absent.
#
# Not meant to be run by hand; use preflight.sh.

TASK="${SLASH_BUILD_TASK:-preflight}"

# What each task actually invokes. The Vivado steps are a single binary; the
# firmware build is the demanding one, because it drives a whole toolchain.
case "$TASK" in
    aved)
        # Two cross-builds, not one: the AMC firmware, and RP1, which
        # build_all.sh invokes and which configures with -G Ninja and derives
        # its BSP with sdtgen.
        required=(bootgen empyro sdtgen ninja cmake make git python3)
        ;;
    hls)
        required=(v++ vitis-run)
        ;;
    bootgen)
        required=(bootgen)
        ;;
    preflight)
        required=()
        ;;
    *)
        required=(vivado)
        ;;
esac

# Everything worth knowing about, whether or not this task needs it.
probed=(vivado v++ vitis-run bootgen empyro sdtgen ninja cmake make git python3)

echo "=== node ==="
echo "host          : $(hostname)"
echo "os            : $(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null)"
echo "cpus          : $(nproc 2>/dev/null)"
echo "memory        : $(sed -n 's/^MemTotal: *//p' /proc/meminfo 2>/dev/null)"
echo "cwd           : $(pwd)"
echo "task          : $TASK"
echo "XILINX_VIVADO : ${XILINX_VIVADO:-<unset>}"
echo "XILINX_VITIS  : ${XILINX_VITIS:-<unset>}"

echo
echo "=== tools ==="
# Absent but irrelevant to this task, reported so that one probe answers the
# question for every task; versus absent and needed, which fails the job.
missing=()
blocking=()
for tool in "${probed[@]}"; do
    path="$(command -v "$tool" 2>/dev/null)"
    if [[ -z "$path" ]]; then
        printf '%-12s : MISSING\n' "$tool"
        missing+=("$tool")
        [[ " ${required[*]} " == *" $tool "* ]] && blocking+=("$tool")
        continue
    fi
    case "$tool" in
        cmake|make|git|python3|ninja) version="$("$tool" --version 2>&1 | head -1)" ;;
        *)                      version="" ;;
    esac
    printf '%-12s : %s %s\n' "$tool" "$path" "$version"
done

# The AVED step cross-compiles for the R5 twice over, using the compilers that
# ship inside Vitis rather than anything installed on the node. The two name
# different binaries -- AMC's CMake toolchain asks for armr5-none-eabi-gcc,
# RP1's for arm-none-eabi-gcc -- so either one missing fails the job, and
# accepting whichever is present would let a node through that cannot finish.
echo
echo "=== cross compilers ==="
missing_cross=()
for cross in armr5-none-eabi-gcc arm-none-eabi-gcc; do
    path="$(command -v "$cross" 2>/dev/null)"
    if [[ -n "$path" ]]; then
        printf '%-22s : %s\n' "$cross" "$path"
    else
        printf '%-22s : MISSING\n' "$cross"
        missing+=("$cross")
        missing_cross+=("$cross")
        [[ " ${required[*]} " == *" empyro "* ]] && blocking+=("$cross")
    fi
done
if [[ ${#missing_cross[@]} -gt 0 ]]; then
    echo "  Vitis ships both, in separate directories: armr5 under"
    echo "  gnu/armr5/lin/gcc-arm-none-eabi/bin and arm under"
    echo "  gnu/aarch32/lin/gcc-arm-none-eabi/bin. If they are not on PATH"
    echo "  here, SLASH_LSF_SETTINGS is probably pointing at Vivado rather"
    echo "  than Vitis for this task."
fi

# For RP1's generator, presence is the wrong question: RHEL 8 ships 3.6.8 as
# python3, which predates the `from __future__ import annotations` the script
# opens with. Reporting the tool as found is how a node passed this probe and
# then failed two hours into the job. Resolve exactly as build-rp1.sh does, so
# that passing here means that script will find an interpreter too.
echo
echo "=== python for RP1 ==="
rp1_python=""
for candidate in "${XILINX_VITIS:-}"/tps/lnx64/python-*/bin/python3 \
                 "$(command -v python3 2>/dev/null)"; do
    [[ -x $candidate ]] || continue
    # Vitis's interpreter needs its sibling lib/ on LD_LIBRARY_PATH, having no
    # RPATH to its own libpython. build-rp1.sh does the same two-step, so a
    # candidate accepted here is one that script will accept too.
    for libdir in "" "${candidate%/bin/*}/lib"; do
        if [[ -n $libdir ]]; then
            version="$(LD_LIBRARY_PATH="$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                "$candidate" -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])' 2>/dev/null)"
        else
            version="$("$candidate" -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])' 2>/dev/null)"
        fi
        [[ -n $version ]] || continue
        case "$version" in
            3.[0-6].*|2.*) printf '%-22s : %s (%s) TOO OLD\n' "rejected" "$candidate" "$version" ;;
            *) printf '%-22s : %s (%s)%s\n' "python >= 3.7" "$candidate" "$version" \
                   "${libdir:+ [needs LD_LIBRARY_PATH]}"
               rp1_python="$candidate" ;;
        esac
        break
    done
    [[ -n $rp1_python ]] && break
done
if [[ -z "$rp1_python" ]]; then
    printf '%-22s : MISSING\n' "python >= 3.7"
    missing+=("python>=3.7")
    [[ " ${required[*]} " == *" empyro "* ]] && blocking+=("python>=3.7")
fi

# Nothing SLASH-specific is installed here, so every input has to be reachable
# over shared storage. A node that cannot see the build directory is the single
# most common way this setup fails, and it fails deep inside a tool.
echo
echo "=== shared storage ==="
for dir in "$(pwd)" "${SLASH_TOOL_CWD:-}" "${HOME:-}"; do
    [[ -z "$dir" ]] && continue
    if [[ -r "$dir" ]]; then
        printf 'readable  : %s\n' "$dir"
    else
        # Always fatal, whatever the task: no shared storage, no build.
        printf 'UNREADABLE: %s\n' "$dir"
        blocking+=("$dir")
    fi
done
if touch ./.slash-preflight-write-check 2>/dev/null; then
    rm -f ./.slash-preflight-write-check
    echo "writable  : $(pwd)"
else
    echo "read-only : $(pwd)"
fi

echo
if [[ ${#blocking[@]} -gt 0 ]]; then
    echo "preflight: FAILED for task '$TASK', missing: ${blocking[*]}"
    exit 1
fi
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "preflight: OK for task '$TASK' (absent, but unused here: ${missing[*]})"
else
    echo "preflight: OK for task '$TASK'"
fi
exit 0
