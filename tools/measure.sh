#!/usr/bin/env bash
# One command per iteration: build, score the reference tests, and say what
# moved against the last blessed measurement.
#
#   tools/measure.sh                 build, score everything, diff vs base
#   tools/measure.sh --only css-grid build and score one directory (seconds)
#   tools/measure.sh --tests         run the unit tests and goldens too
#   tools/measure.sh --set-base      make the run just scored the new base
#
# Logs live in the build directory, which is not part of the repository.
set -u
cd "$(dirname "$0")/.."
build=${SASHFOLD_BUILD:-build-gcc}
logs=$build/wpt-logs
mkdir -p "$logs"

only=""
run_tests=0
set_base=0
while [ $# -gt 0 ]; do
    case $1 in
        --only) only=$2; shift 2 ;;
        --tests) run_tests=1; shift ;;
        --set-base) set_base=1; shift ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done

if [ $set_base = 1 ] && [ -f "$logs/new.log" ]; then
    cp "$logs/new.log" "$logs/base.log"
    printf 'base is now the last run: %s\n' "$(tail -1 "$logs/base.log")"
    exit 0
fi

printf '== building ==\n'
cmake --build "$build" 2>&1 | grep -Ev "^\[[0-9]+/[0-9]+\] (Building|Linking|Automatic)" | tail -20
[ "${PIPESTATUS[0]}" = 0 ] || { printf 'BUILD FAILED\n'; exit 1; }

if [ $run_tests = 1 ]; then
    printf '\n== unit tests and goldens ==\n'
    ctest --test-dir "$build" -E wpt_reftest --output-on-failure 2>&1 | tail -15
fi

printf '\n== scoring ==\n'
args=(wpt tests/wpt/directories.txt tests/wpt/passing.txt --revision tests/wpt/REVISION --print 100000)
[ -n "$only" ] && args+=(--only "$only")
"./$build/tests/wpt_reftest.exe" "${args[@]}" > "$logs/new.log" 2>&1
printf '%s\n' "$(tail -1 "$logs/new.log")"

if [ -n "$only" ]; then
    grep -c "^FAIL " "$logs/new.log" | sed 's/^/failures in this subset: /'
    exit 0
fi
# The denominator is an instrument too: it only moves when a directory is
# added to the list or the checkout changes, so a silent change means
# something is being scored that should not be — a scratch file left in the
# tree scores like a test and shows up as a loss with a strange name.
counted=$(grep -oE "TOTAL +[0-9]+ / +[0-9]+" "$logs/new.log" | grep -oE "[0-9]+$" | tail -1)
if [ -f "$logs/base.log" ] && [ -n "$counted" ]; then
    was=$(grep -oE "TOTAL +[0-9]+ / +[0-9]+" "$logs/base.log" | grep -oE "[0-9]+$" | tail -1)
    if [ -n "$was" ] && [ "$was" != "$counted" ]; then
        printf '\n!! the number of tests scored changed: %s -> %s\n' "$was" "$counted"
        printf '   nothing but a directory list or a checkout change should do that.\n'
    fi
fi
if [ -f "$logs/base.log" ]; then
    tools/wpt-diff.sh "$logs/base.log" "$logs/new.log"
else
    printf 'no base to compare against; run --set-base to make this one it\n'
fi
