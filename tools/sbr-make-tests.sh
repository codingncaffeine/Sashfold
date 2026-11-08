#!/usr/bin/env bash
# Writes the HE-AAC fixtures test_sbr reads: half a second of tones and pink
# noise encoded with spectral band replication by the Fraunhofer FDK AAC
# encoder (tools/sbr-encode.c, an encoder independent of ours), put in MP4
# by ffmpeg (which signals SBR implicitly, as most files do), and beside each
# ffmpeg's decoding of it as 32-bit float samples ("<name>.f32", interleaved,
# at twice the core's rate) — the truth, since ffmpeg's decoder matches the
# ISO/IEC 14496-26 reference decodings of the SBR conformance streams.
#   tools/sbr-make-tests.sh <out-folder>
set -euo pipefail
out="${1:?the out folder}"
mkdir -p "$out"
command -v ffmpeg >/dev/null || { echo "ffmpeg is needed" >&2; exit 2; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cc -O2 -o "$work/sbr-encode" "$(dirname "$0")/sbr-encode.c" -lfdk-aac || { echo "libfdk-aac is needed" >&2; exit 2; }

make() {
    local name="$1" rate="$2" channels="$3" bitrate="$4"; shift 4
    ffmpeg -hide_banner -loglevel error -y "$@" -ar "$rate" -ac "$channels" -f s16le "$work/$name.s16"
    "$work/sbr-encode" "$work/$name.s16" "$rate" "$channels" "$bitrate" "$work/$name.aac"
    ffmpeg -hide_banner -loglevel error -y -i "$work/$name.aac" -c copy "$out/$name.m4a"
    # ffmpeg answers a mono HE-AAC stream in stereo (parametric stereo may
    # follow) with the two channels the same: the first is kept as it is
    # (-ac 1 would mix them down 3 dB).
    if [ "$channels" = 1 ]; then
        ffmpeg -hide_banner -loglevel error -y -i "$out/$name.m4a" -af "pan=mono|c0=c0" -f f32le "$out/$name.f32"
    else
        ffmpeg -hide_banner -loglevel error -y -i "$out/$name.m4a" -f f32le "$out/$name.f32"
    fi
}

tone=(-f lavfi -i "sine=frequency=523:sample_rate=48000:duration=0.5")
noise=(-f lavfi -i "anoisesrc=color=pink:sample_rate=48000:duration=0.5:amplitude=0.3:seed=11")
make he-stereo-44k 44100 2 48000 "${tone[@]}" "${noise[@]}" -filter_complex "[0:a][1:a]amerge=inputs=2,pan=stereo|c0=c0+0.4*c1|c1=0.6*c0+c1[a]" -map "[a]"
make he-mono-48k 48000 1 32000 "${tone[@]}" "${noise[@]}" -filter_complex "[0:a][1:a]amerge=inputs=2,pan=mono|c0=c0+0.5*c1[a]" -map "[a]"
ls -la "$out"
