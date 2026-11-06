#!/usr/bin/env bash
# Compares the engine's PDF reader with poppler's pdfinfo over a folder of
# PDFs (or the files named): page count, every page's size and rotation,
# the title and whether it is encrypted. Prints one line per file, SAME or
# DIFF with what differs, and a count at the end; exit 1 when any differ.
#
#   tools/pdf-corpus.sh [--build DIR] <folder-or-files...>
#
# Needs pdfinfo (poppler) on the PATH. Reads only; writes nothing.
set -euo pipefail

build="build-dev"
if [[ "${1:-}" == "--build" ]]; then
    build="$2"
    shift 2
fi
probe="$build/pdf_probe"
command -v pdfinfo >/dev/null || { echo "pdfinfo (poppler) is needed" >&2; exit 2; }
[[ -x "$probe" ]] || { echo "no $probe: cmake --build $build --target pdf_probe" >&2; exit 2; }
(( $# > 0 )) || { echo "usage: $0 [--build DIR] <folder-or-files...>" >&2; exit 2; }

files=()
for arg in "$@"; do
    if [[ -d "$arg" ]]; then
        while IFS= read -r -d '' f; do files+=("$f"); done < <(find "$arg" -type f -iname '*.pdf' -print0 | sort -z)
    else
        files+=("$arg")
    fi
done

same=0
differ=0
for f in "${files[@]}"; do
    # poppler's view, in the probe's shape.
    theirs=$(pdfinfo -f 1 -l 100000 "$f" 2>/dev/null | awk '
        /^Title:/ { sub(/^Title:[ ]*/, ""); if ($0 != "") print "Title: " $0 }
        /^Pages:/ { print "Pages: " $2 }
        /^Encrypted:/ { print "Encrypted: " $2 }
        /^Page +[0-9]+ size:/ { printf "Page %d size: %s x %s rot ", $2, $4, $6 }
        /^Page +[0-9]+ rot:/ { print $4 }
    ' || true)
    ours=$("$probe" "$f" 2>/dev/null | awk '
        /^Title:/ { sub(/^Title:[ ]*/, ""); if ($0 != "") print "Title: " $0 }
        /^Pages:/ { print "Pages: " $2 }
        /^Encrypted:/ { print "Encrypted: " $2 }
        /^Page [0-9]+ size:/ { print "Page " $2 " size: " $4 " x " $6 " rot " $9 }
        /^Error:/ { print "Error" }
    ' || true)
    # A file poppler refuses is a match for one the engine refuses.
    [[ -z "$theirs" ]] && theirs="Error"
    if [[ "$theirs" == "$ours" ]]; then
        same=$((same + 1))
        echo "SAME  $f"
    else
        differ=$((differ + 1))
        echo "DIFF  $f"
        { diff <(echo "$theirs") <(echo "$ours") || true; } | head -6 | sed 's/^/      /'
    fi
done
echo "--- $same same, $differ differ, of ${#files[@]}"
(( differ == 0 ))
