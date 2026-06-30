#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
# End-to-end build of a foreign-dma-buf-capable ROCm from source via TheRock.
#
# What this script produces
# -------------------------
# A ROCm 7.x installation under $INSTALL_PREFIX containing a `libhsa-runtime64.so`
# whose `fmm_register_graphics_handle()` accepts dma-bufs exported by non-amdgpu
# drivers (slash, etc.) — the missing piece needed by `hsa_amd_interop_map_buffer()`
# to import an FPGA BAR into a HIP kernel's address space.
#
# What this script does, top to bottom
# ------------------------------------
#   0. Verifies host preconditions (kernel >= 6.7, CONFIG_PCI_P2PDMA=y, deps).
#   1. apt-installs missing build dependencies (sudo).
#   2. Clones https://github.com/ROCm/TheRock at $THEROCK_TAG to $SOURCE_DIR/therock.
#   3. Creates a Python venv inside TheRock and installs its requirements.
#   4. Fetches all ROCm component submodules via TheRock's fetch_sources.py (~10 min).
#   5. Applies the foreign-dma-buf fallback patch to rocr-runtime/libhsakmt
#      (via scripts/patch_rocr_foreign_dmabuf.sh in this same directory).
#   6. CMake-configures a trim build (compiler + core runtime + HIP runtime only).
#   7. Builds with all cores via ninja (~1 hour on a typical workstation).
#   8. Installs to $INSTALL_PREFIX (sudo).
#   9. Registers $INSTALL_PREFIX/lib with the dynamic linker.
#  10. Sanity-checks that the patched marker string is present in the installed
#      libhsa-runtime64.so.
#
# Idempotency
# -----------
# Re-running the script is safe. Each phase short-circuits when its outputs
# already exist (clone -> skip if dir present; venv -> skip if .venv present;
# patch -> skip if marker present; configure -> use existing CMakeCache; etc.).
#
# Configuration (env vars; defaults shown)
# ----------------------------------------
#   SOURCE_DIR=<example>/rocm-slash-src     # where TheRock is cloned
#   INSTALL_PREFIX=/opt/rocm-slash            # where ROCm is installed
#   THEROCK_TAG=therock-7.12                # TheRock release tag to build
#   GPU_ARCH=gfx908                         # target GPU architecture
#   JOBS=$(nproc)                           # parallelism for the build
#   SKIP_APT=0                              # set to 1 to skip apt-install step
#   FORCE_RECLONE=0                         # set to 1 to nuke and re-clone TheRock
#
# Example
# -------
#   GPU_ARCH=gfx942 INSTALL_PREFIX=/opt/rocm-mi300 \
#       ./build_rocm_with_foreign_dmabuf.sh
#
# Notes
# -----
# - The build takes >= 1 hour and >= 30 GB disk under $SOURCE_DIR.
# - sudo is required for apt install, `make install`, and ldconfig.
# - This script does NOT load or build kernel modules; it only produces the
#   ROCm userspace stack. Use `examples/rp1_bringup_gpu/README.md` for the
#   kernel/firmware/slash side.

set -euo pipefail

# ─── Configuration ──────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
EXAMPLE_DIR="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"
PATCH_SCRIPT="$SCRIPT_DIR/patch_rocr_foreign_dmabuf.sh"

# Source tree lives under the example by default. Gitignored at the repo root.
# Likely co-located with the SLASH-fw checkout (often on /scratch, /work, etc.),
# avoiding NFS-quota'd $HOME on lab machines.
SOURCE_DIR="${SOURCE_DIR:-$EXAMPLE_DIR/rocm-slash-src}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/opt/rocm-slash}"
THEROCK_TAG="${THEROCK_TAG:-therock-7.12}"
GPU_ARCH="${GPU_ARCH:-gfx908}"
JOBS="${JOBS:-$(nproc)}"
SKIP_APT="${SKIP_APT:-0}"
FORCE_RECLONE="${FORCE_RECLONE:-0}"

THEROCK_DIR="$SOURCE_DIR/therock"

# ─── Helpers ────────────────────────────────────────────────────────────────

PHASE_START_TS=0
phase() {
    PHASE_START_TS=$SECONDS
    printf '\n\033[1;36m=== %s ===\033[0m\n' "$*"
}
phase_done() {
    local dur=$((SECONDS - PHASE_START_TS))
    printf '    \033[2m(%d m %d s)\033[0m\n' $((dur / 60)) $((dur % 60))
}
info()  { printf '    %s\n' "$*"; }
warn()  { printf '\033[33mWARN: %s\033[0m\n' "$*" >&2; }
err()   { printf '\033[31mERROR: %s\033[0m\n' "$*" >&2; }
die()   { err "$*"; exit 1; }

# ─── 0. Preconditions ───────────────────────────────────────────────────────

phase "0. Verifying host preconditions"

# Kernel version
KVER="$(uname -r)"
KMAJ=$(echo "$KVER" | awk -F. '{print $1}')
KMIN=$(echo "$KVER" | awk -F. '{print $2}')
if (( KMAJ < 6 )) || (( KMAJ == 6 && KMIN < 7 )); then
    die "Kernel $KVER is too old. Need >= 6.7 for in-tree amdgpu foreign-dmabuf import."
fi
info "Kernel: $KVER (OK)"

# CONFIG_PCI_P2PDMA must be on
CFG=/boot/config-$KVER
if [[ -r $CFG ]]; then
    if ! grep -q '^CONFIG_PCI_P2PDMA=y' "$CFG"; then
        die "Kernel $KVER was not built with CONFIG_PCI_P2PDMA=y."
    fi
    info "CONFIG_PCI_P2PDMA=y (OK)"
else
    warn "$CFG not readable; cannot confirm CONFIG_PCI_P2PDMA. Continuing."
fi

# Patch script must exist
if [[ ! -x $PATCH_SCRIPT ]]; then
    die "Patch script not found or not executable: $PATCH_SCRIPT"
fi
info "Patch script: $PATCH_SCRIPT (OK)"

# Disk space (rough check: need ~30 GB under $SOURCE_DIR's filesystem)
mkdir -p "$SOURCE_DIR"
AVAIL_KB=$(df -P "$SOURCE_DIR" | awk 'NR==2 {print $4}')
if (( AVAIL_KB < 30 * 1024 * 1024 )); then
    warn "Less than 30 GB free under $SOURCE_DIR. The build needs ~30 GB; expect failures."
else
    info "Disk space under $SOURCE_DIR: $(numfmt --to=iec --suffix=B $((AVAIL_KB * 1024))) (OK)"
fi

phase_done

# ─── 1. apt deps ────────────────────────────────────────────────────────────

phase "1. Installing build dependencies"

if [[ "$SKIP_APT" == "1" ]]; then
    info "SKIP_APT=1; skipping apt install."
else
    sudo apt update
    sudo apt install -y \
        build-essential cmake ninja-build git git-lfs pkg-config ccache patchelf \
        libelf-dev libdrm-dev libdrm-amdgpu1 libnuma-dev libpci-dev \
        libsystemd-dev libegl1-mesa-dev \
        python3 python3-venv python3-dev gfortran \
        automake libtool xxd texinfo bison flex curl make
fi
phase_done

# ─── 2. Clone TheRock ───────────────────────────────────────────────────────

phase "2. Cloning TheRock at $THEROCK_TAG"

if [[ "$FORCE_RECLONE" == "1" && -d "$THEROCK_DIR" ]]; then
    info "FORCE_RECLONE=1; removing existing $THEROCK_DIR"
    rm -rf "$THEROCK_DIR"
fi

if [[ -d "$THEROCK_DIR/.git" ]]; then
    info "$THEROCK_DIR already exists; fetching tags."
    git -C "$THEROCK_DIR" fetch --tags --quiet
    CURRENT_HEAD=$(git -C "$THEROCK_DIR" describe --tags --exact-match 2>/dev/null || echo "")
    if [[ "$CURRENT_HEAD" != "$THEROCK_TAG" ]]; then
        info "Checking out $THEROCK_TAG (was: ${CURRENT_HEAD:-detached})"
        git -C "$THEROCK_DIR" checkout "tags/$THEROCK_TAG"
    else
        info "Already on $THEROCK_TAG."
    fi
else
    git clone --branch "$THEROCK_TAG" --depth 1 \
        https://github.com/ROCm/TheRock.git "$THEROCK_DIR"
fi

phase_done

# ─── 3. Python venv ─────────────────────────────────────────────────────────

phase "3. Setting up Python venv"

cd "$THEROCK_DIR"
if [[ ! -d .venv ]]; then
    python3 -m venv .venv
fi
# Activate for the rest of the script.
# shellcheck disable=SC1091
source .venv/bin/activate

pip install --upgrade --quiet pip
pip install --quiet -r requirements.txt
phase_done

# ─── 4. Fetch component submodules ──────────────────────────────────────────

phase "4. Fetching ROCm component submodules"

# TheRock's fetch_sources.py is itself idempotent.
python3 ./build_tools/fetch_sources.py

# Confirm the target file exists before we attempt to patch it.
FMM_C="$THEROCK_DIR/rocm-systems/projects/rocr-runtime/libhsakmt/src/fmm.c"
if [[ ! -f "$FMM_C" ]]; then
    die "Expected source file not found post-fetch: $FMM_C"
fi
info "Found target source: $FMM_C"

phase_done

# ─── 5. Apply foreign-dma-buf patch ─────────────────────────────────────────

phase "5. Applying foreign-dma-buf patch to rocr-runtime"

# The patch script handles both the file edit and the include checks.
# We don't run its build/install steps here; we'll do a single build below.
# To run only the patch step, set PATCH_ONLY=1 in its env. Otherwise the
# script will also try to rebuild, which is wasteful before we configure.
PATCH_THEROCK_DIR="$THEROCK_DIR" \
PATCH_INSTALL_PREFIX="$INSTALL_PREFIX" \
THEROCK_DIR="$THEROCK_DIR" \
INSTALL_PREFIX="$INSTALL_PREFIX" \
NO_BUILD=1 NO_INSTALL=1 \
    "$PATCH_SCRIPT" || warn "Patch script returned non-zero; continuing if patch is already present."

# Verify the patch landed (idempotent check).
if grep -q "Foreign dma-buf fallback" "$FMM_C"; then
    info "Patch marker present in $FMM_C (OK)"
else
    die "Patch not applied to $FMM_C; aborting before the build."
fi

phase_done

# ─── 6. CMake configure ─────────────────────────────────────────────────────

phase "6. CMake configure (trim: compiler + core + hip runtime, $GPU_ARCH only)"

cd "$THEROCK_DIR"

# Set up TheRock's ccache config for any future incremental rebuilds.
eval "$(./build_tools/setup_ccache.py)"

# THEROCK_DIST_AMDGPU_FAMILIES is required as of 7.12; set both for robustness
# across older / newer tags.
cmake -B build -GNinja . \
    -DTHEROCK_AMDGPU_TARGETS="$GPU_ARCH" \
    -DTHEROCK_AMDGPU_FAMILIES="$GPU_ARCH" \
    -DTHEROCK_DIST_AMDGPU_FAMILIES="$GPU_ARCH" \
    -DTHEROCK_ENABLE_ALL=OFF \
    -DTHEROCK_ENABLE_COMPILER=ON \
    -DTHEROCK_ENABLE_CORE_RUNTIME=ON \
    -DTHEROCK_ENABLE_HIP_RUNTIME=ON \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"

phase_done

# ─── 7. Build ───────────────────────────────────────────────────────────────

phase "7. Building ROCm ($JOBS parallel jobs; expect ~1 hour)"

# If the patch was applied to an already-built tree, force the rocr-runtime
# stamp to be invalidated so the build actually picks up our change.
ROCR_STAMP=$(find build -path '*rocr-runtime*' -name '*.stamp' 2>/dev/null | head -1)
if [[ -n "$ROCR_STAMP" ]]; then
    info "Invalidating $ROCR_STAMP to pick up patch"
    rm -f "$ROCR_STAMP"
fi

cmake --build build -j "$JOBS"
phase_done

# ─── 8. Install ─────────────────────────────────────────────────────────────

phase "8. Installing to $INSTALL_PREFIX"

# CMake install may need sudo if $INSTALL_PREFIX is outside the home dir.
if [[ "$INSTALL_PREFIX" == "$HOME"* ]]; then
    cmake --install build
else
    sudo cmake --install build
fi
phase_done

# ─── 9. Register with ld.so ─────────────────────────────────────────────────

phase "9. Registering $INSTALL_PREFIX/lib with the dynamic linker"

LDSO_CONF="/etc/ld.so.conf.d/rocm-slash.conf"
if [[ ! -f "$LDSO_CONF" ]] || ! grep -qxF "$INSTALL_PREFIX/lib" "$LDSO_CONF"; then
    echo "$INSTALL_PREFIX/lib" | sudo tee "$LDSO_CONF" >/dev/null
    info "Wrote $LDSO_CONF"
fi
sudo ldconfig
phase_done

# ─── 10. Verify ─────────────────────────────────────────────────────────────

phase "10. Verifying installation"

ROCR_LIB="$INSTALL_PREFIX/lib/libhsa-runtime64.so"
HIP_LIB="$INSTALL_PREFIX/lib/libamdhip64.so"
HIPCC="$INSTALL_PREFIX/bin/amdclang++"

[[ -e "$ROCR_LIB" ]] || die "$ROCR_LIB missing"
[[ -e "$HIP_LIB"  ]] || die "$HIP_LIB missing"
[[ -x "$HIPCC"    ]] || die "$HIPCC missing or not executable"
info "$ROCR_LIB present"
info "$HIP_LIB present"
info "$HIPCC present"

if strings "$ROCR_LIB" | grep -q 'foreign dma-buf fallback'; then
    info "Patch marker present in $ROCR_LIB (OK)"
else
    die "Patch marker NOT found in $ROCR_LIB; build may have used a stale object."
fi

if [[ -x "$INSTALL_PREFIX/bin/rocminfo" ]]; then
    info "rocminfo reports:"
    "$INSTALL_PREFIX/bin/rocminfo" 2>/dev/null | grep -E 'Runtime Version|Name:.*gfx' | sed 's/^/        /'
fi

phase_done

# ─── Done ───────────────────────────────────────────────────────────────────

cat <<EOF

──────────────────────────────────────────────────────────────────────────────
 ROCm built and installed with foreign-dma-buf support.

   Source tree : $THEROCK_DIR
   Install dir : $INSTALL_PREFIX
   GPU target  : $GPU_ARCH
   ROCm tag    : $THEROCK_TAG

 Build the GPU example against it:

   cd <SLASH-fw>/examples/rp1_bringup_gpu
   cmake -B build -S . \\
       -DCMAKE_HIP_COMPILER=$INSTALL_PREFIX/bin/amdclang++ \\
       -DGPU_ARCH=$GPU_ARCH
   cmake --build build -j

 Run it:

   sudo ./build/rp1_bringup_gpu /dev/slash_ctl0

 (sudo no longer needs LD_LIBRARY_PATH; we registered $INSTALL_PREFIX/lib
  with ldconfig in step 9.)
──────────────────────────────────────────────────────────────────────────────
EOF
