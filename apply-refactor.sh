#!/bin/bash
# apply-refactor.sh -- replay the src/blincolnlights source-tree refactor (phases 1-4,
# 16/17-Jul-2026) against a fresh, pre-refactor copy of the pidp1-mods repository.
#
# This applies the same changes recorded in REFACTOR-CHANGES.txt and REFACTOR.txt from the
# Cowork sandbox session where they were developed and build-verified. That sandbox could
# not delete/rename files directly (a mount restriction), so the "renames" there were done
# as copy-then-flag-old-for-manual-deletion; this script does the equivalent copy+delete in
# one pass on a normal filesystem where both operations work.
#
# Usage:
#   ./apply-refactor.sh [target-repo-dir]
#   ./apply-refactor.sh --dry-run [target-repo-dir]
#
# target-repo-dir defaults to the current directory. pidp1-refactor-payload.tar.gz must be
# in the same directory as this script (or set PAYLOAD=/path/to/it in the environment).
#
# What this does NOT do: run git add/commit/push, touch anything outside the paths listed
# below, or rebuild anything. Review the result (git status / git diff) and build-verify
# before committing, same as was done in the original sandbox session.

set -e

DRY_RUN=0
if [ "$1" = "--dry-run" ]; then
    DRY_RUN=1
    shift
fi

TARGET="${1:-.}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PAYLOAD="${PAYLOAD:-$SCRIPT_DIR/pidp1-refactor-payload.tar.gz}"

if [ ! -f "$PAYLOAD" ]; then
    echo "error: payload tarball not found at $PAYLOAD" >&2
    echo "       (set PAYLOAD=/path/to/pidp1-refactor-payload.tar.gz if it's elsewhere)" >&2
    exit 1
fi

cd "$TARGET"
TARGET="$(pwd)"

# Sanity check: make sure this looks like the right repo, and that it's still in the
# pre-refactor state (not already applied).
if [ ! -f src/blincolnlights/pdp1/pdp1.h ]; then
    echo "error: $TARGET does not look like a pidp1-mods checkout (no src/blincolnlights/pdp1/pdp1.h)" >&2
    exit 1
fi
if [ -d src/blincolnlights/panel/driver ]; then
    echo "error: $TARGET/src/blincolnlights/panel/driver already exists -- refactor looks" >&2
    echo "       like it's already been applied here. Refusing to run again." >&2
    exit 1
fi
if [ ! -d src/blincolnlights/panel_pidp1 ]; then
    echo "warning: $TARGET/src/blincolnlights/panel_pidp1 does not exist -- expected it to," >&2
    echo "         since phase 4 renames it away. Continuing anyway, but this repo may not" >&2
    echo "         be in the pre-refactor state this script assumes." >&2
fi

echo "Target repo: $TARGET"
echo "Payload:     $PAYLOAD"
echo

# --- Paths that phases 1/2/4 remove or supersede entirely -----------------------------
# (Phase 3, the Makefile dependency-tracking modernization, only edited files in place --
# nothing to delete for that phase. See each directory's own CLAUDE.md for that history.)
DELETE_PATHS=(
    src/blincolnlights/gpio.c
    src/blincolnlights/gpio.h
    src/blincolnlights/checkConfig.c
    src/blincolnlights/tools/mkptyfio_telnet.c
    src/blincolnlights/panel_pidp1
    src/blincolnlights/art
    src/blincolnlights/pinctrl
)

if [ "$DRY_RUN" = 1 ]; then
    echo "--- DRY RUN: no changes will be made ---"
    echo
    echo "Would extract $(tar -tzf "$PAYLOAD" | wc -l) files from payload:"
    tar -tzf "$PAYLOAD" | sed 's/^/  add\/update  /'
    echo
    echo "Would then remove (phases 1, 2, 4 dead/superseded paths):"
    for p in "${DELETE_PATHS[@]}"; do
        if [ -e "$p" ]; then
            echo "  remove     $p"
        else
            echo "  (already absent) $p"
        fi
    done
    exit 0
fi

echo "Extracting payload..."
tar -xzf "$PAYLOAD"

echo "Removing superseded paths..."
for p in "${DELETE_PATHS[@]}"; do
    if [ -e "$p" ]; then
        rm -rf "$p"
        echo "  removed  $p"
    else
        echo "  (already absent, skipped)  $p"
    fi
done

echo
echo "Done. $TARGET now reflects the phase 1-4 source-tree refactor."
echo
echo "Next steps (same as the original sandbox session did):"
echo "  1. git status / git diff -- review before staging anything."
echo "  2. Build-verify the moved/modernized targets, e.g.:"
echo "       make -C src/blincolnlights/panel/gpio"
echo "       make -C src/blincolnlights/panel/driver"
echo "       make -C src/blincolnlights/vpanel_pdp1"
echo "       make -C src/blincolnlights/tapevis"
echo "       make -C src/blincolnlights/pdp1"
echo "  3. Clean up any .o/.d/binary artifacts the build-verify step leaves behind before"
echo "     committing (same *.o/*.d .gitignore rule applies here as in the sandbox)."
echo "  4. This script never runs git add/commit/push -- that's on you, this is your call."
