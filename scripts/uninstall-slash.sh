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

# Bring down SLASH on this machine and remove its packages, leaving a clean slate for a
# fresh build and install.
#
# The order matters and is the reverse of installation:
#
#   1. stop vrtd (socket first, or socket activation restarts the service)
#   2. detach the board from the PCIe bus, if a BDF was given
#   3. unload the slash and ami kernel modules
#   4. purge the SLASH packages
#
# Doing 4 before 3 would drop the module files while a device is still bound to them.
#
# Every step is a no-op when the thing it acts on is absent, so running this twice, or on
# a machine that never had SLASH, is harmless.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/slash-packages.sh
source "${SCRIPT_DIR}/slash-packages.sh"

DEVICES=()
DRY_RUN=0
ASSUME_YES=0
DO_RUNTIME=1
DO_PACKAGES=1

usage() {
    cat <<'EOF'
Usage: sudo scripts/uninstall-slash.sh [options]

Stops SLASH services, unloads its kernel modules and removes its packages.

Options:
  -d, --device <BDF>   V80 board to detach from the PCIe bus, as printed by lspci,
                       e.g. 0000:21:00. May be given more than once. When no --device
                       is given this step is skipped; see the note printed at runtime.
      --keep-packages  Stop services and unload modules, but leave the packages
                       installed.
      --packages-only  Remove the packages only. Assumes nothing is using them.
  -n, --dry-run        Print the commands that would run, and change nothing.
  -y, --yes            Do not ask for confirmation.
  -h, --help           Show this message.

Examples:
  sudo scripts/uninstall-slash.sh -d 0000:21:00
  sudo scripts/uninstall-slash.sh --dry-run
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--device)
            if [[ $# -lt 2 ]]; then
                echo "ERROR: $1 requires a BDF argument." >&2
                exit 1
            fi
            DEVICES+=("$2")
            shift 2
            ;;
        --keep-packages) DO_PACKAGES=0; shift ;;
        --packages-only) DO_RUNTIME=0; shift ;;
        -n|--dry-run)    DRY_RUN=1; shift ;;
        -y|--yes)        ASSUME_YES=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)
            echo "ERROR: Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ "${DO_RUNTIME}" -eq 0 && "${DO_PACKAGES}" -eq 0 ]]; then
    echo "ERROR: --keep-packages and --packages-only leave nothing to do." >&2
    exit 1
fi

# Normalise a user-supplied BDF to the domain-qualified, function-less form that the
# sysfs paths below are built from. Accepts 21:00, 21:00.0, 0000:21:00 and 0000:21:00.0.
normalise_bdf() {
    local bdf="$1"
    bdf="${bdf%.*}"                          # drop any function suffix
    if [[ "${bdf}" != *:*:* ]]; then
        bdf="0000:${bdf}"                    # default to domain 0
    fi
    if [[ ! "${bdf}" =~ ^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}$ ]]; then
        echo "ERROR: '$1' is not a valid PCI address. Expected something like 0000:21:00." >&2
        return 1
    fi
    echo "${bdf,,}"
}

NORMALISED=()
for _dev in "${DEVICES[@]+"${DEVICES[@]}"}"; do
    _n="$(normalise_bdf "${_dev}")" || exit 1
    NORMALISED+=("${_n}")
done

# Run a command, or just show it under --dry-run.
run() {
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        echo "  [dry-run] $*"
        return 0
    fi
    echo "  + $*"
    "$@"
}

# As above, for the one thing that needs a shell redirection.
run_write() {
    local value="$1" path="$2"
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        echo "  [dry-run] echo ${value} > ${path}"
        return 0
    fi
    echo "  + echo ${value} > ${path}"
    echo "${value}" > "${path}"
}

if [[ "${DRY_RUN}" -eq 0 && "${EUID}" -ne 0 ]]; then
    echo "ERROR: This script must be run as root. Try: sudo $0 $*" >&2
    exit 1
fi

if ! slash_detect_pkg_type; then
    if [[ "${DO_PACKAGES}" -eq 1 ]]; then
        exit 1
    fi
    echo "WARNING: unknown distribution; continuing because packages are not being touched." >&2
fi

# -------------------------------------------------------------------------------------
#  Confirm
# -------------------------------------------------------------------------------------

echo "SLASH cleanup"
echo "  distribution:  ${SLASH_PKG_DISTRO:-unknown} (${SLASH_PKG_TYPE:-n/a} packages)"
if [[ "${DO_RUNTIME}" -eq 1 ]]; then
    echo "  services:      vrtd.socket, vrtd.service will be stopped"
    if [[ ${#NORMALISED[@]} -gt 0 ]]; then
        echo "  PCIe devices:  ${NORMALISED[*]} will be detached from the bus"
    else
        echo "  PCIe devices:  none given, skipping (pass --device to detach a board)"
    fi
    echo "  modules:       slash, ami will be unloaded"
fi
if [[ "${DO_PACKAGES}" -eq 1 ]]; then
    echo "  packages:      installed SLASH packages will be removed"
fi
echo ""

if [[ "${DRY_RUN}" -eq 0 && "${ASSUME_YES}" -eq 0 && -t 0 ]]; then
    read -r -p "Proceed? [y/N] " _answer </dev/tty
    case "${_answer}" in
        [yY]|[yY][eE][sS]) ;;
        *) echo "Aborted." >&2; exit 1 ;;
    esac
    echo ""
fi

# -------------------------------------------------------------------------------------
#  1. Services
# -------------------------------------------------------------------------------------

if [[ "${DO_RUNTIME}" -eq 1 ]]; then
    echo "Stopping services"
    if command -v systemctl >/dev/null 2>&1; then
        # The socket has to go first: while it is listening, systemd restarts
        # vrtd.service the moment anything connects to /run/vrtd.sock.
        run systemctl stop vrtd.socket || true
        run systemctl stop vrtd.service || true
        run systemctl disable vrtd.socket 2>/dev/null || true
    else
        echo "  systemctl not present, nothing to stop"
    fi
    echo ""
fi

# -------------------------------------------------------------------------------------
#  2. PCIe devices
# -------------------------------------------------------------------------------------

if [[ "${DO_RUNTIME}" -eq 1 ]]; then
    echo "Detaching PCIe devices"
    if [[ ${#NORMALISED[@]} -eq 0 ]]; then
        echo "  No --device given, skipping."
        echo "  To detach a board, find its address with"
        echo "    lspci -D -d 10ee:"
        echo "  and re-run with --device <BDF>, e.g. --device 0000:21:00."
    else
        for bdf in "${NORMALISED[@]}"; do
            # A V80 presents three functions. Removing them individually through sysfs
            # rather than via `v80-smi debug hotplug-op` is deliberate: that command
            # talks to vrtd, which step 1 has just stopped.
            for fn in 0 1 2; do
                dev="/sys/bus/pci/devices/${bdf}.${fn}"
                if [[ -e "${dev}/remove" ]]; then
                    run_write 1 "${dev}/remove"
                else
                    echo "  ${bdf}.${fn} not present, skipping"
                fi
            done
        done
    fi
    echo ""
fi

# -------------------------------------------------------------------------------------
#  3. Kernel modules
# -------------------------------------------------------------------------------------

module_loaded() {
    # Anchor the match: a bare `grep slash` also matches the slash_qdma row, and a bare
    # `grep ami` matches anything with "ami" in its name.
    lsmod 2>/dev/null | grep -q "^$1 "
}

if [[ "${DO_RUNTIME}" -eq 1 ]]; then
    echo "Unloading kernel modules"
    for mod in slash ami; do
        if module_loaded "${mod}"; then
            run rmmod "${mod}" || echo "  WARNING: could not unload ${mod}" >&2
        else
            echo "  ${mod} not loaded, skipping"
        fi
    done
    echo ""
fi

# -------------------------------------------------------------------------------------
#  4. Packages
# -------------------------------------------------------------------------------------

if [[ "${DO_PACKAGES}" -eq 1 ]]; then
    echo "Removing packages"
    mapfile -t INSTALLED < <(slash_installed_packages)

    if [[ ${#INSTALLED[@]} -eq 0 ]]; then
        echo "  No SLASH packages installed."
    else
        echo "  Found: ${INSTALLED[*]}"
        if [[ "${SLASH_PKG_TYPE}" == "deb" ]]; then
            run apt-get purge -y "${INSTALLED[@]}"
            run apt-get autoremove --purge -y
        else
            run dnf remove -y --setopt='*.skip_if_unavailable=True' "${INSTALLED[@]}"
        fi
    fi
    echo ""
fi

# -------------------------------------------------------------------------------------
#  Summary
# -------------------------------------------------------------------------------------

if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "Dry run complete. Nothing was changed."
    exit 0
fi

STATUS=0

if [[ "${DO_RUNTIME}" -eq 1 ]]; then
    for mod in slash ami; do
        if module_loaded "${mod}"; then
            echo "WARNING: ${mod} is still loaded. Something still holds a reference to it" >&2
            echo "         — check for processes with /dev/slash_ctl* or /dev/ami* open." >&2
            STATUS=1
        fi
    done
fi

if [[ "${DO_PACKAGES}" -eq 1 ]]; then
    mapfile -t REMAINING < <(slash_installed_packages)
    if [[ ${#REMAINING[@]} -gt 0 ]]; then
        echo "WARNING: these packages are still installed: ${REMAINING[*]}" >&2
        STATUS=1
    fi
fi

if [[ "${STATUS}" -eq 0 ]]; then
    echo "SLASH cleanup complete."
else
    echo "SLASH cleanup finished with warnings."
fi

if [[ "${DO_RUNTIME}" -eq 1 && ${#NORMALISED[@]} -gt 0 ]]; then
    echo ""
    echo "The board is off the PCIe bus until it is rescanned or the machine is rebooted:"
    echo "  echo 1 | sudo tee /sys/bus/pci/rescan"
fi

exit "${STATUS}"
