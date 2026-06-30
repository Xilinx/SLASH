#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
# Patch the rocr-runtime / libhsakmt source under a TheRock checkout so
# `hsa_amd_interop_map_buffer()` can import slash-exported dma-bufs (or any
# foreign-source dma-buf) on kernel >= 6.7.
#
# Why this is needed
# ------------------
# `hsa_amd_interop_map_buffer()` calls `hsaKmtRegisterGraphicsHandleToNodes`
# in libhsakmt, which issues `AMDKFD_IOC_GET_DMABUF_INFO` first. The
# kernel-side handler for that ioctl, `amdgpu_amdkfd_get_dmabuf_info()`,
# rejects any dma-buf whose `ops` field isn't `amdgpu_dmabuf_ops` with -EINVAL.
# Slash exports its own ops, so the discovery call hard-fails and the runtime
# never proceeds to `IMPORT_DMABUF`.
#
# The companion ioctl `AMDKFD_IOC_IMPORT_DMABUF` was generalised by Felix
# Kuehling in 2023 (commit 0188006d "drm/amdkfd: Import DMABufs for interop
# through DRM") to route through `drm_gem_prime_fd_to_handle()`, which
# handles foreign dma-bufs correctly. That patch is in mainline since 6.7.
#
# So everything is in place except the userspace gate. This script teaches
# libhsakmt to treat -EINVAL from `GET_DMABUF_INFO` as "foreign source" and
# synthesise the size/gpu_id/flags that IMPORT_DMABUF needs:
#   - size:    fetched via lseek(fd, 0, SEEK_END) on the dma-buf fd.
#   - gpu_id:  taken from the caller's NodeArray.
#   - flags:   0 (no special metadata).
#
# Idempotent: re-running the script after a successful patch is a no-op.
#
# Usage
# -----
#   ./patch_rocr_foreign_dmabuf.sh                 # uses default path
#   THEROCK_DIR=/path/to/therock ./patch_rocr_foreign_dmabuf.sh
#
# After the script finishes the rocr-runtime sub-build is re-built and
# installed; just re-run the example:
#   sudo LD_LIBRARY_PATH=/opt/rocm-slash/lib ./build/rp1_bringup_gpu /dev/slash_ctl0

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
EXAMPLE_DIR="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"
THEROCK_DIR="${THEROCK_DIR:-$EXAMPLE_DIR/rocm-slash-src/therock}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/opt/rocm-slash}"

if [[ ! -d "$THEROCK_DIR" ]]; then
    echo "ERROR: TheRock dir not found at: $THEROCK_DIR" >&2
    echo "Set THEROCK_DIR=/path/to/therock or pass it as env var." >&2
    exit 1
fi

echo "==> Locating libhsakmt fmm.c under $THEROCK_DIR"
FMM_C=$(find "$THEROCK_DIR" -type f -name 'fmm.c' -path '*libhsakmt/src/fmm.c' \
        2>/dev/null | head -1)
if [[ -z "$FMM_C" ]]; then
    echo "ERROR: libhsakmt/src/fmm.c not found." >&2
    echo "Has fetch_sources.py been run inside TheRock?" >&2
    exit 1
fi
echo "    $FMM_C"

echo "==> Applying foreign-dma-buf fallback patch"
python3 - "$FMM_C" <<'PYEOF'
import re
import sys

path = sys.argv[1]
src = open(path).read()

MARKER = "Foreign dma-buf fallback"
if MARKER in src:
    print("    Already patched. Skipping.")
    sys.exit(0)

# Match the second GET_DMABUF_INFO call (inside the metadata-retry block) and
# the subsequent `if (r) goto error_free_metadata;`. Insert our fallback
# between the retry block's closing brace and the goto.
#
# Tolerated variations across rocr-runtime versions:
#   - kmtIoctl(kfd_fd, ...)        (ROCT-Thunk-Interface, older rocr-runtime)
#   - hsakmt_ioctl(ctx->fd, ...)   (rocr-runtime 7.x merged libhsakmt with ctx)
pattern = re.compile(
    r"(\s+r = (?:kmtIoctl|hsakmt_ioctl)\([^,]+,\s*"
    r"AMDKFD_IOC_GET_DMABUF_INFO,\s*\(void \*\)&infoArgs\);\s*\n"
    r"\s*\}\s*\n)"
    r"(\s*if \(r\)\s*\n\s*goto error_free_metadata;)"
)
m = pattern.search(src)
if not m:
    print("ERROR: Could not find GET_DMABUF_INFO retry block.", file=sys.stderr)
    print("       The libhsakmt source may have been restructured.", file=sys.stderr)
    sys.exit(1)

PATCH = """
\t/* Foreign dma-buf fallback. amdgpu_amdkfd_get_dmabuf_info() returns
\t * -EINVAL when the dma-buf was exported by a non-amdgpu driver (e.g.
\t * a third-party FPGA driver such as slash). The kernel-side
\t * AMDKFD_IOC_IMPORT_DMABUF since 6.7 routes through
\t * drm_gem_prime_fd_to_handle() which DOES handle foreign sources, so
\t * we just need to synthesize the size/gpu_id/flags fields that
\t * GET_DMABUF_INFO would have produced and let the rest of the function
\t * proceed normally.
\t */
\tif (r && errno == EINVAL && gpu_id_array_size > 0) {
\t\toff_t dbuf_size = lseek((int)GraphicsResourceHandle, 0, SEEK_END);
\t\tif (dbuf_size > 0) {
\t\t\tinfoArgs.size          = (uint64_t)dbuf_size;
\t\t\tinfoArgs.gpu_id        = gpu_id_array[0];
\t\t\tinfoArgs.flags         = 0;
\t\t\tinfoArgs.metadata_size = 0;
\t\t\tfree(metadata);
\t\t\tmetadata               = NULL;
\t\t\tr = 0;
\t\t\tpr_info(\"[%s] foreign dma-buf fallback: gpu_id=%u size=0x%lx\\n\",
\t\t\t        __func__, infoArgs.gpu_id, (unsigned long)dbuf_size);
\t\t}
\t}
"""

new_src = src[:m.end(1)] + PATCH + src[m.start(2):]
open(path, "w").write(new_src)
print(f"    Patched {path}")
PYEOF

# Verify required headers are present (lseek + errno + SEEK_END).
echo "==> Verifying include dependencies"
NEEDED_INCLUDES=(unistd.h errno.h)
for hdr in "${NEEDED_INCLUDES[@]}"; do
    if ! grep -q "#include <${hdr}>" "$FMM_C"; then
        echo "    Adding #include <${hdr}>"
        # Insert after the last existing system include in the top block.
        python3 - "$FMM_C" "$hdr" <<'PYEOF'
import re, sys
path, hdr = sys.argv[1], sys.argv[2]
src = open(path).read()
inc = f"#include <{hdr}>\n"
if inc in src:
    sys.exit(0)
m = list(re.finditer(r"^#include <[^>]+>\n", src, re.MULTILINE))
if not m:
    sys.exit("No system #include block found")
last = m[-1]
src = src[:last.end()] + inc + src[last.end():]
open(path, "w").write(src)
PYEOF
    fi
done

if [[ "${NO_BUILD:-0}" == "1" ]]; then
    echo "==> NO_BUILD=1; skipping rebuild + install. Source patched in place."
    exit 0
fi

echo "==> Rebuilding rocr-runtime"
cd "$THEROCK_DIR"
# TheRock invalidates the rocr-runtime stamp file when the source tree changes,
# so a top-level cmake --build picks up the patch automatically. If the build
# system doesn't notice, force a re-stamp:
ROCR_STAMP=$(find build -path '*rocr-runtime*' -name '*.stamp' 2>/dev/null | head -1)
if [[ -n "$ROCR_STAMP" ]]; then
    touch -d 'now' "$FMM_C"
    rm -f "$ROCR_STAMP"
fi
cmake --build build

if [[ "${NO_INSTALL:-0}" == "1" ]]; then
    echo "==> NO_INSTALL=1; skipping install step."
    exit 0
fi

echo "==> Reinstalling to $INSTALL_PREFIX"
sudo cmake --install build
sudo ldconfig

echo "==> Verifying patched libhsakmt is loaded"
ROCR_LIB="$INSTALL_PREFIX/lib/libhsa-runtime64.so"
if ! [[ -e "$ROCR_LIB" ]]; then
    echo "WARN: $ROCR_LIB not found post-install; manual check required." >&2
else
    if strings "$ROCR_LIB" | grep -q 'foreign dma-buf fallback'; then
        echo "    OK: patched string present in $ROCR_LIB"
    else
        echo "    WARN: patched string not found in $ROCR_LIB. The runtime may"
        echo "    have been built before the patch landed; consult build logs."
    fi
fi

cat <<EOF

==> Done.

Re-run the GPU example to validate:

    cd "$EXAMPLE_DIR"
    sudo LD_LIBRARY_PATH=$INSTALL_PREFIX/lib ./build/rp1_bringup_gpu /dev/slash_ctl0

Expected: status=1 (PASS), slot0=0xdeadbeef.
On failure, capture: sudo dmesg | tail -100 and the strace from
the README troubleshooting section.
EOF
