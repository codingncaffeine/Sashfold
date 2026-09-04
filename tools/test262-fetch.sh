#!/usr/bin/env bash
# Fetches the test262 checkout the script-engine conformance runner scores
# against: a shallow, blob-less clone of github.com/tc39/test262 holding
# the harness plus the directories in tests/test262/directories.txt, at the
# revision pinned in tests/test262/REVISION. Nothing of it is committed
# here (test262/ is ignored).
#
#   tools/test262-fetch.sh [checkout-dir]      (default: test262/ at the repo root)
#
# Run it again after moving REVISION: it fetches the new commit into the
# same checkout. Then: build/tests/test262_runner test262 tests/test262/directories.txt
# tests/test262/passing.txt [--update].
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="${1:-$root/test262}"
remote="https://github.com/tc39/test262.git"

revision="$(grep -v '^#' "$root/tests/test262/REVISION" | grep -v '^[[:space:]]*$' | head -1)"
if [ -z "$revision" ]; then
  echo "tests/test262/REVISION names no revision" >&2
  exit 1
fi
# A line starting with - is an exclusion the runner applies; it is not a directory.
mapfile -t dirs < <(grep -v '^#' "$root/tests/test262/directories.txt" | grep -v '^[[:space:]]*$' | grep -v '^-')
dirs+=(harness)

if [ ! -d "$dest/.git" ]; then
  git clone --no-checkout --depth 1 --filter=blob:none "$remote" "$dest"
  git -C "$dest" config core.longpaths true
fi
git -C "$dest" sparse-checkout set "${dirs[@]}"
if ! git -C "$dest" cat-file -e "$revision^{commit}" 2>/dev/null; then
  git -C "$dest" fetch --depth 1 origin "$revision"
fi
git -C "$dest" checkout --quiet --detach "$revision"
echo "test262 at $(git -C "$dest" rev-parse HEAD) in $dest"
