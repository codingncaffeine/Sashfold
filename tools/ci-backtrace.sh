#!/usr/bin/env bash
# After a failed ctest: each failed test run again under gdb, its backtrace
# printed, so that a crash on a runner names the function it died in; then
# once more with the machine code off, since a crash that goes away there is
# the compiler's. Usage: tools/ci-backtrace.sh [build-dir]
set -u
build=${1:-build}
failed="$build/Testing/Temporary/LastTestsFailed.log"
if [ ! -f "$failed" ]; then
    echo "ci-backtrace: no failed tests recorded"
    exit 0
fi
while IFS=: read -r _ name; do
    # Windows writes the list, and ctest its output, with CRLF line ends.
    name=${name//$'\r'/}
    [ -n "$name" ] || continue
    line=$(ctest --test-dir "$build" -R "^${name}\$" -N -V | tr -d '\r' | sed -n 's/^[0-9]*: Test command: //p' | head -1)
    if [ -z "$line" ]; then
        echo "ci-backtrace: $name: no command found"
        continue
    fi
    # A Windows path's backslashes would be read as escapes here; Windows
    # takes forward slashes as well.
    line=${line//\\//}
    eval "set -- $line"
    echo "=== $name under gdb"
    timeout 900 gdb -batch -ex "set pagination off" -ex "set debuginfod enabled off" -ex run -ex "bt 40" -ex "info registers rip" -ex "x/8i \$pc" --args "$@" 2>&1 | tail -n 80
    echo "=== $name with SASHFOLD_JIT=off"
    SASHFOLD_JIT=off timeout 900 "$@" > /dev/null 2>&1
    echo "exit $?"
done < "$failed"
exit 0
