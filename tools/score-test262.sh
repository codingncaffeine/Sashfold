#!/usr/bin/env bash
# Regenerate the published test262 score: the per-directory table the site
# links to, and the two places the headline number is written by hand. The
# wiki keeps its own copy of the same number and is printed here as a
# reminder. The twin of score-docs.sh for the script engine's yardstick.
set -eu
cd "$(dirname "$0")/.."
build=${SASHFOLD_BUILD:-build-gcc}
"./$build/tests/test262_runner.exe" test262 tests/test262/directories.txt tests/test262/passing.txt \
    --revision tests/test262/REVISION --json docs/test262.json --html docs/test262.html --jobs 6 > /dev/null 2>&1 || true

passed=$(grep -o '"passed": [0-9]*' docs/test262.json | head -1 | grep -o '[0-9]*')
total=$(grep -o '"total": [0-9]*' docs/test262.json | head -1 | grep -o '[0-9]*')
pct=$(awk -v p="$passed" -v t="$total" 'BEGIN { printf "%.1f", 100 * p / t }')
comma=$(printf '%s' "$total" | sed -E ':a; s/([0-9])([0-9]{3})($|,)/\1,\2\3/; ta')
dirs=$(grep -c '"path":' docs/test262.json)

# Each of these must match exactly one line: the stat tiles sit next to
# other figures that are not this one.
count() { grep -c "$1" "$2" || true; }
[ "$(count 'tests over [0-9]* directories' README.md)" = 1 ] || { printf 'README test262 row moved\n'; exit 1; }
[ "$(count 'the ECMAScript conformance suite</a>' docs/index.html)" = 1 ] || { printf 'the test262 tile moved\n'; exit 1; }

sed -i -E "s|[0-9,]+ tests over [0-9]+ directories|${comma} tests over ${dirs} directories|" README.md
sed -i -E "/tests over [0-9]+ directories/ s|\*\*[0-9]+ / [0-9]+ \([0-9.]+%\)\*\*|**${passed} / ${total} (${pct}%)**|" README.md
sed -i -E "/ECMAScript conformance suite<\/a>/ s|[0-9]+ / [0-9]+|${passed} / ${total}|; /ECMAScript conformance suite<\/a>/ s|[0-9.]+%, by directory|${pct}%, by directory|" docs/index.html

printf '%s / %s (%s%%) over %s directories\n' "$passed" "$total" "$pct" "$dirs"
grep -n "$passed / $total" README.md docs/index.html
printf 'the wiki Measurements table wants the same figure\n'
