#!/usr/bin/env bash
# The failures of the three suites grouped by cause, and the top of each on
# one local page: the scripted WPT tests by the cause that blocks the most
# subtests (tools/wpt-gaps.sh), test262 by feature and by error, the CSS
# reference tests by the properties their test files use and by directory.
#
#   tools/suite-gaps.sh <build-dir> <out-dir> [--skip-run]
#
# Runs test262_runner, wpt_reftest and wpt_testharness from <build-dir>
# (--skip-run reads the logs a previous run left in <out-dir>) and writes
# into <out-dir>: test262-features.tsv (failing, tests with the feature,
# feature), test262-errors.tsv, test262-directories.tsv,
# reftest-properties.tsv (failing, tests using it, property),
# reftest-directories.tsv, reftest-reasons.tsv, scripted/ and index.html.
# An error is the message with quoted text, «values» and numbers replaced.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:?usage: tools/suite-gaps.sh <build-dir> <out-dir> [--skip-run]}"
out="${2:?usage: tools/suite-gaps.sh <build-dir> <out-dir> [--skip-run]}"
skip="${3:-}"
top=25
mkdir -p "$out"
out="$(cd "$out" && pwd)"
build="$(cd "$build" && pwd)"
export LC_ALL=C
tab=$'\t'

if [ "$skip" != "--skip-run" ]; then
    "$build/tests/test262_runner" "$here/test262" "$here/tests/test262/directories.txt" "$here/tests/test262/passing.txt" \
        --revision "$here/tests/test262/REVISION" --print 1000000 > "$out/test262.log" 2>&1 || true
    "$build/tests/wpt_reftest" "$here/wpt" "$here/tests/wpt/directories.txt" "$here/tests/wpt/passing.txt" \
        --revision "$here/tests/wpt/REVISION" --print 1000000 > "$out/reftest.log" 2>&1 || true
    "$build/tests/wpt_testharness" "$here/wpt" "$here/tests/wpt/harness-directories.txt" "$here/tests/wpt/harness-passing.txt" \
        --revision "$here/tests/wpt/REVISION" --failures "$out/scripted-failures.tsv" > "$out/scripted.log" 2>&1 || true
fi
grep -q 'TOTAL' "$out/test262.log" || { echo "test262.log has no score: the runner did not finish" >&2; exit 1; }
grep -q 'TOTAL' "$out/reftest.log" || { echo "reftest.log has no score: the runner did not finish" >&2; exit 1; }
grep -q '^wpt testharness:' "$out/scripted.log" || { echo "scripted.log has no score: the runner did not finish" >&2; exit 1; }

norm='
function norm(m) {
    gsub(/\(\/[^)]*\)/, "(PATH)", m)
    gsub(/\\xc2\\xab/, "«", m)
    gsub(/\\xc2\\xbb/, "»", m)
    gsub(/(https?|file|data):[^ "'"'"')]*/, "URL", m)
    gsub(/«[^»]*»/, "«…»", m)
    gsub(/"[^"]*"/, "\"…\"", m)
    gsub(/-?[0-9]+(\.[0-9]+)?(e[-+]?[0-9]+)?/, "N", m)
    gsub(/ +/, " ", m)
    return substr(m, 1, 160)
}'

# The failures: "FAIL <path>: <reason>".
failures() { grep -a '^FAIL ' "$1" | sed 's/^FAIL //' | awk '{ i = index($0, ": "); print (i ? substr($0, 1, i - 1) "\t" substr($0, i + 2) : $0 "\t") }'; }
failures "$out/test262.log" > "$out/test262-failing.tsv"
failures "$out/reftest.log" > "$out/reftest-failing.tsv"

# --- test262 -----------------------------------------------------------------
# Every scored test's features, from its front matter: `features: [a, b]` or
# a `- a` list under `features:`.
{ grep -v '^#' "$here/tests/test262/passing.txt"; cut -f1 "$out/test262-failing.tsv"; } | sort -u > "$out/test262-all.txt"
(cd "$here/test262" && xargs -a "$out/test262-all.txt" -n 4000 awk '
    FNR == 1 { front = 0; list = 0 }
    /^\/\*---/ { front = 1; next }
    front && /^---\*\// { nextfile }
    front {
        if (list) {
            if ($0 ~ /^[[:space:]]*-/) {
                f = $0
                sub(/^[[:space:]]*-[[:space:]]*/, "", f)
                sub(/[[:space:]]*(#.*)?$/, "", f)
                if (f != "")
                    print FILENAME "\t" f
                next
            }
            list = 0
        }
        if ($0 ~ /^features:/) {
            line = $0
            sub(/^features:[[:space:]]*/, "", line)
            if (line ~ /^\[/) {
                gsub(/[][]/, "", line)
                n = split(line, items, ",")
                for (i = 1; i <= n; i++) {
                    f = items[i]
                    gsub(/^[[:space:]]+|[[:space:]]+$/, "", f)
                    if (f != "")
                        print FILENAME "\t" f
                }
            } else if (line == "") {
                list = 1
            }
        }
    }') > "$out/test262-features-of.tsv"

awk -F'\t' -v out="$out" "$norm"'
    FILENAME == ARGV[1] { features[$1] = features[$1] SUBSEP $2; next }
    FILENAME == ARGV[2] {
        n = split(substr(features[$1], 2), f, SUBSEP)
        if (n == 0) { tests["(none)"]++ } else { for (i = 1; i <= n; i++) tests[f[i]]++ }
        next
    }
    {
        n = split(substr(features[$1], 2), f, SUBSEP)
        if (n == 0) { failing["(none)"]++ } else { for (i = 1; i <= n; i++) failing[f[i]]++ }
        e = norm($2)
        errors[e]++
        if (!(e in example)) example[e] = $1
        split($1, p, "/")
        directories[p[1] "/" p[2] "/" p[3]]++
    }
    END {
        for (k in failing) printf "%d\t%d\t%s\n", failing[k], tests[k], k > (out "/f.tmp")
        for (k in errors) printf "%d\t%s\t%s\n", errors[k], k, example[k] > (out "/e.tmp")
        for (k in directories) printf "%d\t%s\n", directories[k], k > (out "/d.tmp")
    }' "$out/test262-features-of.tsv" "$out/test262-all.txt" "$out/test262-failing.tsv"
{ printf 'failing\ttests\tfeature\n'; sort -t "$tab" -k1,1nr -k3,3 "$out/f.tmp"; } > "$out/test262-features.tsv"
{ printf 'failing\terror\ta test\n'; sort -t "$tab" -k1,1nr -k2,2 "$out/e.tmp"; } > "$out/test262-errors.tsv"
{ printf 'failing\tdirectory\n'; sort -t "$tab" -k1,1nr -k2,2 "$out/d.tmp"; } > "$out/test262-directories.tsv"

# --- CSS reference tests -----------------------------------------------------
# The property names each test file writes, in its <style> elements and
# style="" attributes, once per file.
{ grep -v '^#' "$here/tests/wpt/passing.txt" | sed 's/$/\tPASS/'; cut -f1 "$out/reftest-failing.tsv" | sed 's/$/\tFAIL/'; } > "$out/reftest-all.tsv"
# Each file's CSS is collected whole and split once at braces and semicolons
# (gawk, for ENDFILE); the part of a chunk before its last brace is a
# selector. Lines over 20,000 characters are skipped: generated data, not CSS.
(cd "$here/wpt" && cut -f1 "$out/reftest-all.tsv" | xargs -n 2000 gawk '
    function names(css,    chunks, n, i, part, brace, decls, m, j, name) {
        gsub(/\/\*[^*]*\*+([^\/*][^*]*\*+)*\//, "", css)
        n = split(css, chunks, "}")
        for (i = 1; i <= n; i++) {
            part = chunks[i]
            while ((brace = index(part, "{")) > 0)
                part = substr(part, brace + 1)
            m = split(part, decls, ";")
            for (j = 1; j <= m; j++) {
                if (!match(decls[j], /^[[:space:]]*-?[A-Za-z][A-Za-z-]*[[:space:]]*:/))
                    continue
                name = tolower(substr(decls[j], RSTART, RLENGTH))
                gsub(/[[:space:]:]/, "", name)
                if (!(name in seen)) {
                    seen[name] = 1
                    print FILENAME "\t" name
                }
            }
        }
    }
    FNR == 1 { css = ""; in_style = 0; delete seen }
    length($0) > 20000 { next }
    {
        rest = $0
        while (match(rest, /[sS][tT][yY][lL][eE][[:space:]]*=[[:space:]]*("[^"]*"|\047[^\047]*\047)/)) {
            attribute = substr(rest, RSTART, RLENGTH)
            sub(/^[^"\047]*["\047]/, "", attribute)
            css = css "}" substr(attribute, 1, length(attribute) - 1) "}"
            rest = substr(rest, RSTART + RLENGTH)
        }
        line = $0
        if (!in_style && match(line, /<[sS][tT][yY][lL][eE][^>]*>/)) {
            line = substr(line, RSTART + RLENGTH)
            css = css "}"
            in_style = 1
        }
        if (in_style) {
            if (match(line, /<\/[sS][tT][yY][lL][eE]>/)) {
                css = css substr(line, 1, RSTART - 1) "}"
                in_style = 0
            } else {
                css = css "\n" line
            }
        }
    }
    ENDFILE { names(css) }') > "$out/reftest-properties-of.tsv"

awk -F'\t' -v out="$out" "$norm"'
    FILENAME == ARGV[1] { verdict[$1] = $2; next }
    FILENAME == ARGV[2] { tests[$2]++; if (verdict[$1] == "FAIL") failing[$2]++; next }
    {
        r = norm($2)
        reasons[r]++
        if (!(r in example)) example[r] = $1
        split($1, p, "/")
        directories[p[1] "/" p[2]]++
    }
    END {
        for (k in tests) if (failing[k] > 0) printf "%d\t%d\t%s\n", failing[k], tests[k], k > (out "/p.tmp")
        for (k in reasons) printf "%d\t%s\t%s\n", reasons[k], k, example[k] > (out "/r.tmp")
        for (k in directories) printf "%d\t%s\n", directories[k], k > (out "/d.tmp")
    }' "$out/reftest-all.tsv" "$out/reftest-properties-of.tsv" "$out/reftest-failing.tsv"
{ printf 'failing\ttests\tproperty\n'; sort -t "$tab" -k1,1nr -k3,3 "$out/p.tmp"; } > "$out/reftest-properties.tsv"
# The same by the share of their tests that fail, for properties 20 or more tests use.
{
    printf 'failing %%\tfailing / tests\tproperty\n'
    awk -F'\t' '$2 >= 20 { printf "%.0f\t%d / %d\t%s\n", 100 * $1 / $2, $1, $2, $3 }' "$out/p.tmp" | sort -t "$tab" -k1,1nr -k3,3
} > "$out/reftest-properties-by-rate.tsv"
{ printf 'failing\treason\ta test\n'; sort -t "$tab" -k1,1nr -k2,2 "$out/r.tmp"; } > "$out/reftest-reasons.tsv"
{ printf 'failing\tdirectory\n'; sort -t "$tab" -k1,1nr -k2,2 "$out/d.tmp"; } > "$out/reftest-directories.tsv"
rm -f "$out/f.tmp" "$out/e.tmp" "$out/d.tmp" "$out/p.tmp" "$out/r.tmp"

# --- the scripted tests ------------------------------------------------------
bash "$here/tools/wpt-gaps.sh" "$out/scripted-failures.tsv" "$out/scripted" > "$out/scripted-summary.txt"

# --- the page ----------------------------------------------------------------
section() { # title, file, top, columns to show (awk field list), headings
    local title="$1" file="$2"
    printf '<section><h2>%s</h2><table>\n' "$title"
    awk -F'\t' -v top="$3" -v fields="$4" '
        function esc(s) { gsub(/&/, "\\&amp;", s); gsub(/</, "\\&lt;", s); gsub(/>/, "\\&gt;", s); return s }
        BEGIN { n = split(fields, show, ",") }
        NR == 1 || NR <= top + 1 {
            cell = NR == 1 ? "th" : "td"
            printf "<tr>"
            for (i = 1; i <= n; i++) printf "<%s>%s</%s>", cell, esc($show[i]), cell
            printf "</tr>\n"
        }' "$file"
    printf '</table></section>\n'
}
{
    printf '<!doctype html>\n<meta charset="utf-8">\n<title>Suite gaps</title>\n'
    printf '<style>body{font:14px system-ui,sans-serif;margin:0;padding:16px;background:#fafaf7;color:#222}'
    printf 'main{display:grid;grid-template-columns:repeat(auto-fit,minmax(360px,1fr));gap:16px}'
    printf 'section{overflow-x:auto}table{border-collapse:collapse;width:100%%}td,th{padding:2px 6px;border-bottom:1px solid #ddd;text-align:left;vertical-align:top}'
    printf 'td:first-child,td:nth-child(2){text-align:right;white-space:nowrap;font-variant-numeric:tabular-nums}h2{font-size:15px}</style>\n'
    printf '<h1>Suite gaps</h1>\n<p>%s</p><p>%s</p><p>%s</p>\n' \
        "$(grep -a '^wpt testharness:' "$out/scripted.log")" \
        "test262 $(grep -a 'TOTAL' "$out/test262.log" | tr -s ' ')" \
        "reference tests $(grep -a 'TOTAL' "$out/reftest.log" | tr -s ' ')"
    printf '<main>\n'
    section "Scripted WPT: causes by subtests blocked" "$out/scripted/causes.tsv" "$top" "1,3,4"
    section "test262: features" "$out/test262-features.tsv" "$top" "1,2,3"
    section "Reference tests: properties the failing tests use" "$out/reftest-properties.tsv" "$top" "1,2,3"
    section "Scripted WPT: files that stop early" "$out/scripted/short-files.tsv" "$top" "1,2,5"
    section "test262: errors" "$out/test262-errors.tsv" "$top" "1,2,3"
    section "Reference tests: directories" "$out/reftest-directories.tsv" "$top" "1,2"
    section "Reference tests: properties by the share of their tests that fail" "$out/reftest-properties-by-rate.tsv" "$top" "1,2,3"
    printf '</main>\n'
} > "$out/index.html"

echo "$(grep -a '^wpt testharness:' "$out/scripted.log")"
echo "test262 $(grep -a 'TOTAL' "$out/test262.log" | tr -s ' ')"
echo "reference tests $(grep -a 'TOTAL' "$out/reftest.log" | tr -s ' ')"
for file in test262-features test262-errors reftest-properties reftest-directories; do
    echo
    echo "== $file =="
    head -n 13 "$out/$file.tsv" | cut -c1-200 | column -t -s "$tab"
done
echo
echo "page: $out/index.html"
