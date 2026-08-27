#!/usr/bin/env bash
# Refresh the vendored Peanut-GB header to a given upstream commit and rewrite
# the pin comment that records it. Usage: scripts/update_peanut_gb.sh <sha>
set -euo pipefail

REPO_URL="https://github.com/deltabeard/Peanut-GB"
API_URL="https://api.github.com/repos/deltabeard/Peanut-GB/commits"
DEST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/include/peanut_gb.h"

if [ "$#" -ne 1 ]; then
    echo "usage: $(basename "$0") <sha>" >&2
    exit 2
fi
sha="$1"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

curl -fsSL "$REPO_URL/raw/$sha/peanut_gb.h" -o "$tmp/peanut_gb.h"

# Committer date, ISO 8601. The API pretty-prints, so this reads the second
# "date" line (author's comes first). Falls back to "unknown" rather than
# inventing one; re-run with network access to fill it in.
date="$(curl -fsSL "$API_URL/$sha" 2>/dev/null \
    | grep -o '"date": "[0-9-]\{10\}' \
    | sed -n '2s/.*"//p')"
: "${date:=unknown}"

# Insert the pin comment directly after the licence block's closing */.
awk -v sha="$sha" -v date="$date" -v url="$REPO_URL" '
    BEGIN { done = 0 }
    { print }
    !done && /^ \*\// {
        print ""
        print "/*"
        print " * Vendored from " url " — upstream commit"
        print " * " sha " (" date ")."
        print " * Local modifications: none."
        print " * Update with scripts/update_peanut_gb.sh <sha>."
        print " */"
        done = 1
    }
' "$tmp/peanut_gb.h" > "$tmp/pinned.h"

mv "$tmp/pinned.h" "$DEST"
echo "include/peanut_gb.h <- $sha ($date)"
git -C "$(dirname "$DEST")/.." --no-pager diff --stat -- include/peanut_gb.h
