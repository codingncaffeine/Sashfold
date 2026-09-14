#!/usr/bin/env bash
# Groups the scripted WPT runner's failures by cause and ranks the causes by
# the subtests they block, counted against a browser's subtests on the same
# files: a file that throws or times out early never generates the rest of
# its subtests, so our own count cannot see what it hides.
#
#   tools/wpt-gaps.sh <failures.tsv> <out-dir>
#
# <failures.tsv> is written by `wpt_testharness ... --failures <file>`; the
# browser counts are tests/wpt/browser-counts.tsv (tools/wpt-browser-counts.sh).
# Writes causes.tsv, directories.tsv and short-files.tsv into <out-dir> and
# prints the totals and the top causes.
#
# A cause is the status and the message with quoted strings, URLs and numbers
# replaced. Subtests a file hides (the browser's count minus ours) go to the
# cause that stopped it: why it never reported, its harness error, or else
# its last failing subtest. "<= chrome" caps each file's share at what Chrome
# passes there beyond us, so it is a ceiling, and causes sharing a file
# overlap.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
failures="${1:?usage: tools/wpt-gaps.sh <failures.tsv> <out-dir>}"
out="${2:?usage: tools/wpt-gaps.sh <failures.tsv> <out-dir>}"
counts="${SASHFOLD_WPT_BROWSER_COUNTS:-$here/tests/wpt/browser-counts.tsv}"
directories="$here/tests/wpt/harness-directories.txt"
top="${SASHFOLD_WPT_GAPS_TOP:-40}"
mkdir -p "$out"

awk -F'\t' -v out="$out" -f /dev/stdin "$directories" "$counts" "$failures" <<'AWK'
# Single-quoted names ('cloneContents', 'Range') are kept: they name the API.
# Every other quoted string is a value and is replaced.
function norm(m,    kept, rest, piece) {
    gsub(/\\[ntr]/, " ", m)
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
    sub(/ $/, "", m)
    return substr(m, 1, 160)
}
function cause(status, message) {
    return message == "" ? status : status ": " norm(message)
}
function directory_of(id,    j, best) {
    best = ""
    for (j = 1; j <= ndirs; j++)
        if (index(id, dirs[j] "/") == 1 && length(dirs[j]) > length(best))
            best = dirs[j]
    return best == "" ? "(other)" : best
}
FILENAME == ARGV[1] {
    if ($0 !~ /^#/ && $0 != "")
        dirs[++ndirs] = $0
    next
}
FILENAME == ARGV[2] {
    if ($0 !~ /^#/ && $0 != "") {
        chrome_passed[$1] = $2; chrome_total[$1] = $3
        firefox_passed[$1] = $4; firefox_total[$1] = $5
    }
    next
}
$1 == "file" {
    id = $2
    ids[++nids] = id
    passed[id] = $3; reported[id] = $4; completed[id] = $5
    harness[id] = $6; harness_message[id] = $7
    next
}
$1 == "fail" {
    c = cause($4, $5)
    in_file[c SUBSEP $2]++
    last_cause[$2] = c
    next
}
END {
    for (i = 1; i <= nids; i++) {
        id = ids[i]
        known = id in chrome_total
        total = known ? chrome_total[id] : reported[id]
        hidden = total - reported[id]
        if (hidden < 0)
            hidden = 0
        stopper = ""
        if (completed[id] == 0)
            stopper = cause("NO_REPORT", harness_message[id])
        else if (harness[id] != "OK")
            stopper = cause(harness[id], harness_message[id])
        else if (id in last_cause)
            stopper = last_cause[id]
        if (hidden > 0) {
            if (stopper == "")
                stopper = "(fewer subtests than Chrome, none failed)"
            in_file[stopper SUBSEP id] += hidden
            printf "%d\t%s\t%d / %d\t%d\t%s\n", hidden, id, passed[id], reported[id], total, stopper > (out "/short-files.unsorted")
        }
        d = directory_of(id)
        dir_files[d]++
        dir_passed[d] += passed[id]
        dir_reported[d] += reported[id]
        dir_total[d] += total
        dir_chrome[d] += known ? chrome_passed[id] : 0
        dir_firefox[d] += known ? firefox_passed[id] : 0
        if (!known)
            dir_unknown[d]++
        winnable[id] = known ? chrome_passed[id] - passed[id] : reported[id] - passed[id]
        if (winnable[id] < 0)
            winnable[id] = 0
    }
    for (key in in_file) {
        split(key, part, SUBSEP)
        c = part[1]; id = part[2]; n = in_file[key]
        blocked[c] += n
        ceiling[c] += n < winnable[id] ? n : winnable[id]
        files[c]++
        if (n > example_size[c]) {
            example_size[c] = n
            example[c] = id
        }
    }
    for (c in blocked)
        printf "%d\t%d\t%d\t%s\t%s\n", blocked[c], ceiling[c], files[c], c, example[c] > (out "/causes.unsorted")
    all_files = all_passed = all_reported = all_total = all_chrome = all_firefox = all_unknown = 0
    for (d in dir_files) {
        printf "%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", d, dir_files[d], dir_passed[d], dir_reported[d], dir_total[d], dir_chrome[d], dir_firefox[d], dir_unknown[d] > (out "/directories.unsorted")
        all_files += dir_files[d]; all_passed += dir_passed[d]; all_reported += dir_reported[d]
        all_total += dir_total[d]; all_chrome += dir_chrome[d]; all_firefox += dir_firefox[d]; all_unknown += dir_unknown[d]
    }
    printf "%d\t%d\t%d\t%d\t%d\t%d\t%d\n", all_files, all_passed, all_reported, all_total, all_chrome, all_firefox, all_unknown > (out "/totals.tsv")
}
AWK

tab=$'\t'
{ printf 'blocked\t<= chrome\tfiles\tcause\tlargest file\n'; sort -t "$tab" -k1,1nr -k4,4 "$out/causes.unsorted"; } > "$out/causes.tsv"
{ printf 'directory\tfiles\tpassed\treported\tchrome subtests\tchrome passed\tfirefox passed\tfiles without browser counts\n'; sort "$out/directories.unsorted"; } > "$out/directories.tsv"
{ printf 'hidden\tfile\tpassed / reported\tchrome subtests\tstopped by\n'; sort -t "$tab" -k1,1nr -k2,2 "$out/short-files.unsorted" 2>/dev/null || true; } > "$out/short-files.tsv"
rm -f "$out/causes.unsorted" "$out/directories.unsorted" "$out/short-files.unsorted"

IFS="$tab" read -r files passed reported total chrome firefox unknown < "$out/totals.tsv"
percent() { awk -v a="$1" -v b="$2" 'BEGIN { printf (b > 0 ? "%.1f%%" : "-"), 100 * a / b }'; }
echo "files $files ($unknown without browser counts)"
echo "passed $passed / reported $reported ($(percent "$passed" "$reported")) / chrome subtests $total ($(percent "$passed" "$total"))"
echo "chrome passes $chrome ($(percent "$chrome" "$total")), firefox passes $firefox"
echo
column -t -s "$tab" "$out/directories.tsv"
echo
head -n $((top + 1)) "$out/causes.tsv" | cut -c1-220 | column -t -s "$tab"
