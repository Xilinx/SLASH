#!/usr/bin/env bash
#
# Rsync the working tree to a remote host, respecting .gitignore.
# Does NOT touch git history on either side — just mirrors tracked/untracked
# non-ignored files.
#
# Usage:
#   ./scripts/push-remote.sh <host> [--dry-run]
#
# The host must be an SSH config entry listed in SCRATCH_MAP below.
# The repo dirname is detected automatically from the local checkout.
#
set -euo pipefail

# ── scratch directory map ────────────────────────────────────────────
# hostname → scratch root (repo dirname gets appended automatically)
declare -A SCRATCH_MAP=(
    [xirxlabs61]="/scratch/users/vserbu"
)
# ─────────────────────────────────────────────────────────────────────

REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
REPO_DIR="$(basename "$REPO_ROOT")"

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <host> [--dry-run]"
    echo ""
    echo "Known hosts:"
    for h in "${!SCRATCH_MAP[@]}"; do
        echo "  $h  →  ${SCRATCH_MAP[$h]}/$REPO_DIR"
    done
    exit 1
fi

HOST="$1"
shift

if [[ -z "${SCRATCH_MAP[$HOST]+x}" ]]; then
    echo "Unknown host: $HOST"
    echo ""
    echo "Known hosts:"
    for h in "${!SCRATCH_MAP[@]}"; do
        echo "  $h  →  ${SCRATCH_MAP[$h]}/$REPO_DIR"
    done
    exit 1
fi

DRY_RUN=""
for arg in "$@"; do
    case "$arg" in
        --dry-run|-n) DRY_RUN="--dry-run" ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

REMOTE_DEST="${HOST}:${SCRATCH_MAP[$HOST]}/$REPO_DIR"

echo "Syncing $REPO_DIR → $REMOTE_DEST"

rsync -az --no-i-r --info=progress2 --delete \
    --filter=':- .gitignore' \
    --exclude='.git/' \
    $DRY_RUN \
    "$REPO_ROOT/" \
    "$REMOTE_DEST"
