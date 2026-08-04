#!/bin/bash

set -euo pipefail
shopt -s nullglob

usage() {
    cat <<EOF
Usage: ${0} [options]

Clone and test a SLASH branch on a V80 host using repo-built artifacts. Many
options are mandatory.

Mandatory options:
  --repo-url URL        Git repository URL to clone (or REPO_URL)
  --branch BRANCH      Branch to test (or BRANCH)
  --scratch DIR        Clone destination / working directory (or SCRATCH)
  --bdf BDF            Board BDF prefix, e.g. 0000:65:00 (or BDF)
  --xsdb-target-id XSDB_ID
                       ID of the device in xsdb for jtag reprogramming
                       (or XSDB_ID)
  --smbus-ip ZIP       SMBus IP zip file to extract into the root-design iprepo
                       before pbuild (or SMBUS_IP)

Other options:
  --protected-user USER
                       Unprivileged user that owns the checkout/build
                       (or PROTECTED_USER; default: \$SUDO_USER)
  --scratch-vrtd DIR   Directory for direct-run vrtd config/socket/logs
                       (or SCRATCH_VRTD; default: SCRATCH/vrtd)
  --source FILE        Source an environment setup script before building;
                       useful for Vivado/Vitis settings scripts.
  -h, --help           Show this help
EOF
}

require_var() {
    local name="${1}"
    local value="${2}"
    if [[ -z "${value}" ]]; then
        echo "ERROR: missing required option/env: ${name}" >&2
        usage >&2
        exit 1
    fi
}

REPO_URL="${REPO_URL:-}"
BRANCH="${BRANCH:-}"
SCRATCH="${SCRATCH:-}"
BDF="${BDF:-}"
XSDB_ID="${XSDB_ID:-}"
SMBUS_IP="${SMBUS_IP:-}"
PROTECTED_USER="${PROTECTED_USER:-${SUDO_USER:-}}"
SCRATCH_VRTD="${SCRATCH_VRTD:-}"

while [[ ${#} -gt 0 ]]; do
    case "${1}" in
        --repo-url)
            [[ ${#} -ge 2 ]] || { echo "ERROR: --repo-url requires a value" >&2; exit 1; }
            REPO_URL="${2}"
            shift 2
            ;;
        --branch)
            [[ ${#} -ge 2 ]] || { echo "ERROR: --branch requires a value" >&2; exit 1; }
            BRANCH="${2}"
            shift 2
            ;;
        --scratch)
            [[ ${#} -ge 2 ]] || { echo "ERROR: --scratch requires a value" >&2; exit 1; }
            SCRATCH="${2}"
            shift 2
            ;;
        --bdf)
            [[ ${#} -ge 2 ]] || { echo "ERROR: --bdf requires a value" >&2; exit 1; }
            BDF="${2}"
            shift 2
            ;;
        --xsdb-target-id)
            [[ ${#} -ge 2 ]] || { echo "ERROR: --xsdb-target-id requires a value" >&2; exit 1; }
            XSDB_ID="${2}"
            shift 2
            ;;
        --smbus-ip)
            [[ ${#} -ge 2 ]] || { echo "ERROR: --smbus-ip requires a value" >&2; exit 1; }
            SMBUS_IP="${2}"
            shift 2
            ;;
        --protected-user)
            [[ ${#} -ge 2 ]] || { echo "ERROR: --protected-user requires a value" >&2; exit 1; }
            PROTECTED_USER="${2}"
            shift 2
            ;;
        --scratch-vrtd)
            [[ ${#} -ge 2 ]] || { echo "ERROR: --scratch-vrtd requires a value" >&2; exit 1; }
            SCRATCH_VRTD="${2}"
            shift 2
            ;;
        --source)
            [[ ${#} -ge 2 ]] || { echo "ERROR: --source requires a value" >&2; exit 1; }
            set +u
            source "${2}"
            set -u
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: ${1}" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ "${EUID}" -ne 0 ]]; then
    echo "ERROR: branch-test.sh must be run as root (use sudo)" >&2
    exit 1
fi

require_var REPO_URL "${REPO_URL}"
require_var BRANCH "${BRANCH}"
require_var SCRATCH "${SCRATCH}"
require_var BDF "${BDF}"
require_var XSDB_ID "${XSDB_ID}"
require_var SMBUS_IP "${SMBUS_IP}"
require_var PROTECTED_USER "${PROTECTED_USER}"

if [[ -z "${SCRATCH_VRTD}" ]]; then
    SCRATCH_VRTD="${SCRATCH}/vrtd"
fi

if [[ ! -f "${SMBUS_IP}" ]]; then
    echo "ERROR: SMBus IP zip file not found: ${SMBUS_IP}" >&2
    exit 1
fi
SMBUS_IP="$(realpath "${SMBUS_IP}")"

set -x

# Helpers
function remove_bdf() {
    if [[ -e "/sys/bus/pci/devices/${1}" ]]; then
        echo 1 | tee "/sys/bus/pci/devices/${1}/remove"
    fi
}

function remove_board() {
    remove_bdf "${1}.0"
    remove_bdf "${1}.1"
    remove_bdf "${1}.2"
}

function protected() {
    sudo --user="${PROTECTED_USER}" --set-home --preserve-env \
        env \
        PATH="${PATH}" \
        LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
        PYTHONPATH="${PYTHONPATH:-}" \
        XILINX_VIVADO="${XILINX_VIVADO:-}" \
        XILINX_VITIS="${XILINX_VITIS:-}" \
        XILINX_HLS="${XILINX_HLS:-}" \
        XILINX_XRT="${XILINX_XRT:-}" \
        XILINXD_LICENSE_FILE="${XILINXD_LICENSE_FILE:-}" \
        LM_LICENSE_FILE="${LM_LICENSE_FILE:-}" \
        "${@}"
}

VRTD_PID=""
RESTORE_SYSTEM_RUNTIME=0

function restore_system_runtime() {
    local rc="${?}"
    trap - EXIT
    set +e

    if [[ -v OLD_PYTHONPATH ]]; then
        export PYTHONPATH="${OLD_PYTHONPATH}"
    fi

    if [[ -v VRTD_PID ]]; then
        kill "${VRTD_PID}" 2>/dev/null
        wait "${VRTD_PID}" 2>/dev/null
    fi

    if [[ "${RESTORE_SYSTEM_RUNTIME}" -eq 1 ]]; then
        remove_board "${BDF}"

        rmmod slash
        rmmod ami

        modprobe ami
        modprobe slash

        udevadm settle # Let udev create slash_hotplug

        systemctl restart vrtd.socket
        systemctl restart vrtd.service
        sleep 5

        v80-smi write-static-shell --jtag --no-remove-device --xsdb-target-id "${XSDB_ID}"

        v80-smi reset -d "${BDF}"
    fi

    exit "${rc}"
}

trap restore_system_runtime EXIT



protected git clone --depth 1 --branch "${BRANCH}" --single-branch "${REPO_URL}" "${SCRATCH}"
pushd "${SCRATCH}"

# Let repo-built tools resolve slashkit resources from this checkout.
OLD_PYTHONPATH="${PYTHONPATH:-}"
export PYTHONPATH="${PWD}/linker${PYTHONPATH:+:${PYTHONPATH}}"

protected git submodule update --init --recursive

# Build phase

protected python3 -m zipfile -e "${SMBUS_IP}" linker/slashkit/resources/base/iprepo
if ! compgen -G 'linker/slashkit/resources/base/iprepo/smbus*/' >/dev/null; then
    echo "ERROR: extracted SMBus IP zip did not create linker/slashkit/resources/base/iprepo/smbus*/" >&2
    exit 1
fi

protected scripts/pconfigure.sh
protected scripts/pbuild.sh

# SLASH driver
protected make -C driver
protected make -C driver/tests all # kernel-module kselftest binaries (userspace)

# AMI driver
protected make -C submodules/AVED/sw/AMI/driver

# Run phase: serialize against other concurrent branch-test runs on this
# shared host. The build above is CPU-only and may run in parallel, but
# everything below mutates shared state (kernel modules, PCIe functions,
# vrtd, the board) and must not race. This is a cooperative advisory lock
# (flock): only branch-test runs honor it. Held on fd 200 until the script
# exits so the restore_system_runtime cleanup is covered too. Override the
# path with BRANCH_TEST_LOCK if a narrower/wider scope is ever needed.
exec 200>"${BRANCH_TEST_LOCK:-/run/lock/slash-branch-test.lock}"
flock 200

# Install phase

systemctl stop vrtd.socket vrtd.service || true
RESTORE_SYSTEM_RUNTIME=1

remove_board "${BDF}"
rmmod slash || true
rmmod ami || true

insmod submodules/AVED/sw/AMI/driver/ami.ko
insmod driver/slash.ko

udevadm settle # Let udev create slash_hotplug

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
VRTD_LOG="${SCRATCH_VRTD}/vrtd.log" \
    pbuild/smi/vrt/vrtd/src/vrtd &

VRTD_PID="${!}"

V80_SMI=(
    protected

    env
    VRTD_SOCKET="${SCRATCH_VRTD}/vrtd.sock"
    SMI_VERSAL_FLASH_TCL="${PWD}/smi/resources/versal_flash_pdi.tcl"

    pbuild/smi/src/v80-smi
)

# Wait for vrtd startup

sleep 5

# Static shell load phase

# Flash the new image

"${V80_SMI[@]}" write-static-shell --jtag --no-remove-device --xsdb-target-id "${XSDB_ID}"

# Run tests phase

# Tests board exists and sesnors exist
JQ_READY='any(.boards[]; .bdf_base == $bdf and .status == "OK" and ((.sensors? // []) | length > 0))'
"${V80_SMI[@]}" list --json --sensors |
    jq --exit-status --arg bdf "${BDF}" "${JQ_READY}" >/dev/null

# Test examples
protected scripts/test-examples.sh --use-repo emu "${BDF}"
protected scripts/test-examples.sh --use-repo sim "${BDF}"
protected env VRTD_SOCKET="${SCRATCH_VRTD}/vrtd.sock" scripts/test-examples.sh --use-repo hw "${BDF}"
protected env VRTD_SOCKET="${SCRATCH_VRTD}/vrtd.sock" scripts/stress-test.sh "${BDF}" --use-pbuild --no-reset

# kselftest

# For jtag, restart is reflash
"${V80_SMI[@]}" write-static-shell --jtag --xsdb-target-id "${XSDB_ID}" --device "${BDF}"

# Test again for existence
"${V80_SMI[@]}" list --json --sensors |
    jq --exit-status --arg bdf "${BDF}" "${JQ_READY}" >/dev/null

kill "${VRTD_PID}"
wait "${VRTD_PID}"
unset VRTD_PID

# kselftest tests are currently built around only one device existing in the system

SLASH_CTL_DEVICES=(/dev/slash_ctl*)
SLASH_QDMA_CTL_DEVICES=(/dev/slash_qdma_ctl*)

if [[ "${#SLASH_CTL_DEVICES[@]}" -ne 1 || "${#SLASH_QDMA_CTL_DEVICES[@]}" -ne 1 ]]; then
    echo "WARN: not one SLASH device, skipping kselftest"
else
    # Cannot run destructive tests as those include SBR reset, which will reset to flash.
    env -u SLASH_TEST_DESTRUCTIVE make -C driver/tests run
fi
