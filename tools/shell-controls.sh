#!/bin/bash
# Planted-fault controls for the shell's scripts: proof that a script's
# assertions can see the faults they were written against.
#
# A test that passes says the code is right only if it would have failed
# were the code wrong. A .controls file beside a script (tests/shell/
# <name>.controls) lists faults to plant in the source, one at a time, and
# for each the script lines that must then fail — those and no others:
#
#   script tests/shell/frame.script
#   args --theme tests/shell/frame-theme.json
#
#   control ring
#   file src/core/Bitmap.cpp
#   fails 68 69 111
#   plant s~the text as it stands~the text with the fault~
#
# `plant` is a perl substitution over the whole file (\n between lines). It
# must land exactly once. A control MATCHES when the planted tree builds,
# the script fails, and the lines that failed are exactly `fails`. Anything
# else — the plant landing nowhere or twice, the build breaking, the script
# passing, other lines failing — is reported as what it is, and the run
# fails. The source is put back from a copy after every control, on any
# exit, and at the start of a run that finds a copy left by one that was
# killed; at the end it is compared byte for byte and the tree rebuilt.
#
#   tools/shell-controls.sh [--build-dir build-gcc] [--self-test] [file.controls ...]
#
# With no files, every tests/shell/*.controls. --self-test proves the runner
# itself on the first control it is given: it must MATCH with its own
# prediction, MISMATCH with a wrong one, and say so when a plant lands nowhere.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build="build-gcc"
self_test=0
files=()
while [ $# -gt 0 ]; do
    case "$1" in
    --build-dir) build=$2; shift 2 ;;
    --self-test) self_test=1; shift ;;
    *) files+=("$1"); shift ;;
    esac
done
[ ${#files[@]} -eq 0 ] && files=("$root"/tests/shell/*.controls)
binary="$root/$build/sashfold"
keep="$root/$build/controls-backup"
logs="$root/$build/controls-logs"
mkdir -p "$logs" "$root/$build/test-downloads"

# Put back whatever a copy is held of, and drop the copy.
restore() {
    [ -d "$keep" ] || return 0
    (cd "$keep" && find . -type f -print) | while read -r held; do
        cmp -s "$keep/$held" "$root/$held" || cp "$keep/$held" "$root/$held"
    done
}
forget() { [ -d "$keep" ] && find "$keep" -type f -delete && find "$keep" -depth -type d -empty -delete; return 0; }
if [ -d "$keep" ] && [ -n "$(find "$keep" -type f -print -quit)" ]; then
    echo "a copy of the source from a run that did not finish: putting it back first"
    restore
    forget
fi
trap 'restore' EXIT

hold() { # keep a copy of a source file before its first plant
    local file=$1
    [ -f "$keep/$file" ] && return 0
    mkdir -p "$keep/$(dirname "$file")" && cp "$root/$file" "$keep/$file"
}

matched=0
total=0
touched=()

# run_control <script> <args> <name> <file> <predicted> <plant> → prints one line, returns 0 on MATCH
run_control() {
    local script=$1 args=$2 name=$3 file=$4 predicted=$5 plant=$6
    restore
    hold "$file"
    local known=0
    for one in "${touched[@]:-}"; do [ "$one" = "$file" ] && known=1; done
    [ $known -eq 0 ] && touched+=("$file")
    local count
    count=$(PLANT="$plant" perl -0pi -e 'our $c = eval "\$_ =~ $ENV{PLANT}"; die $@ if $@; $c ||= 0; END { print STDERR $c }' "$root/$file" 2>&1 >/dev/null)
    if [ "$count" != "1" ]; then echo "CONTROL $name: the plant did not land once (count=$count) in $file"; return 1; fi
    if cmp -s "$root/$file" "$keep/$file"; then echo "CONTROL $name: the source is unchanged"; return 1; fi
    if ! cmake --build "$root/$build" --target sashfold > "$logs/$name.build.log" 2>&1; then
        echo "CONTROL $name: the planted tree did not BUILD ($logs/$name.build.log)"; return 1
    fi
    # Run from the logs folder, with the script and what its arguments name
    # made absolute: a golden that fails writes the frame it got beside
    # where it runs, and that is not to be the source tree.
    local there_args=${args//tests\//$root/tests/}
    # shellcheck disable=SC2086
    (cd "$logs" && "$binary" --script "$root/$script" $there_args --downloads "$root/$build/test-downloads") > "$logs/$name.run.log" 2>&1
    local rc=$?
    local got
    got=$(grep -o "^FAIL line [0-9]*" "$logs/$name.run.log" | awk '{print $3}' | tr '\n' ' ' | sed 's/ $//')
    if [ $rc -eq 0 ]; then echo "CONTROL $name: the script PASSED with the fault planted"; return 1; fi
    if [ "$got" = "$predicted" ]; then echo "CONTROL $name: MATCH (failed lines: $got)"; return 0; fi
    echo "CONTROL $name: MISMATCH predicted [$predicted] got [$got]"
    return 1
}

# Reads a .controls file into parallel arrays.
names=(); sources=(); predictions=(); plants=(); scripts=(); arguments=()
read_controls() {
    local path=$1 script="" args="" name="" file="" fails="" plant=""
    flush() {
        [ -z "$name" ] && return 0
        if [ -z "$script" ] || [ -z "$file" ] || [ -z "$plant" ]; then echo "$path: control $name lacks a script, a file or a plant"; exit 2; fi
        names+=("$name"); sources+=("$file"); predictions+=("$fails"); plants+=("$plant"); scripts+=("$script"); arguments+=("$args")
        name=""; file=""; fails=""; plant=""
    }
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
        "#"*|"") ;;
        "script "*) script=${line#script } ;;
        "args "*) args=${line#args } ;;
        "control "*) flush; name=${line#control } ;;
        "file "*) file=${line#file } ;;
        "fails "*) fails=${line#fails } ;;
        "plant "*) plant=${line#plant } ;;
        *) echo "$path: not understood: $line"; exit 2 ;;
        esac
    done < "$path"
    flush
}
for path in "${files[@]}"; do read_controls "$path"; done
if [ ${#names[@]} -eq 0 ]; then echo "no controls to run"; exit 2; fi

if [ $self_test -eq 1 ]; then
    ok=1
    run_control "${scripts[0]}" "${arguments[0]}" "selftest-as-written" "${sources[0]}" "${predictions[0]}" "${plants[0]}" || ok=0
    out=$(run_control "${scripts[0]}" "${arguments[0]}" "selftest-wrong-prediction" "${sources[0]}" "999999" "${plants[0]}"); echo "$out"
    case "$out" in *MISMATCH*) ;; *) ok=0 ;; esac
    out=$(run_control "${scripts[0]}" "${arguments[0]}" "selftest-no-such-text" "${sources[0]}" "1" 's~THIS TEXT IS NOWHERE IN THE SOURCE~x~'); echo "$out"
    case "$out" in *"did not land"*) ;; *) ok=0 ;; esac
else
    for i in "${!names[@]}"; do
        total=$((total + 1))
        run_control "${scripts[$i]}" "${arguments[$i]}" "${names[$i]}" "${sources[$i]}" "${predictions[$i]}" "${plants[$i]}" && matched=$((matched + 1))
    done
fi

restore
trap - EXIT
clean=1
for file in "${touched[@]:-}"; do
    [ -z "$file" ] && continue
    cmp -s "$root/$file" "$keep/$file" || { echo "NOT RESTORED: $file"; clean=0; }
done
[ $clean -eq 1 ] && forget
cmake --build "$root/$build" --target sashfold > "$logs/restore.build.log" 2>&1 || { echo "the restored tree did not build"; clean=0; }
if [ $self_test -eq 1 ]; then
    [ $ok -eq 1 ] && [ $clean -eq 1 ] && echo "RUNNER SELF-TEST: ok (a match, a mismatch and a plant that landed nowhere each told apart; source restored)" && exit 0
    echo "RUNNER SELF-TEST: FAILED"; exit 1
fi
echo "CONTROLS: $matched of $total matched; source $([ $clean -eq 1 ] && echo "restored byte for byte" || echo "NOT RESTORED")"
[ $matched -eq $total ] && [ $clean -eq 1 ]
