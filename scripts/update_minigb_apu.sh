#!/usr/bin/env bash
# Refresh the vendored MiniGB APU sources to a given upstream commit and
# rewrite the pin comment that records it. The APU lives inside the Peanut-GB
# repository, so run this with the same SHA as scripts/update_peanut_gb.sh.
# Usage: scripts/update_minigb_apu.sh <sha>
set -euo pipefail

REPO_URL="https://github.com/deltabeard/Peanut-GB"
API_URL="https://api.github.com/repos/deltabeard/Peanut-GB/commits"
SUBDIR="examples/sdl2/minigb_apu"
DEST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/lib/minigb_apu"

if [ "$#" -ne 1 ]; then
    echo "usage: $(basename "$0") <sha>" >&2
    exit 2
fi
sha="$1"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for f in minigb_apu.c minigb_apu.h LICENSE; do
    curl -fsSL "$REPO_URL/raw/$sha/$SUBDIR/$f" -o "$tmp/$f"
done

# Committer date, ISO 8601. The API pretty-prints, so this reads the second
# "date" line (author's comes first). Falls back to "unknown" rather than
# inventing one; re-run with network access to fill it in.
date="$(curl -fsSL "$API_URL/$sha" 2>/dev/null \
    | grep -o '"date": "[0-9-]\{10\}' \
    | sed -n '2s/.*"//p')"
: "${date:=unknown}"

# Insert the pin comment directly after the licence block's closing */. The
# LICENSE file carries no comment syntax and is copied verbatim.
for f in minigb_apu.c minigb_apu.h; do
    awk -v sha="$sha" -v date="$date" -v url="$REPO_URL" -v subdir="$SUBDIR" '
        BEGIN { done = 0 }
        { print }
        !done && /^ \*\// {
            print ""
            print "/*"
            print " * Vendored from " url " (" subdir "/) — upstream commit"
            print " * " sha " (" date ")."
            print " * Local modifications: none."
            print " * Update with scripts/update_minigb_apu.sh <sha>."
            print " */"
            done = 1
        }
    ' "$tmp/$f" > "$tmp/pinned.$f"
    mv "$tmp/pinned.$f" "$tmp/$f"
done

for f in minigb_apu.c minigb_apu.h LICENSE; do
    mv "$tmp/$f" "$DEST/$f"
done
echo "lib/minigb_apu/ <- $sha ($date)"
git -C "$DEST/../.." --no-pager diff --stat -- lib/minigb_apu
