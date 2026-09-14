#!/usr/bin/env bash
# Ranks what the Sashfold 100 wrote that the engine dropped, from the
# <id>.gaps.tsv files a `tools/sashfold100.sh ... --gaps` run leaves beside
# the pictures: each property, at-rule, selector part, script error and
# element by how many of the pages use it.
#
#   tools/sashfold100-gaps.sh <out-dir> [<top per kind>]
#
# Writes <out-dir>/gaps.tsv (pages, hits, kind, item, one page that has it)
# and prints the top rows of each kind. Script errors are grouped with their
# quoted strings, URLs and numbers replaced; single-quoted names are kept.
set -euo pipefail
out="${1:?usage: tools/sashfold100-gaps.sh <out-dir> [<top per kind>]}"
top="${2:-15}"
shopt -s nullglob
files=("$out"/*.gaps.tsv)
if [ "${#files[@]}" -eq 0 ]; then
    echo "no .gaps.tsv files in $out (render with tools/sashfold100.sh --gaps)" >&2
    exit 1
fi
export LC_ALL=C

awk -F'\t' -f /dev/stdin "${files[@]}" <<'AWK' | sort -t $'\t' -k1,1nr -k2,2nr -k3,3 -k4,4 > "$out/gaps.tsv"
function norm(m,    kept, rest, piece) {
    gsub(/(https?|blob|data|about|javascript|file):[^ "')]*/, "URL", m)
    gsub(/"[^"]*"/, "\"…\"", m)
    kept = ""
    rest = m
    while (match(rest, /'[^']*'/)) {
        piece = substr(rest, RSTART, RLENGTH)
        if (piece !~ /^'[A-Za-z_$][A-Za-z0-9_$.]*'$/ || RLENGTH > 42)
            piece = "'…'"
        kept = kept substr(rest, 1, RSTART - 1) piece
        rest = substr(rest, RSTART + RLENGTH)
    }
    m = kept rest
    gsub(/-?[0-9]+(\.[0-9]+)?(e[-+]?[0-9]+)?/, "N", m)
    gsub(/ +/, " ", m)
    return substr(m, 1, 160)
}
FNR == 1 {
    page = FILENAME
    sub(/.*\//, "", page)
    sub(/\.gaps\.tsv$/, "", page)
}
NF >= 3 {
    item = $1 == "script error" ? norm($2) : $2
    key = $1 SUBSEP item
    if (!((key SUBSEP page) in seen)) {
        seen[key SUBSEP page] = 1
        pages[key]++
        if (!(key in example))
            example[key] = page
    }
    hits[key] += $3
}
END {
    for (key in pages) {
        split(key, part, SUBSEP)
        printf "%d\t%d\t%s\t%s\t%s\n", pages[key], hits[key], part[1], part[2], example[key]
    }
}
AWK

echo "${#files[@]} pages with a census"
for kind in "css property" "css at-rule" "css selector" "css selector sample" "script error" "html custom element" "html unknown element"; do
    echo
    echo "== $kind (pages, hits, item, a page) =="
    awk -F'\t' -v kind="$kind" -v top="$top" '$3 == kind && shown++ < top { printf "%5d %7d  %s  [%s]\n", $1, $2, substr($4, 1, 110), $5 }' "$out/gaps.tsv"
done
