#!/usr/bin/env bash
# Fetches the web-platform-tests checkout the CSS reference-test runner
# scores against: a shallow, blob-less clone of github.com/web-platform-tests/wpt
# holding only the directories in tests/wpt/directories.txt plus the
# suite's shared fonts, images, common, css/support and css/reference trees, at the
# revision pinned in tests/wpt/REVISION. About 30k files; nothing of it is
# committed here (wpt/ is ignored).
#
#   tools/wpt-fetch.sh [checkout-dir]      (default: wpt/ at the repo root)
#
# Run it again after moving REVISION: it fetches the new commit into the
# same checkout. Then: build/wpt_reftest wpt tests/wpt/directories.txt
# tests/wpt/passing.txt [--update].
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="${1:-$root/wpt}"
remote="https://github.com/web-platform-tests/wpt.git"

revision="$(grep -v '^#' "$root/tests/wpt/REVISION" | grep -v '^[[:space:]]*$' | head -1)"
if [ -z "$revision" ]; then
  echo "tests/wpt/REVISION names no revision" >&2
  exit 1
fi
mapfile -t dirs < <(grep -v '^#' "$root/tests/wpt/directories.txt" | grep -v '^[[:space:]]*$')
dirs+=(css/support css/reference fonts images common)

if [ ! -d "$dest/.git" ]; then
  git clone --no-checkout --depth 1 --filter=blob:none "$remote" "$dest"
  # Some test paths run past Windows' default path limit.
  git -C "$dest" config core.longpaths true
fi
git -C "$dest" sparse-checkout set "${dirs[@]}"
if ! git -C "$dest" cat-file -e "$revision^{commit}" 2>/dev/null; then
  git -C "$dest" fetch --depth 1 origin "$revision"
fi
git -C "$dest" checkout --quiet --detach "$revision"
echo "wpt at $(git -C "$dest" rev-parse HEAD) in $dest"
