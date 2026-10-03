#!/usr/bin/env bash
# The script engine timed on the kernels of tools/kernels.js: one or more
# builds of js_probe, and V8 (node) with its JIT off and on when asked, run
# in turn once a round so that the machine's moods fall on every arm alike.
# The report is each kernel's median in milliseconds (at scale 1), each
# arm's geometric mean against the first arm, and any kernel whose answer
# differs between two js_probe builds — a fast path that computes wrongly
# is caught here as well as timed.
#
#   tools/kernels.sh [--rounds N] [--node] <js_probe> [<js_probe> ...]
#
# Exit status 1 when two js_probe builds disagree on an answer.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
rounds=5
with_node=0
probes=()
while [ $# -gt 0 ]; do
    case "$1" in
        --rounds) rounds="$2"; shift 2 ;;
        --node) with_node=1; shift ;;
        *) probes+=("$1"); shift ;;
    esac
done
[ ${#probes[@]} -gt 0 ] || { echo "usage: tools/kernels.sh [--rounds N] [--node] <js_probe> [<js_probe> ...]"; exit 2; }
source="$(cat "$here/kernels.js")"
results="$(mktemp)"
trap 'rm -f "$results"' EXIT
for round in $(seq 1 "$rounds"); do
    for i in "${!probes[@]}"; do
        line="$("${probes[$i]}" "$source; __out" --no-stress | sed 's/^ok //')"
        echo "probe$((i + 1)) $line" >> "$results"
    done
    if [ "$with_node" -eq 1 ] && command -v node > /dev/null; then
        echo "v8-nojit $(node --jitless -e "var __scale=10; $source; console.log(__out)")" >> "$results"
        echo "v8-full $(node -e "var __scale=20; $source; console.log(__out)")" >> "$results"
    fi
done
node - "$results" "${probes[@]}" <<'NODE'
const fs = require('fs');
const [file, ...probes] = process.argv.slice(2);
const times = {}, answers = {};
for (const line of fs.readFileSync(file, 'utf8').trim().split('\n')) {
    const [arm, ...pairs] = line.trim().split(/\s+/);
    for (const pair of pairs) {
        const [kernel, rest] = pair.split('=');
        const [ms, answer] = rest.split('/');
        ((times[arm] ??= {})[kernel] ??= []).push(Number(ms));
        if (arm.startsWith('probe')) ((answers[kernel] ??= {})[arm] ??= new Set()).add(answer);
    }
}
const median = list => { const s = [...list].sort((a, b) => a - b); return s[Math.floor(s.length / 2)]; };
const arms = Object.keys(times), kernels = Object.keys(times[arms[0]]);
probes.forEach((p, i) => console.log(`probe${i + 1} = ${p}`));
console.log('kernel'.padEnd(10) + arms.map(a => a.padStart(11)).join(''));
for (const k of kernels) console.log(k.padEnd(10) + arms.map(a => median(times[a][k]).toFixed(1).padStart(11)).join(''));
const geo = arm => Math.exp(kernels.reduce((s, k) => s + Math.log(Math.max(median(times[arm][k]), 0.05) / Math.max(median(times[arms[0]][k]), 0.05)), 0) / kernels.length);
console.log('vs first'.padEnd(10) + arms.map(a => (geo(a).toFixed(2) + 'x').padStart(11)).join(''));
let disagree = 0;
for (const k of kernels) {
    const seen = new Set(Object.values(answers[k] || {}).flatMap(set => [...set]));
    if (seen.size > 1) { disagree++; console.log(`ANSWERS DIFFER: ${k}: ${JSON.stringify(Object.fromEntries(Object.entries(answers[k]).map(([a, s]) => [a, [...s]])))}`); }
}
process.exit(disagree ? 1 : 0);
NODE
