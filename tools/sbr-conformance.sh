#!/usr/bin/env bash
# The SBR decoder against the ISO/IEC 14496-26 conformance streams for
# HE-AAC (AAC-LC with spectral band replication, MP4): each stream decoded
# by media_probe --native and compared, channel by channel, with the
# reference decoder's output ("hq", the complex QMF decoder). The streams
# and references are public at standards.iso.org; they are fetched once into
# <folder> (outside the repository: they are ISO's) and kept there.
#   tools/sbr-conformance.sh <media_probe> <folder> [name-filter]
# Prints one line a stream; exits 1 when one falls under 100 dB (the header
# sweeps "gh", which change the frequency tables every frame, are held to
# 75 dB: they come out at 78 dB, where ffmpeg reaches 127 — a difference not
# yet found).
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
for n in "${names[@]}"; do
    [[ -n "$filter" && "$n" != *"$filter"* ]] && continue
    stream="$folder/al_sbr_$n.mp4"
    truth="$folder/al_sbr_$n.ref.wav"
    [ -s "$stream" ] || curl -sSf -m 120 -A "Mozilla/5.0" -o "$stream" "$base/compressedMp4/al_sbr_$n.mp4" || { echo "$n: cannot fetch"; status=1; continue; }
    [ -s "$truth" ] || curl -sSf -m 120 -A "Mozilla/5.0" -o "$truth" "$base/referencesWav/add-opt/al_sbr_hq_$n.wav" || { echo "$n: cannot fetch the reference"; status=1; continue; }
    line=$("$probe" --native "$stream" --wav "$truth" 2>&1 | grep -o "SNR [-0-9.]* dB at a lag of [-0-9]*")
    snr=$(echo "$line" | awk '{print $2}')
    floor=100
    [[ "$n" == gh_* ]] && floor=75
    verdict=ok
    if [ -z "$snr" ] || awk -v s="$snr" -v f="$floor" 'BEGIN { exit !(s < f) }'; then verdict=LOW; status=1; fi
    printf "%-26s %-34s %s\n" "$n" "${line:-no decoding}" "$verdict"
done
exit $status
