#!/usr/bin/env bash
# Writes the AAC fixtures test_aac reads, with ffmpeg's encoder as the
# independent writer and its decoder as the truth: half a second of a tone
# and pink noise as AAC-LC in MP4, in stereo with mid/side, intensity
# stereo and TNS switched on, and in mono; beside each, ffmpeg's decoding
# of it as 16-bit samples ("<name>.s16", interleaved). Perceptual noise
# substitution is off: its noise is the decoder's own, never the same twice.
#   tools/aac-make-tests.sh <out-folder>
set -euo pipefail
out="${1:?the out folder}"
mkdir -p "$out"
command -v ffmpeg >/dev/null || { echo "ffmpeg is needed" >&2; exit 2; }

make() {
    local name="$1"; shift
    ffmpeg -hide_banner -loglevel error -y "$@" "$out/$name.m4a"
    ffmpeg -hide_banner -loglevel error -y -i "$out/$name.m4a" -f s16le "$out/$name.s16"
}

tone=(-f lavfi -i "sine=frequency=440:sample_rate=44100:duration=0.5")
noise=(-f lavfi -i "anoisesrc=color=pink:sample_rate=44100:duration=0.5:amplitude=0.3:seed=7")
make stereo "${tone[@]}" "${noise[@]}" -filter_complex "[0:a][1:a]amerge=inputs=2,pan=stereo|c0=c0+0.3*c1|c1=0.5*c0+c1[a]" -map "[a]" \
    -c:a aac -b:a 128k -aac_pns 0 -aac_tns 1 -aac_ms 1 -aac_is 1
make mono "${tone[@]}" "${noise[@]}" -filter_complex "[0:a][1:a]amerge=inputs=2,pan=mono|c0=c0+0.5*c1[a]" -map "[a]" \
    -c:a aac -b:a 64k -aac_pns 0 -aac_tns 1
make stereo-48k "${tone[@]}" "${noise[@]}" -filter_complex "[0:a][1:a]amerge=inputs=2,pan=stereo|c0=c0+0.3*c1|c1=0.5*c0+c1,aresample=48000[a]" -map "[a]" \
    -c:a aac -b:a 96k -aac_pns 0 -aac_tns 1 -aac_ms 1
ls -la "$out"
