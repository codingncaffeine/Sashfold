#!/usr/bin/env bash
# The pledge, enforced: a Sashfold binary may import only the operating
# system's own libraries. Anything else in the import table fails the build.
# Extend an allowlist ONLY alongside the platform work that needs the new
# import — never to make a red build green.
set -uo pipefail
export LC_ALL=C # msys grep aborts under some locales; C is deterministic everywhere

binary="${1:?usage: pledge-check.sh <binary>}"
here="$(dirname "$0")"

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) platform=windows ;;
    Linux) platform=linux ;;
    Darwin) platform=macos ;;
    *)
        echo "pledge-check: unrecognised platform '$(uname -s)'"
        exit 2
        ;;
esac

allowlist="$here/pledge-allowlist-$platform.txt"
if [ ! -f "$allowlist" ]; then
    echo "pledge-check: missing $allowlist"
    exit 2
fi

imports() {
    case "$platform" in
        windows) objdump -p "$binary" | awk '/DLL Name:/ { print tolower($3) }' ;;
        linux) objdump -p "$binary" | awk '/NEEDED/ { print $2 }' ;;
        macos) otool -L "$binary" | tail -n +2 | awk '{ print $1 }' | while read -r p; do basename "$p"; done ;;
    esac
}

violations=0
allowed=0
while IFS= read -r import; do
    [ -z "$import" ] && continue
    # No -i: Windows imports are lowercased at extraction, Linux/macOS names
    # are case-exact, and msys grep's -i can abort outright.
    if grep -qxF "$import" "$allowlist"; then
        allowed=$((allowed + 1))
    else
        echo "PLEDGE VIOLATION: $binary imports '$import' — not in $(basename "$allowlist")"
        violations=1
    fi
done < <(imports | sort -u)

if [ "$violations" -eq 0 ]; then
    echo "pledge-check PASS: $binary imports only OS libraries ($allowed allowed)"
fi
exit "$violations"
