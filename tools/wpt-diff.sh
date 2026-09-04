#!/usr/bin/env bash
# What changed between two reftest logs: which tests started passing, which
# stopped, and where the losses cluster. A total on its own hides a trade of
# twenty-two lost against four won, so this prints both sides and buckets
# the losses by directory and by test family.
#
#   tools/wpt-diff.sh old.log new.log [--lost-only]
set -u
old=${1:?usage: wpt-diff.sh old.log new.log}
new=${2:?usage: wpt-diff.sh old.log new.log}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fails() { grep "^FAIL " "$1" | cut -d: -f1 | sed 's/^FAIL //' | sort -u; }
fails "$old" > "$tmp/old"
fails "$new" > "$tmp/new"
comm -23 "$tmp/old" "$tmp/new" > "$tmp/won"   # failed before, passes now
comm -13 "$tmp/old" "$tmp/new" > "$tmp/lost"  # passed before, fails now

score() { grep -E "^(baseline held|[0-9]+ / [0-9]+|.*tests still pass)" "$1" | tail -1; }
printf 'old: %s\n' "$(score "$old")"
printf 'new: %s\n' "$(score "$new")"
printf '\n== %s won, %s lost ==\n' "$(wc -l < "$tmp/won")" "$(wc -l < "$tmp/lost")"

bucket() { # $1 = file of test paths, $2 = heading
    [ -s "$1" ] || return 0
    printf '\n-- %s by directory --\n' "$2"
    sed 's|^css/||' "$1" | awk -F/ '{ n = NF > 3 ? 3 : NF - 1; p = $1; for (i = 2; i <= n; i++) p = p "/" $i; print p }' \
        | sort | uniq -c | sort -rn | head -20
    printf -- '-- %s by family --\n' "$2"
    sed 's|^css/||' "$1" | sed -E 's/[-0-9]+[a-z]*\.(html|xht)$//' | sort | uniq -c | sort -rn | head -15
}
bucket "$tmp/lost" lost
[ "${3:-}" = "--lost-only" ] || bucket "$tmp/won" won

if [ -s "$tmp/lost" ]; then
    printf '\n-- every lost test --\n'
    cat "$tmp/lost"
    printf '\nlook at one with:\n  ./build-gcc/tests/wpt_reftest.exe wpt tests/wpt/directories.txt tests/wpt/passing.txt --revision tests/wpt/REVISION --only %s --dump /tmp/dump --print 5\n' "$(head -1 "$tmp/lost")"
fi
