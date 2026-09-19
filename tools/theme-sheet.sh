#!/bin/bash
# The chrome under many themes on one sheet: for judging its look by eye, and
# for a before and an after of any change to it.
#
#   tools/theme-sheet.sh <out.png> [--build-dir build-gcc] [--columns N] [--width W] [--height H] <theme>...
#
# A theme is one of our theme files (.json), or a Firefox or Chrome theme —
# its .xpi, its .crx, a .zip or its unpacked folder — which is converted as
# the window converts one dropped into the profile. Each is shown the same
# way: two tabs, the pointer over the one behind, the main menu open, a page
# of a few words. With no themes, the shipped ones.
#
# The run is headless and touches no profile: --script, its own temporary
# folder, nothing on the network.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
[ $# -ge 1 ] || { sed -n 2,14p "$0"; exit 2; }
out=$1; shift
build="build-gcc"; columns=2; width=900; height=360
themes=()
while [ $# -gt 0 ]; do
    case "$1" in
    --build-dir) build=$2; shift 2 ;;
    --columns) columns=$2; shift 2 ;;
    --width) width=$2; shift 2 ;;
    --height) height=$2; shift 2 ;;
    *) themes+=("$1"); shift ;;
    esac
done
[ ${#themes[@]} -gt 0 ] || themes=("$root"/themes/*.json)
work=$(mktemp -d "${TMPDIR:-/tmp}/sashfold-theme-sheet.XXXXXX")
made=()
tidy() { for f in "${made[@]:-}"; do [ -n "$f" ] && rm -f "$f"; done; rmdir "$work" 2>/dev/null || true; }
trap tidy EXIT

cells=()
index=0
for theme in "${themes[@]}"; do
    index=$((index + 1))
    theme=$(cd "$(dirname "$theme")" && pwd)/$(basename "$theme")
    label=$(basename "$theme")
    shot="$work/$index.png"
    script="$work/$index.script"
    made+=("$shot" "$script")
    {
        echo "resize $width $height"
        echo "open data:text/html,<title>The free encyclopedia</title><h1>A page</h1><p>Some words on it, and <a href=https://example.org/>a link</a>.</p>"
        case "$theme" in *.json) ;; *) echo "import-theme $theme" ;; esac
        echo "new-tab"
        echo "open data:text/html,<title>A second tab</title><p>In front.</p>"
        echo "move 120 20"
        echo "main-menu"
        echo "screenshot $shot"
    } > "$script"
    args=()
    case "$theme" in *.json) args=(--theme "$theme") ;; esac
    if ! "$root/$build/sashfold" --script "$script" "${args[@]}" --downloads "$work" > "$work/$index.log" 2>&1; then
        echo "theme-sheet: $label did not render:"; sed -n 1,5p "$work/$index.log"
        made+=("$work/$index.log")
        continue
    fi
    made+=("$work/$index.log")
    cells+=("$label=$shot")
done
[ ${#cells[@]} -gt 0 ] || { echo "theme-sheet: nothing rendered"; exit 1; }
"$root/$build/png_sheet" "$out" --columns "$columns" "${cells[@]}"
