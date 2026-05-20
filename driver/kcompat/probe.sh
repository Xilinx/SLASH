#!/bin/sh
#/**
# * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
# * This program is free software; you can redistribute it and/or modify it under the terms of the
# * GNU General Public License as published by the Free Software Foundation; version 2.
# *
# * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
# * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# * General Public License for more details.
# *
# * You should have received a copy of the GNU General Public License along with this program; if
# * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# * 02110-1301, USA.
# */

# Probe the kernel build at $1 for SLASH API compatibility features.
#
# Each *.c file in this directory is built as a tiny standalone module
# against the target kernel headers; a successful build means the
# feature is available. One make-style assignment per feature is
# printed to stdout, e.g.:
#
#     SLASH_HAVE_VM_FLAGS_SET=y
#     SLASH_HAVE_MODULE_IMPORT_NS_STRING=n
#
# To add a new probe, drop another conftest .c file into this directory.
# The macro name is derived from the file basename (uppercased).

set -eu

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <kernel-build-dir>" >&2
    exit 2
fi

kdir="$1"
here="$(cd "$(dirname "$0")" && pwd)"

if [ ! -d "$kdir" ]; then
    echo "$0: kernel build dir '$kdir' not found" >&2
    exit 1
fi

scratch="$here/.scratch"
cleanup() { rm -rf "$scratch"; }
trap cleanup EXIT HUP INT TERM

# Conftest builds must actually compile to be meaningful. Drop any
# flags the parent make may have set (notably -n / --dry-run, which
# would make every probe look successful but produce no real result).
unset MAKEFLAGS MFLAGS MAKEOVERRIDES

rm -rf "$scratch"
mkdir -p "$scratch"
printf 'obj-m := conftest.o\n' > "$scratch/Makefile"

for src in "$here"/*.c; do
    [ -f "$src" ] || continue
    feat=$(basename "$src" .c)
    cp "$src" "$scratch/conftest.c"
    if "${MAKE:-make}" -s -C "$kdir" M="$scratch" modules >/dev/null 2>&1; then
        ans=y
    else
        ans=n
    fi
    printf 'SLASH_HAVE_%s=%s\n' "$(printf '%s' "$feat" | tr '[:lower:]' '[:upper:]')" "$ans"
done
