#!/usr/bin/env bash
# Writes tests/wpt/browser-counts.tsv: Chrome's and Firefox's passed and total
# subtests for every file in the scripted directories, from the wpt.fyi runs
# at the revision in tests/wpt/REVISION. Committed, so tools/wpt-gaps.sh works
# offline; re-run it when the revision moves.
#
#   tools/wpt-browser-counts.sh
#
# Rows: file, chrome passed, chrome total, firefox passed, firefox total.
# Variants (?include=...) are summed into their file; a file one browser has
# no result for gets 0 / 0 on that side.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
revision="$(grep -v '^#' "$here/tests/wpt/REVISION" | grep -m1 .)"
directories="$here/tests/wpt/harness-directories.txt"
target="$here/tests/wpt/browser-counts.tsv"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export LC_ALL=C

curl -fsS "https://wpt.fyi/api/runs?sha=${revision:0:10}&label=master" > "$work/runs.json"
pattern="^/($(grep -v '^#' "$directories" | grep . | paste -sd'|'))/"
header="# wpt.fyi runs at $revision:"
for browser in chrome firefox; do
    url="$(jq -r --arg b "$browser" 'map(select(.browser_name == $b))[0].results_url // empty' "$work/runs.json")"
    if [ -z "$url" ]; then
        echo "no $browser run on wpt.fyi at ${revision:0:10}" >&2
        exit 1
    fi
    header="$header $browser $(jq -r --arg b "$browser" 'map(select(.browser_name == $b))[0] | "\(.browser_version) (run \(.id))"' "$work/runs.json")"
    curl -fsS -o "$work/$browser.raw" "$url"
    # The summary is named .json.gz but usually served as plain JSON.
    if [ "$(head -c 2 "$work/$browser.raw" | od -An -tx1 | tr -d ' ')" = "1f8b" ]; then
        gzip -dc "$work/$browser.raw" > "$work/$browser.json"
    else
        mv "$work/$browser.raw" "$work/$browser.json"
    fi
    jq -r --arg re "$pattern" 'to_entries[] | select(.key | test($re)) | [(.key | sub("\\?.*$"; "") | ltrimstr("/")), .value.c[0], .value.c[1]] | @tsv' "$work/$browser.json" \
        | awk -F'\t' '{ passed[$1] += $2; total[$1] += $3 } END { for (id in total) print id "\t" passed[id] "\t" total[id] }' \
        | sort > "$work/$browser.tsv"
done

{
    echo "$header"
    join -t $'\t' -a 1 -a 2 -e 0 -o 0,1.2,1.3,2.2,2.3 "$work/chrome.tsv" "$work/firefox.tsv"
} > "$target"
echo "$(grep -vc '^#' "$target") files -> $target"
