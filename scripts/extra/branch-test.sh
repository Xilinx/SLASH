#!/bin/bash

set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 --repo-url URL --branch BRANCH --scratch DIR --bdf BDF [options]

Clone and test a SLASH branch on a V80 host using repo-built artifacts.

Options:
  --repo-url URL        Git repository URL to clone (or REPO_URL)
  --branch BRANCH      Branch to test (or BRANCH)
  --scratch DIR        Clone destination / working directory (or SCRATCH)
  --bdf BDF            Board BDF prefix, e.g. 0000:65:00 (or BDF)
  --protected-user USER
                       Unprivileged user that owns the checkout/build (or PROTECTED_USER)
  --scratch-vrtd DIR   Directory for direct-run vrtd config/socket/logs
                       (or SCRATCH_VRTD; default: SCRATCH/vrtd)
  -h, --help           Show this help
EOF
}

require_var() {
    local name="$1"
    local value="$2"
    if [[ -z "$value" ]]; then
        echo "ERROR: missing required option/env: $name" >&2
        usage >&2
        exit 1
    fi
}

REPO_URL="${REPO_URL:-}"
BRANCH="${BRANCH:-}"
SCRATCH="${SCRATCH:-}"
BDF="${BDF:-}"
PROTECTED_USER="${PROTECTED_USER:-${SUDO_USER:-}}"
SCRATCH_VRTD="${SCRATCH_VRTD:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo-url)
            [[ $# -ge 2 ]] || { echo "ERROR: --repo-url requires a value" >&2; exit 1; }
            REPO_URL="$2"
            shift 2
            ;;
        --branch)
            [[ $# -ge 2 ]] || { echo "ERROR: --branch requires a value" >&2; exit 1; }
            BRANCH="$2"
            shift 2
            ;;
        --scratch)
            [[ $# -ge 2 ]] || { echo "ERROR: --scratch requires a value" >&2; exit 1; }
            SCRATCH="$2"
            shift 2
            ;;
        --bdf)
            [[ $# -ge 2 ]] || { echo "ERROR: --bdf requires a value" >&2; exit 1; }
            BDF="$2"
            shift 2
            ;;
        --protected-user)
            [[ $# -ge 2 ]] || { echo "ERROR: --protected-user requires a value" >&2; exit 1; }
            PROTECTED_USER="$2"
            shift 2
            ;;
        --scratch-vrtd)
            [[ $# -ge 2 ]] || { echo "ERROR: --scratch-vrtd requires a value" >&2; exit 1; }
            SCRATCH_VRTD="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

require_var REPO_URL "$REPO_URL"
require_var BRANCH "$BRANCH"
require_var SCRATCH "$SCRATCH"
require_var BDF "$BDF"
require_var PROTECTED_USER "$PROTECTED_USER"

if [[ -z "$SCRATCH_VRTD" ]]; then
    SCRATCH_VRTD="${SCRATCH}/vrtd"
fi

set -x

function protected() {
    sudo --user="${PROTECTED_USER}" "$@"
}

protected git clone --depth 1 --branch "${BRANCH}" --single-branch "${REPO_URL}" "${SCRATCH}"
pushd "${SCRATCH}"
protected git submodule update --init --recursive

# Build phase

protected scripts/pconfigure.sh
protected scripts/pbuild.sh

PDI_PATH="linker/slashkit/resources/static_shell/amd_v80_gen5x8_25.1.pdi"
if [[ ! -f "$PDI_PATH" ]]; then
    echo "ERROR: expected static shell PDI from pbuild not found: $PDI_PATH" >&2
    exit 1
fi

pushd driver
protected make
popd

pushd submodules/AVED/sw/AMI/driver
protected make
popd

# Install phase

echo 1 | tee "/sys/bus/pci/devices/${BDF}.0/remove" || true
echo 1 | tee "/sys/bus/pci/devices/${BDF}.1/remove" || true
echo 1 | tee "/sys/bus/pci/devices/${BDF}.2/remove" || true
rmmod slash || true
rmmod ami || true

# Flash phase

PDI_PATH="${PDI_PATH}" xsdb scripts/extra/versal_flash_pdi.tcl

insmod submodules/AVED/sw/AMI/driver/ami.ko
insmod driver/slash.ko

echo 1 | tee /sys/bus/pci/rescan

# Launch vrtd phase

protected mkdir -p "${SCRATCH_VRTD}"
protected cp vrt/vrtd/conf/vrtd.conf "${SCRATCH_VRTD}/vrtd.conf"
{
    echo ""
    echo "[user:${PROTECTED_USER}]"
    echo "role = fullaccess"
} | protected tee -a "${SCRATCH_VRTD}/vrtd.conf"

VRTD_CONFIG="${SCRATCH_VRTD}/vrtd.conf" \
VRTD_SOCKET="${SCRATCH_VRTD}/vrtd.sock" \
VRTD_LOG_STDERR=1 \
pbuild/smi/vrt/vrtd/src/vrtd >"${SCRATCH_VRTD}/vrtd.log" 2>&1 &

VRTD_PID=$!
cleanup() {
    kill "${VRTD_PID}" 2>/dev/null || true
    wait "${VRTD_PID}" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for vrtd startup

sleep 5

# Run tests phase

protected env VRTD_SOCKET="${SCRATCH_VRTD}/vrtd.sock" pbuild/smi/src/v80-smi list --sensors # Test ami/sensors

protected scripts/test-examples.sh --use-repo emu "${BDF}"
protected scripts/test-examples.sh --use-repo sim "${BDF}"
protected env VRTD_SOCKET="${SCRATCH_VRTD}/vrtd.sock" scripts/test-examples.sh --use-repo hw "${BDF}"
protected env VRTD_SOCKET="${SCRATCH_VRTD}/vrtd.sock" scripts/stress-test.sh --use-pbuild "${BDF}" --no-reset
