#!/usr/bin/env bash
# Fetches ITU-T H.264.1's conformance bitstreams that tests/h264_conformance
# decodes (the progressive, frame-coded streams of the profiles the decoders
# read; the list is tests/h264-conformance/streams.txt) from the ITU's public
# archive into h264-conformance/<name>/, one folder per stream.
#   tools/h264-conformance-fetch.sh [repo root]
set -euo pipefail
root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
out="$root/h264-conformance"
list="$root/tests/h264-conformance/streams.txt"
base="https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/AVCv1"
command -v curl >/dev/null && command -v unzip >/dev/null || { echo "curl and unzip are needed" >&2; exit 2; }
mkdir -p "$out/.zips"
grep -v '^\s*#' "$list" | awk 'NF { print $1 }' | while read -r name; do
    [ -d "$out/$name" ] && continue
    zip="$out/.zips/$name.zip"
    [ -s "$zip" ] || curl -sfL --retry 3 -o "$zip" "$base/$name.zip" || { echo "could not fetch $name" >&2; rm -f "$zip"; continue; }
    mkdir -p "$out/$name.part" && unzip -qo "$zip" -d "$out/$name.part" && mv "$out/$name.part" "$out/$name"
    echo "fetched $name"
done
