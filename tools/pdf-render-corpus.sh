#!/usr/bin/env bash
# Draws pages with the engine's PDF renderer, with poppler's pdftoppm and
# with Ghostscript at the same resolution, and prints how far ours is from
# each: the mean channel difference and the share of pixels off by more
# than 32 (pdf_probe --compare). Two references because they disagree with
# each other (poppler's tiling patterns have seams and a shifted phase; Ghostscript
# rounds differently): a page close to either is drawn right. One line per
# page; the pictures stay in the out folder (<name>-<page>.png ours,
# .poppler.png and .gs.png theirs) for looking.
#
#   tools/pdf-render-corpus.sh [--build DIR] [--dpi D] [--pages N] --out DIR <folder-or-files...>
#
# Needs pdftoppm (poppler) and gs (Ghostscript). Writes only into the out folder.
set -euo pipefail

build="build-dev"
dpi=72
pages=3
out=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build) build="$2"; shift 2 ;;
        --dpi) dpi="$2"; shift 2 ;;
        --pages) pages="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        *) break ;;
    esac
done
probe="$build/pdf_probe"
command -v pdftoppm >/dev/null || { echo "pdftoppm (poppler) is needed" >&2; exit 2; }
command -v gs >/dev/null || { echo "gs (Ghostscript) is needed" >&2; exit 2; }
[[ -x "$probe" ]] || { echo "no $probe" >&2; exit 2; }
[[ -n "$out" ]] || { echo "usage: $0 [--build DIR] [--dpi D] [--pages N] --out DIR <folder-or-files...>" >&2; exit 2; }
mkdir -p "$out"

files=()
for arg in "$@"; do
    if [[ -d "$arg" ]]; then
        while IFS= read -r -d '' f; do files+=("$f"); done < <(find "$arg" -type f -iname '*.pdf' -print0 | sort -z)
    else
        files+=("$arg")
    fi
done

for f in "${files[@]}"; do
    name=$(basename "$f" .pdf)
    count=$(pdfinfo "$f" 2>/dev/null | awk '/^Pages:/ { print $2 }')
    [[ -n "$count" ]] || { echo "SKIP  $name (poppler cannot open it)"; continue; }
    for ((p = 1; p <= count && p <= pages; ++p)); do
        ref="$out/$name-$p.poppler"
        pdftoppm -r "$dpi" -f "$p" -l "$p" -png -singlefile "$f" "$ref" 2>/dev/null || { echo "SKIP  $name p$p (pdftoppm failed)"; continue; }
        gs -q -dSAFER -dBATCH -dNOPAUSE -sDEVICE=png16m -r"$dpi" -dFirstPage="$p" -dLastPage="$p" -dTextAlphaBits=4 \
            -dGraphicsAlphaBits=4 -o "$out/$name-$p.gs.png" "$f" >/dev/null 2>&1 || true
        compares=(--compare "$ref.png")
        [[ -s "$out/$name-$p.gs.png" ]] && compares+=(--compare "$out/$name-$p.gs.png")
        line=$("$probe" --render "$p" --dpi "$dpi" --out "$out/$name-$p.png" "${compares[@]}" "$f" 2>&1 | tr '\n' ' ')
        printf '%-40s p%-3d %s\n' "$name" "$p" "$line"
    done
done
