#!/usr/bin/env bash
# Refresh the vendored gnuboy core to a given upstream commit and rewrite the
# pin comment that records it. Usage: scripts/update_gnuboy.sh <sha>
#
# Like scripts/update_peanut_gb.sh, this overwrites the vendored files and
# resets the pin comment's modification list to "none". Re-applying the local
# modifications is the caller's job: read them out of the pin comment (or
# git history) before running this, and rewrite the list afterwards.
set -euo pipefail

REPO_URL="https://github.com/ducalex/retro-go"
API_URL="https://api.github.com/repos/ducalex/retro-go/commits"
SRC_DIR="retro-core/components/gnuboy"
DEST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/lib/gnuboy"

# The core and its licence paperwork. Deliberately not the whole directory:
# upstream also ships CMakeLists.txt (retro-go's build), docs/ (630 KB of
# PDFs) and tests/ (a blargg archive), none of which this firmware wants.
FILES=(
    cpu.c cpu.h
    gnuboy.c gnuboy.h
    hw.c hw.h
    lcd.c lcd.h
    sound.c sound.h
    tables.h
    COPYING CREDITS
)

if [ "$#" -ne 1 ]; then
    echo "usage: $(basename "$0") <sha>" >&2
    exit 2
fi
sha="$1"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for f in "${FILES[@]}"; do
    curl -fsSL "$REPO_URL/raw/$sha/$SRC_DIR/$f" -o "$tmp/$f"
done

# Committer date, ISO 8601. The API pretty-prints, so this reads the second
# "date" line (author's comes first). Falls back to "unknown" rather than
# inventing one; re-run with network access to fill it in.
date="$(curl -fsSL "$API_URL/$sha" 2>/dev/null \
    | grep -o '"date": "[0-9-]\{10\}' \
    | sed -n '2s/.*"//p')"
: "${date:=unknown}"

# Head gnuboy.h with the pin comment. The file opens on #pragma once, not a
# licence block, so the note goes above it; COPYING carries the licence.
{
    echo "/*"
    echo " * Vendored from $REPO_URL — upstream commit"
    echo " * $sha ($date),"
    echo " * from $SRC_DIR."
    echo " * Licensed GPL-2.0-or-later; see COPYING beside this file and the"
    echo " * consequence recorded in the repository LICENSE."
    echo " * Local modifications: none."
    echo " * Update with scripts/update_gnuboy.sh <sha>."
    echo " */"
    echo ""
    cat "$tmp/gnuboy.h"
} > "$tmp/pinned.h"
mv "$tmp/pinned.h" "$tmp/gnuboy.h"

mkdir -p "$DEST"
for f in "${FILES[@]}"; do
    mv "$tmp/$f" "$DEST/$f"
done

echo "lib/gnuboy/ <- $sha ($date)"
echo "Re-apply the local modifications and rewrite the list in gnuboy.h." >&2
git -C "$DEST/../.." --no-pager diff --stat -- lib/gnuboy
