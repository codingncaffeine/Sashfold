#!/usr/bin/env bash
# Regenerate the published score: the per-directory table the site links to,
# and the two places the headline number is written by hand. The wiki keeps
# its own copy of the same number and is printed here as a reminder.
set -eu
cd "$(dirname "$0")/.."
build=${SASHFOLD_BUILD:-build-gcc}
"./$build/tests/wpt_reftest.exe" wpt tests/wpt/directories.txt tests/wpt/passing.txt \
    --revision tests/wpt/REVISION --json docs/wpt.json --html docs/wpt.html > /dev/null 2>&1 || true

passed=$(grep -o '"passed": [0-9]*' docs/wpt.json | head -1 | grep -o '[0-9]*')
total=$(grep -o '"total": [0-9]*' docs/wpt.json | head -1 | grep -o '[0-9]*')
pct=$(awk -v p="$passed" -v t="$total" 'BEGIN { printf "%.1f", 100 * p / t }')
comma=$(printf '%s' "$total" | sed -E ':a; s/([0-9])([0-9]{3})($|,)/\1,\2\3/; ta')
dirs=$(grep -c '"path":' docs/wpt.json)

# Each of these must match exactly one line: the stat tiles sit next to
# three other figures that are not this one.
count() { grep -c "$1" "$2" || true; }
[ "$(count 'tests over CSS2' README.md)" = 1 ] || { printf 'README headline row moved\n'; exit 1; }
[ "$(count 'by directory' docs/index.html)" = 1 ] || { printf 'the score tile moved\n'; exit 1; }

sed -i -E "s|[0-9,]+ tests over CSS2|${comma} tests over CSS2|" README.md
sed -i -E "s|\*\*[0-9]+ / [0-9]+ \([0-9.]+%\)\*\*|**${passed} / ${total} (${pct}%)**|" README.md
sed -i -E "/by directory/ s|[0-9]+ / [0-9]+|${passed} / ${total}|; /by directory/ s|[0-9.]+%, by directory|${pct}%, by directory|" docs/index.html

printf '%s / %s (%s%%) over %s directories (CSS2 and %s css-* ones)\n' "$passed" "$total" "$pct" "$dirs" "$((dirs - 1))"
grep -n "$passed / $total" README.md docs/index.html
printf 'the wiki Measurements table wants the same figure\n'
