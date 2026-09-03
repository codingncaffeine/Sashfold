#!/usr/bin/env bash
# Fetches the Unicode data files this repository reads but does not vendor:
# the two bidirectional-algorithm conformance files the bidi test scores
# against, and the four property files tools/gen-unicode.cpp turns into the
# committed headers. Together they are about 25 MB, which is why they are
# downloaded rather than committed (ucd/ is ignored).
#
#   tools/ucd-fetch.sh [directory]      (default: ucd/ at the repo root)
#
# The version comes from tests/ucd/VERSION; move it deliberately, refetch,
# regenerate the headers, and check that the ones already committed come
# back byte-identical before trusting anything new.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="${1:-$root/ucd}"

version="$(grep -v '^#' "$root/tests/ucd/VERSION" | grep -v '^[[:space:]]*$' | head -1)"
if [ -z "$version" ]; then
  echo "tests/ucd/VERSION names no version" >&2
  exit 1
fi

mkdir -p "$dest"
# Each file with the path it lives at: the IDNA table is under its own
# directory rather than the version's, and the bidi classes are under
# ucd/extracted. A wrong path answers with a 404 page rather than an error,
# so every download is checked for the header the file should start with.
fetch() {
  local url="$1" name="$2" marker="$3"
  if [ -s "$dest/$name" ] && head -c 200 "$dest/$name" | grep -q "$marker"; then
    return 0
  fi
  echo "fetching $name"
  curl -fsS -o "$dest/$name.part" "$url"
  if ! head -c 200 "$dest/$name.part" | grep -q "$marker"; then
    echo "$url did not answer with $name (no '$marker' in its first bytes)" >&2
    rm -f "$dest/$name.part"
    exit 1
  fi
  mv "$dest/$name.part" "$dest/$name"
}

base="https://www.unicode.org/Public/$version"
fetch "https://www.unicode.org/Public/idna/$version/IdnaMappingTable.txt" \
  IdnaMappingTable.txt "# IdnaMappingTable"
fetch "$base/ucd/UnicodeData.txt" UnicodeData.txt ";"
fetch "$base/ucd/DerivedNormalizationProps.txt" DerivedNormalizationProps.txt "# DerivedNormalizationProps"
fetch "$base/ucd/extracted/DerivedBidiClass.txt" DerivedBidiClass.txt "# DerivedBidiClass"
fetch "$base/ucd/BidiBrackets.txt" BidiBrackets.txt "# BidiBrackets"
fetch "$base/ucd/BidiTest.txt" BidiTest.txt "# BidiTest"
fetch "$base/ucd/BidiCharacterTest.txt" BidiCharacterTest.txt "# BidiCharacterTest"

echo "Unicode $version data is in $dest"
