# shellcheck shell=bash

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

# Shared knowledge about the set of packages SLASH installs. Source this; it is not
# executable and has no side effects beyond defining the variables and functions below.
#
#   source "$(dirname "$0")/slash-packages.sh"
#   slash_detect_pkg_type          # sets SLASH_PKG_TYPE and SLASH_PKG_DISTRO
#   slash_installed_packages       # prints the installed subset, one per line
#
# The package names must match packaging/debian/control and packaging/rpm/slash.spec.
# "ami" comes from submodules/AVED and is built by scripts/package-ami.sh, but it is
# installed and removed alongside the rest, so it belongs in these lists.

# Debian binary package names.
DEB_PACKAGES=(
    slash
    slash-dev
    slash-sim-emu
    slash-sim-emu-dev
    slash-dkms
    libslash
    libslash-dev
    vrtd
    libvrtd
    libvrtd-dev
    libvrt
    libvrt-dev
    v80-smi
    slashkit
    ami
)

# RPM package names. Same set; RPM spells the development packages "-devel".
RPM_PACKAGES=(
    slash
    slash-devel
    slash-sim-emu
    slash-sim-emu-devel
    slash-dkms
    libslash
    libslash-devel
    vrtd
    libvrtd
    libvrtd-devel
    libvrt
    libvrt-devel
    v80-smi
    slashkit
    ami
)

# Determine whether this machine uses .deb or .rpm packages.
# Sets SLASH_PKG_TYPE ("deb" or "rpm") and SLASH_PKG_DISTRO (a human-readable name).
# Returns non-zero on an unsupported distribution rather than exiting, so callers
# decide whether that is fatal.
slash_detect_pkg_type() {
    if [[ ! -f /etc/os-release ]]; then
        echo "ERROR: /etc/os-release not found. Cannot detect distribution." >&2
        return 1
    fi

    # Subshell-free sourcing would leak ID/ID_LIKE/PRETTY_NAME into the caller, so
    # read the three fields we need out of a subshell instead.
    local id id_like pretty
    id="$(. /etc/os-release && echo "${ID:-}")"
    id_like="$(. /etc/os-release && echo "${ID_LIKE:-}")"
    pretty="$(. /etc/os-release && echo "${PRETTY_NAME:-${ID:-unknown}}")"

    SLASH_PKG_DISTRO="${pretty}"

    case "${id_like:-$id}" in
        *debian*|*ubuntu*)
            SLASH_PKG_TYPE="deb"
            return 0
            ;;
        *rhel*|*fedora*|*centos*|*suse*)
            SLASH_PKG_TYPE="rpm"
            return 0
            ;;
    esac

    # Fallback: check ID directly, for distributions that set no ID_LIKE.
    case "${id}" in
        debian|ubuntu|linuxmint|pop)
            SLASH_PKG_TYPE="deb"
            ;;
        rhel|fedora|centos|rocky|alma|ol|sles|opensuse*)
            SLASH_PKG_TYPE="rpm"
            ;;
        *)
            echo "ERROR: Unsupported distribution: ${id} (ID_LIKE=${id_like:-unset})" >&2
            return 1
            ;;
    esac
}

# Print the SLASH packages that are currently installed, one per line.
# Requires slash_detect_pkg_type to have run first. Prints nothing when none are.
slash_installed_packages() {
    local pkg
    if [[ "${SLASH_PKG_TYPE:-}" == "deb" ]]; then
        for pkg in "${DEB_PACKAGES[@]}"; do
            if dpkg-query -W -f='${db:Status-Status}\n' "${pkg}" 2>/dev/null \
                | grep -qx 'installed'; then
                echo "${pkg}"
            fi
        done
    elif [[ "${SLASH_PKG_TYPE:-}" == "rpm" ]]; then
        for pkg in "${RPM_PACKAGES[@]}"; do
            if rpm -q "${pkg}" &>/dev/null; then
                echo "${pkg}"
            fi
        done
    else
        echo "ERROR: slash_detect_pkg_type has not been called." >&2
        return 1
    fi
}
