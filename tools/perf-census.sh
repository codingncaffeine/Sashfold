#!/usr/bin/env bash
# Where the seconds go, ranked, over a rendered Sashfold 100 corpus: reads
# census.tsv (tools/sashfold100.cpp writes it beside index.html and
# sashfold100.json, one row per page whether it loaded or not) and says
# which step of a page's request-to-pixels time costs the hundred the most,
# so the next piece of speed work is picked by more than a hunch. This is
# the same census the dashboard's own "Where the seconds go" table comes
# from; here for a column to rank the slowest pages by, and a longer list
# than the ten the dashboard keeps.
#
#   tools/perf-census.sh <renders-dir or census.tsv> [--top N] [--by <column>]
#
# <renders-dir> is a tools/sashfold100.sh output directory, whose
# census.tsv is read, or the tsv file itself. --top N (default 10) is how
# many pages the slowest-pages list shows. --by <column> ranks that list by
# a column other than total: network, engine, resolve, connect, tls,
# first_byte, body, requests, reused, bytes, document, stylesheets,
# scripts, images, fonts, xhr, parse, scripts_run, sheets_read, style,
# images_phase, layout or paint.
#
# Prints, in order: a one-line summary over the pages that loaded; "where
# the seconds go", the median and sum of every step and, for the six
# columns that are what got fetched, how many pages each is the biggest of;
# and the slowest pages by the chosen column, then which pages did not load
# at all, if any.
set -euo pipefail
export LC_ALL=C

usage="usage: tools/perf-census.sh <renders-dir or census.tsv> [--top N] [--by <column>]"
bad_usage() { echo "$usage${1:+ ($1)}" >&2; exit 2; }

[ $# -ge 1 ] || bad_usage
input="$1"
shift
top=10
by=total
while [ $# -gt 0 ]; do
    case "$1" in
        --top)
            [ $# -ge 2 ] || bad_usage "--top wants a number"
            top="$2"
            shift 2 ;;
        --by)
            [ $# -ge 2 ] || bad_usage "--by wants a column name"
            by="$2"
            shift 2 ;;
        *) bad_usage "unknown option $1" ;;
    esac
done
case "$top" in ''|*[!0-9]*) bad_usage "--top wants a whole number, not '$top'" ;; esac
case " total network resolve connect tls first_byte body requests reused bytes document stylesheets scripts images fonts xhr engine parse scripts_run sheets_read style images_phase layout paint " in
    *" $by "*) ;;
    *) bad_usage "unknown --by column: $by" ;;
esac

tsv="$input"
[ -d "$input" ] && tsv="$input/census.tsv"
if [ ! -f "$tsv" ]; then
    echo "no such file: $tsv" >&2
    exit 1
fi

awk -F'\t' -v top="$top" -v by="$by" -f /dev/stdin "$tsv" <<'AWK'
# Fixed order for "where the seconds go" (the network's own steps, then the
# network by what was fetched, then the engine's phases) and which six of
# those are "what was fetched" — used to find each loaded page's biggest.
BEGIN {
    FS = "\t"
    nsteps = split("network resolve connect tls first_byte body document stylesheets scripts images fonts xhr engine parse scripts_run sheets_read style images_phase layout paint", steps, " ")
    nfetched = split("document stylesheets scripts images fonts xhr", fetched, " ")
    for (i = 1; i <= nfetched; i++)
        is_fetched[fetched[i]] = 1
    nnum = split("total network resolve connect tls first_byte body requests reused bytes document stylesheets scripts images fonts xhr engine parse scripts_run sheets_read style images_phase layout paint", numcols, " ")
    top = top + 0
}

# Ascending insertion sort of a[1..n], in place. A Sashfold 100 run is a few
# hundred pages at most, so the O(n^2) cost of this never matters, and awk
# has no sort() to shell out to instead.
function sort_asc(a, n,    i, j, key) {
    for (i = 2; i <= n; i++) {
        key = a[i]
        j = i - 1
        while (j >= 1 && a[j] > key) {
            a[j + 1] = a[j]
            j--
        }
        a[j + 1] = key
    }
}
# Descending insertion sort of the row keys in idx[1..n] by keyval[key].
# Stable, so pages that tie on the ranked column keep the order they were
# read in.
function sort_desc_by(idx, keyval, n,    i, j, k, kv) {
    for (i = 2; i <= n; i++) {
        k = idx[i]
        kv = keyval[k]
        j = i - 1
        while (j >= 1 && keyval[idx[j]] < kv) {
            idx[j + 1] = idx[j]
            j--
        }
        idx[j + 1] = k
    }
}
# The upper of the two middle values when n is even, same as the C++ census
# in tools/sashfold100.cpp, so the two agree. Copies its input, so sorting
# for the median never reorders the caller's array.
function median(values, n,    tmp, i) {
    if (n == 0)
        return 0
    for (i = 1; i <= n; i++)
        tmp[i] = values[i]
    sort_asc(tmp, n)
    return tmp[int(n / 2) + 1]
}
function pct(part, whole) {
    return whole > 0 ? sprintf("%.1f%%", 100 * part / whole) : "-"
}
function fatal(msg) {
    print "perf-census.sh: " msg > "/dev/stderr"
    aborted = 1
    exit 1
}

NR == 1 {
    for (i = 1; i <= NF; i++)
        col[$i] = i
    if (!("id" in col) || !("outcome" in col) || !("total" in col) || !("url" in col))
        fatal("census.tsv's header is missing id, outcome, total or url")
    if (!(by in col))
        fatal("no such column: " by)
    next
}
NF == 0 { next }
{
    rows++
    r = rows
    row_id[r] = $(col["id"])
    row_outcome[r] = $(col["outcome"])
    row_url[r] = $(col["url"])
    if (row_outcome[r] == "loaded") {
        nloaded++
        loaded[nloaded] = r
        for (i = 1; i <= nnum; i++) {
            name = numcols[i]
            val[r, name] = (name in col) ? $(col[name]) + 0 : 0
        }
    } else {
        nbad++
        bad[nbad] = r
    }
}

END {
    if (aborted)
        exit 1

    # total isn't printed as a step of its own, but it is what every share
    # below is a share of, and the summary line wants its sum and median too.
    sum_total = 0
    for (p = 1; p <= nloaded; p++) {
        tmp_total[p] = val[loaded[p], "total"]
        sum_total += tmp_total[p]
    }
    median_total = median(tmp_total, nloaded)

    for (s = 1; s <= nsteps; s++) {
        name = steps[s]
        sum = 0
        delete tmp
        for (p = 1; p <= nloaded; p++) {
            tmp[p] = val[loaded[p], name]
            sum += tmp[p]
        }
        step_sum[name] = sum
        step_median[name] = median(tmp, nloaded)
        step_share[name] = pct(sum, sum_total)
    }

    # Each loaded page's biggest of the six "what was fetched" columns.
    for (p = 1; p <= nloaded; p++) {
        r = loaded[p]
        best = fetched[1]
        bestv = val[r, best]
        for (i = 2; i <= nfetched; i++) {
            v = val[r, fetched[i]]
            if (v > bestv) {
                bestv = v
                best = fetched[i]
            }
        }
        fetch_wins[best]++
    }

    printf "%d / %d pages loaded; total: sum %d ms, median %d ms; network: sum %d ms, median %d ms (%s of the total sum); engine: sum %d ms, median %d ms\n", \
        nloaded, rows, sum_total, median_total, step_sum["network"], step_median["network"], step_share["network"], step_sum["engine"], step_median["engine"]

    print ""
    print "Where the seconds go, over the loaded pages:"
    label_w = length("step")
    median_w = length("median ms")
    sum_w = length("sum ms")
    share_w = length("share")
    big_w = length("biggest")
    for (s = 1; s <= nsteps; s++) {
        name = steps[s]
        if (length(name) > label_w) label_w = length(name)
        if (length(step_median[name]) > median_w) median_w = length(step_median[name])
        if (length(step_sum[name]) > sum_w) sum_w = length(step_sum[name])
        if (length(step_share[name]) > share_w) share_w = length(step_share[name])
        if (is_fetched[name] && length(fetch_wins[name] + 0) > big_w) big_w = length(fetch_wins[name] + 0)
    }
    printf "%-*s %*s %*s %*s %*s\n", label_w, "step", median_w, "median ms", sum_w, "sum ms", share_w, "share", big_w, "biggest"
    for (s = 1; s <= nsteps; s++) {
        name = steps[s]
        biggest = is_fetched[name] ? fetch_wins[name] + 0 : ""
        printf "%-*s %*d %*d %*s %*s\n", label_w, name, median_w, step_median[name], sum_w, step_sum[name], share_w, step_share[name], big_w, biggest
    }

    # The pages ranked by --by, descending; only loaded pages are ranked.
    for (p = 1; p <= nloaded; p++) {
        rank[p] = loaded[p]
        keyval[loaded[p]] = val[loaded[p], by]
    }
    sort_desc_by(rank, keyval, nloaded)
    shown = (top < nloaded) ? top : nloaded

    print ""
    printf "The slowest pages, top %d of %d loaded by %s:\n", shown, nloaded, by
    id_w = length("id"); by_w = length(by); total_w = length("total")
    net_w = length("network"); eng_w = length("engine")
    req_w = length("requests"); bytes_w = length("bytes")
    for (i = 1; i <= shown; i++) {
        r = rank[i]
        if (length(row_id[r]) > id_w) id_w = length(row_id[r])
        if (length(val[r, by]) > by_w) by_w = length(val[r, by])
        if (length(val[r, "total"]) > total_w) total_w = length(val[r, "total"])
        if (length(val[r, "network"]) > net_w) net_w = length(val[r, "network"])
        if (length(val[r, "engine"]) > eng_w) eng_w = length(val[r, "engine"])
        if (length(val[r, "requests"]) > req_w) req_w = length(val[r, "requests"])
        if (length(val[r, "bytes"]) > bytes_w) bytes_w = length(val[r, "bytes"])
    }
    printf "%-*s %*s %*s %*s %*s %*s %*s  %s\n", id_w, "id", by_w, by, total_w, "total", net_w, "network", eng_w, "engine", req_w, "requests", bytes_w, "bytes", "url"
    for (i = 1; i <= shown; i++) {
        r = rank[i]
        printf "%-*s %*d %*d %*d %*d %*d %*d  %s\n", id_w, row_id[r], by_w, val[r, by], total_w, val[r, "total"], net_w, val[r, "network"], eng_w, val[r, "engine"], req_w, val[r, "requests"], bytes_w, val[r, "bytes"], row_url[r]
    }

    if (nbad > 0) {
        print ""
        print "Pages that did not load:"
        idb_w = length("id")
        for (i = 1; i <= nbad; i++)
            if (length(row_id[bad[i]]) > idb_w) idb_w = length(row_id[bad[i]])
        printf "%-*s  %s\n", idb_w, "id", "outcome"
        for (i = 1; i <= nbad; i++)
            printf "%-*s  %s\n", idb_w, row_id[bad[i]], row_outcome[bad[i]]
    }
}
AWK
