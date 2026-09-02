#!/usr/bin/env bash
# Renders the Sashfold 100 — every row of tests/sashfold100/corpus.txt —
# with a built sashfold, one process per page under a time limit, and
# writes the dashboard (index.html + sashfold100.json) into the output
# directory beside the pictures. The sashfold100 generator is expected
# beside the sashfold executable (both are CMake targets).
#
#   tools/sashfold100.sh <sashfold-exe> <out-dir> [--corpus <file>] [--jobs N]
#                        [--timeout <seconds>] [--only <substring>]
#
# Each row leaves <id>.png (the page, cut at 2400 px), <id>-thumb.png (the
# viewport's top at 320 px), <id>.json (the render's report) and <id>.log
# (what the render printed). A render that does not finish in time, or
# exits without a report, gets a report saying so, so the dashboard
# always has a row for it.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
exe=""
out=""
corpus="$root/tests/sashfold100/corpus.txt"
jobs=4
limit=60
only=""
width=1024
height=768
max_height=2400

while [ $# -gt 0 ]; do
  case "$1" in
    --corpus) corpus="$2"; shift 2 ;;
    --jobs) jobs="$2"; shift 2 ;;
    --timeout) limit="$2"; shift 2 ;;
    --only) only="$2"; shift 2 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *)
      if [ -z "$exe" ]; then exe="$1"; elif [ -z "$out" ]; then out="$1"; else echo "too many arguments" >&2; exit 2; fi
      shift ;;
  esac
done
if [ -z "$exe" ] || [ -z "$out" ]; then
  echo "usage: tools/sashfold100.sh <sashfold-exe> <out-dir> [--corpus <file>] [--jobs N] [--timeout <seconds>] [--only <substring>]" >&2
  exit 2
fi
if [ ! -x "$exe" ]; then
  echo "not an executable: $exe" >&2
  exit 1
fi
generator="$(dirname "$exe")/sashfold100"
case "$exe" in *.exe) generator="$generator.exe" ;; esac
if [ ! -x "$generator" ]; then
  echo "the sashfold100 generator is not beside $exe (build the sashfold100 target)" >&2
  exit 1
fi
mkdir -p "$out"

render_row() { # id url
  local id="$1" url="$2" code
  rm -f "$out/$id.png" "$out/$id-thumb.png" "$out/$id.json" "$out/$id.log"
  set +e
  timeout --kill-after=10 "$limit" "$exe" --render "$url" -o "$out/$id.png" \
    --width "$width" --height "$height" --max-height "$max_height" \
    --thumbnail "$out/$id-thumb.png" --thumbnail-width 320 \
    --report "$out/$id.json" >"$out/$id.log" 2>&1
  code=$?
  set -e
  if [ ! -f "$out/$id.json" ]; then
    local outcome="crashed"
    if [ "$code" = 124 ] || [ "$code" = 137 ]; then outcome="timeout"; fi
    printf '{ "input": "%s", "outcome": "%s", "exit": %d }\n' "$url" "$outcome" "$code" >"$out/$id.json"
  fi
  echo "$id: exit $code"
}

# The rows are taken round-robin across the categories — the first row of
# each, then the second of each — so the pages running at once are on
# different hosts and no one site sees a burst (a category's rows often
# share a host, and Wikipedia's picture server answers a burst with 429).
started=$(date +%s)
running=0
count=0
while IFS=$'\t' read -r kind id category rank url flags note; do
  [ "$kind" = "row" ] || continue
  if [ -n "$only" ] && [[ "$id $url" != *"$only"* ]]; then continue; fi
  count=$((count + 1))
  render_row "$id" "$url" &
  running=$((running + 1))
  if [ "$running" -ge "$jobs" ]; then
    wait -n
    running=$((running - 1))
  fi
done < <(grep -v '^#' "$corpus" | awk -F'\t' '$1 == "row" { print ++turn[$3] "\t" $0 }' | sort -s -n -k1,1 | cut -f2-)
wait
echo "rendered $count page(s) in $(( $(date +%s) - started )) s"

"$generator" "$corpus" "$out" --html "$out/index.html" --json "$out/sashfold100.json"
