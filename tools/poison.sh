#!/usr/bin/env bash
# Run a command with fresh and freed memory poisoned and freed blocks kept
# back (tools/poison-preload.cpp says why): a value swept while something
# still names it dies here, where glibc alone lets it pass.
#   tools/poison.sh build/tests/test_bindings
#   SASHFOLD_GC_STRESS=1 tools/poison.sh ctest --test-dir build -j 6
# glibc only: the preload calls the allocator by glibc's own names.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="${POISON_DIR:-${TMPDIR:-/tmp}/sashfold-poison-$(id -u)}"
mkdir -p "$out"
preload="$out/poison-preload.so"
if [ ! -f "$preload" ] || [ "$here/poison-preload.cpp" -nt "$preload" ]; then
    "${CXX:-g++}" -std=c++20 -O2 -fPIC -shared -fno-exceptions -fno-rtti \
        -o "$preload" "$here/poison-preload.cpp" -lpthread
fi
[ "$#" -gt 0 ] || { echo "usage: poison.sh <command> [arguments]" >&2; exit 2; }
LD_PRELOAD="$preload${LD_PRELOAD:+:$LD_PRELOAD}" exec "$@"
