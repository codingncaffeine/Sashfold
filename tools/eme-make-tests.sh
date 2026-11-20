#!/usr/bin/env bash
# Writes the encrypted media fixtures test_eme reads, with ffmpeg as the
# independent writer and reader. One encode of a test picture (H.264 with
# B-frames) and a tone (AAC), copied without decoding into a clear file and
# a Common Encryption 'cenc' file (ffmpeg's muxer encrypts), fragmented as
# MSE sends it and plain; and a picture-only 'cbcs' file that
# tools/cenc-to-cbcs.py makes from the 'cenc' one with the openssl command.
# Beside each clear file, its packets as ffmpeg reads them: "stream size
# sha256" per line. Each encrypted file is checked here first: ffmpeg's
# own decryption of it must give exactly the clear file's packets.
#   tools/eme-make-tests.sh <out-folder>
set -euo pipefail
out="${1:?the out folder}"
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$out"
command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null && command -v openssl >/dev/null \
    || { echo "ffmpeg, ffprobe and openssl are needed" >&2; exit 2; }
work="$(mktemp -d "$out/.work.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# The key and its ID the tests license (Clear Key), and the 'cbcs' file's
# constant IV.
key=3a2f5b4c6d7e8f901a2b3c4d5e6f7081
kid=0f1e2d3c4b5a69788796a5b4c3d2e1f0
iv=00112233445566778899aabbccddeeff

ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=160x120:rate=24:duration=1" \
    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=1" \
    -c:v libx264 -preset veryfast -crf 35 -g 12 -bf 2 -pix_fmt yuv420p -c:a aac -b:a 32k "$work/source.mp4"

encrypt=(-encryption_scheme cenc-aes-ctr -encryption_key "$key" -encryption_kid "$kid")
# Sound and picture in one fragment: ffmpeg's muxer writes the second
# track's 'senc' of a later fragment with every sample since the first
# (45 entries for a run of 26, the first fragment's leading), which its own
# demuxer and ours both read as the 23001-7 box it should be — so sound and
# picture over several fragments come one track a file.
single=(-movflags empty_moov+default_base_moof -frag_duration 5000000)
fragmented=(-movflags frag_keyframe+empty_moov+default_base_moof)
copy() { ffmpeg -hide_banner -loglevel error -y -i "$work/source.mp4" -c copy "$@"; }
copy "${single[@]}" "$out/av-clear-frag.mp4"
copy "${single[@]}" "${encrypt[@]}" "$out/av-cenc-frag.mp4"
copy -movflags +faststart "$out/av-clear-plain.mp4"
copy -movflags +faststart "${encrypt[@]}" "$out/av-cenc-plain.mp4"
copy -vn -movflags empty_moov+default_base_moof -frag_duration 300000 "$out/a-clear-frag.mp4"
copy -vn -movflags empty_moov+default_base_moof -frag_duration 300000 "${encrypt[@]}" "$out/a-cenc-frag.mp4"
copy -an "${fragmented[@]}" "$out/v-clear-frag.mp4"
copy -an "${fragmented[@]}" "${encrypt[@]}" "$out/v-cenc-frag.mp4"
python3 -I "$here/cenc-to-cbcs.py" "$out/v-cenc-frag.mp4" "$out/v-clear-frag.mp4" "$key" "$iv" "$out/v-cbcs-frag.mp4"

# WebM: VP9 and Opus, encrypted by tools/webm-encrypt.py (openssl again):
# whole frames, partitioned ones and frames left clear.
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=64x48:rate=10:duration=1" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" \
    -c:v libvpx-vp9 -b:v 20k -g 5 -c:a libopus -b:a 16k "$out/vp9-opus-clear.webm"
python3 -I "$here/webm-encrypt.py" "$out/vp9-opus-clear.webm" "$key" "$kid" "$out/vp9-opus-enc.webm"

packets() {
    ffprobe -hide_banner -loglevel error "$@" -show_packets -show_data_hash sha256 -of compact=p=0:nk=1 \
        -show_entries packet=stream_index,size,data_hash | awk -F'|' '{ sub("SHA256:", "", $3); print $1, $2, $3 }'
}
for name in av-clear-frag av-clear-plain a-clear-frag v-clear-frag; do
    packets "$out/$name.mp4" > "$out/$name.packets"
done
packets "$out/vp9-opus-clear.webm" > "$out/vp9-opus-clear.packets"
same() {
    if ! cmp -s <(packets -decryption_key "$key" "$out/$1.mp4") "$out/$2.packets"; then
        echo "ffmpeg does not decrypt $1.mp4 to $2's packets" >&2
        exit 1
    fi
    echo "$1.mp4: ffmpeg decrypts it to $2's packets"
}
same av-cenc-frag av-clear-frag
same av-cenc-plain av-clear-plain
same a-cenc-frag a-clear-frag
same v-cenc-frag v-clear-frag
same v-cbcs-frag v-clear-frag
ls -la "$out"
