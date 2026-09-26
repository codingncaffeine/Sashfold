#!/usr/bin/env bash
# Regenerate the published score: the per-directory table the site links to,
# and the two places the headline number is written by hand. The wiki keeps
# its own copy of the same number and is printed here as a reminder.
set -eu
cd "$(dirname "$0")/.."
build=${SASHFOLD_BUILD:-build-gcc}
runner="./$build/tests/wpt_reftest"
[ -f "$runner.exe" ] && runner="$runner.exe"
"$runner" wpt tests/wpt/directories.txt tests/wpt/passing.txt \
    --revision tests/wpt/REVISION --json docs/wpt.json --html docs/wpt.html > /dev/null 2>&1 || true

passed=$(grep -o '"passed": [0-9]*' docs/wpt.json | head -1 | grep -o '[0-9]*')
total=$(grep -o '"total": [0-9]*' docs/wpt.json | head -1 | grep -o '[0-9]*')
pct=$(awk -v p="$passed" -v t="$total" 'BEGIN { printf "%.1f", 100 * p / t }')
comma=$(printf '%s' "$total" | sed -E ':a; s/([0-9])([0-9]{3})($|,)/\1,\2\3/; ta')
dirs=$(grep -c '"path":' docs/wpt.json)

# Each of these must match exactly one line: the stat tiles sit next to
# three other figures that are not this one.
count() { grep -c "$1" "$2" || true; }
[ "$(count '| WPT CSS reference tests |' README.md)" = 1 ] || { printf 'README reference-test row moved\n'; exit 1; }
[ "$(count 'WPT CSS reference tests</a>' docs/index.html)" = 1 ] || { printf 'the score tile moved\n'; exit 1; }

sed -i -E "/\| WPT CSS reference tests \|/ s|\*\*[0-9]+ / [0-9]+ \([0-9.]+%\)\*\*|**${passed} / ${total} (${pct}%)**|" README.md
sed -i -E "/WPT CSS reference tests<\/a>/ s|[0-9]+ / [0-9]+|${passed} / ${total}|; /WPT CSS reference tests<\/a>/ s|[0-9.]+%, by directory|${pct}%, by directory|" docs/index.html

printf '%s / %s (%s%%) over %s directories (CSS2 and %s css-* ones)\n' "$passed" "$total" "$pct" "$dirs" "$((dirs - 1))"
grep -n "$passed / $total" README.md docs/index.html

# The scripted suite, scored by subtest: its own table, row and tile.
harness="./$build/tests/wpt_testharness"
[ -f "$harness.exe" ] && harness="$harness.exe"
"$harness" wpt tests/wpt/harness-directories.txt tests/wpt/harness-passing.txt \
    --revision tests/wpt/REVISION --json docs/wpt-harness.json --html docs/wpt-harness.html > /dev/null 2>&1 || true
hpassed=$(grep -o '"passed": [0-9]*' docs/wpt-harness.json | head -1 | grep -o '[0-9]*')
htotal=$(grep -o '"total": [0-9]*' docs/wpt-harness.json | head -1 | grep -o '[0-9]*')
hfiles=$(grep -o '"files": [0-9]*' docs/wpt-harness.json | head -1 | grep -o '[0-9]*')
hpct=$(awk -v p="$hpassed" -v t="$htotal" 'BEGIN { printf "%.1f", 100 * p / t }')
hcomma=$(printf '%s' "$htotal" | sed -E ':a; s/([0-9])([0-9]{3})($|,)/\1,\2\3/; ta')
[ "$(count '| WPT testharness tests |' README.md)" = 1 ] || { printf 'README harness row moved\n'; exit 1; }
[ "$(count 'WPT scripted tests</a>' docs/index.html)" = 1 ] || { printf 'the harness tile moved\n'; exit 1; }
sed -i -E "/\| WPT testharness tests \|/ s|\*\*[0-9]+ / [0-9]+ \([0-9.]+%\)\*\*|**${hpassed} / ${htotal} (${hpct}%)**|" README.md
sed -i -E "/WPT scripted tests<\/a>/ s|[0-9]+ / [0-9]+|${hpassed} / ${htotal}|; /WPT scripted tests<\/a>/ s|[0-9.]+%, by directory|${hpct}%, by directory|" docs/index.html
printf '%s / %s (%s%%) subtests over %s test files\n' "$hpassed" "$htotal" "$hpct" "$hfiles"
grep -n "$hpassed / $htotal" README.md docs/index.html
printf 'the wiki Measurements table wants the same two figures\n'
