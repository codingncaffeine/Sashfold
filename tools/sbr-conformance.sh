#!/usr/bin/env bash
# The SBR decoder against the ISO/IEC 14496-26 conformance streams for
# HE-AAC (AAC-LC with spectral band replication, MP4): each stream decoded
# by media_probe --native and compared, channel by channel, with the
# reference decoder's output ("hq", the complex QMF decoder). The streams
# and references are public at standards.iso.org; they are fetched once into
# <folder> (outside the repository: they are ISO's) and kept there.
#   tools/sbr-conformance.sh <media_probe> <folder> [name-filter]
# Prints one line a stream; exits 1 when one falls under 100 dB.
set -uo pipefail
probe="${1:?the media_probe binary}"
folder="${2:?a folder for the streams}"
filter="${3:-}"
base="https://standards.iso.org/ittf/PubliclyAvailableStandards/ISO_IEC_14496-26_2010_Bitstreams/DVD1/mpeg4audio-conformance"
mkdir -p "$folder"
names=()
for t in e gh i s; do for r in 32 44 48; do for c in 1 2; do names+=("${t}_${r}_${c}"); done; done; done
names+=(qmf_32_1 qmf_44_1 qmf_48_1 cm_48_1 cm_48_2 i_32_1_new i_44_1_new i_48_1_new
    sig_48_2_sig0 sig_48_2_sig1 sig_48_2_sig2 sig_24_2_fsaac24_sig1 twi_22_1_fsaac22 twi_48_1_fsaac24
    sr_16_2_fsaac08 sr_16_2_fsaac16 sr_22_2_fsaac11 sr_22_2_fsaac22 sr_24_2_fsaac12 sr_24_2_fsaac24
    sr_32_2_fsaac16 sr_32_2_fsaac32 sr_44_2_fsaac22 sr_44_2_fsaac44 sr_48_2_fsaac24 sr_48_2_fsaac48
    sr_64_2_fsaac32 sr_88_2_fsaac44 sr_96_2_fsaac48)
status=0
fetched=()
for n in "${names[@]}"; do
    [[ -n "$filter" && "$n" != *"$filter"* ]] && continue
    stream="$folder/al_sbr_$n.mp4"
    truth="$folder/al_sbr_$n.ref.wav"
    [ -s "$stream" ] || curl -sSf -m 120 -A "Mozilla/5.0" -o "$stream" "$base/compressedMp4/al_sbr_$n.mp4" || { echo "$n: cannot fetch"; status=1; continue; }
    [ -s "$truth" ] || curl -sSf -m 120 -A "Mozilla/5.0" -o "$truth" "$base/referencesWav/add-opt/al_sbr_hq_$n.wav" || { echo "$n: cannot fetch the reference"; status=1; continue; }
    fetched+=("$n")
done
# The streams decode side by side, a quarter of the machine's cores at a
# time (SBR_CONFORMANCE_JOBS to say otherwise); the lines print in order.
jobs_max="${SBR_CONFORMANCE_JOBS:-$(( $(nproc) / 4 > 0 ? $(nproc) / 4 : 1 ))}"
lines="$(mktemp -d)"
trap 'rm -rf "$lines"' EXIT
running=0
for n in "${fetched[@]}"; do
    ("$probe" --native "$folder/al_sbr_$n.mp4" --wav "$folder/al_sbr_$n.ref.wav" 2>&1 | grep -o "SNR [-0-9.]* dB at a lag of [-0-9]*" > "$lines/$n") &
    running=$((running + 1))
    if [ "$running" -ge "$jobs_max" ]; then
        wait -n
        running=$((running - 1))
    fi
done
wait
for n in "${fetched[@]}"; do
    line="$(cat "$lines/$n")"
    snr=$(echo "$line" | awk '{print $2}')
    floor=100
    verdict=ok
    if [ -z "$snr" ] || awk -v s="$snr" -v f="$floor" 'BEGIN { exit !(s < f) }'; then verdict=LOW; status=1; fi
    printf "%-26s %-34s %s\n" "$n" "${line:-no decoding}" "$verdict"
done
exit $status
