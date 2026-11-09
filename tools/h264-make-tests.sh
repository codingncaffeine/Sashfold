#!/usr/bin/env bash
# Writes the H.264 fixtures test_h264 decodes beside the MP4 ones: short
# clips from x264 (through ffmpeg) that between them use every tool of
# the profiles the web serves — CAVLC and CABAC, the 8x8 transform with
# each coder, every partition down to 4x4, spatial and temporal direct,
# several references and B-pyramids, multiple slices, scaling matrices,
# the deblocking filter's offsets and its absence, constrained intra
# prediction, a size that is not a whole number of macroblocks (cropping),
# intra-only pictures, and motion that reaches past the picture's edges.
# Beside each "<name>.mp4", "<name>.pictures": the SHA-256 of each picture
# as ffmpeg decodes it (Y, Cb, Cr planes), in presentation order.
#   tools/h264-make-tests.sh <out-folder>
set -euo pipefail
out="${1:?the out folder}"
mkdir -p "$out"
command -v ffmpeg >/dev/null || { echo "ffmpeg is needed" >&2; exit 2; }

make() {
    local name="$1" size="$2"; shift 2
    local w="${size%x*}" h="${size#*x}"
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=$size:rate=24:duration=1" "$@" -an -pix_fmt yuv420p "$out/$name.mp4"
    ffmpeg -hide_banner -loglevel error -i "$out/$name.mp4" -an -fps_mode passthrough -f rawvideo -pix_fmt yuv420p - \
        | python3 -I -c '
import hashlib, sys
w, h = int(sys.argv[1]), int(sys.argv[2])
size = w * h + 2 * ((w + 1) // 2) * ((h + 1) // 2)
data = sys.stdin.buffer.read()
for i in range(len(data) // size):
    print(hashlib.sha256(data[i * size:(i + 1) * size]).hexdigest())
' "$w" "$h" > "$out/$name.pictures"
}

x264=(-c:v libx264 -crf 30 -g 12)
make cavlc-baseline 176x96 "${x264[@]}" -profile:v baseline -x264-params "ref=3"
make cavlc-high 176x96 "${x264[@]}" -bf 2 -x264-params "cabac=0:8x8dct=1:ref=2"
make temporal-direct 176x96 "${x264[@]}" -bf 3 -x264-params "direct=temporal:b-pyramid=none:ref=2"
make temporal-direct-cavlc 176x96 "${x264[@]}" -bf 2 -x264-params "direct=temporal:cabac=0"
make partitions-all 176x96 "${x264[@]}" -bf 2 -x264-params "partitions=all:subme=7:ref=3:8x8dct=0"
make partitions-all-8x8 176x96 "${x264[@]}" -bf 2 -x264-params "partitions=all:subme=7:ref=3:direct=auto"
make slices 176x96 "${x264[@]}" -bf 2 -x264-params "slices=4"
make scaling-matrix 176x96 "${x264[@]}" -bf 2 -x264-params "cqm=jvt"
make deblock-offsets 176x96 "${x264[@]}" -bf 2 -x264-params "deblock=-3,2"
make no-deblock 176x96 "${x264[@]}" -bf 2 -x264-params "no-deblock=1"
make constrained-intra 176x96 "${x264[@]}" -bf 0 -x264-params "constrained-intra=1"
make cropped 170x98 "${x264[@]}" -bf 2
make intra-only 176x96 -c:v libx264 -crf 24 -g 1 -x264-params "keyint=1"
make edges 176x96 "${x264[@]}" -bf 2 -vf "scroll=h=0.05:v=0.03" -x264-params "me=umh:merange=32"
make low-qp 176x96 -c:v libx264 -crf 6 -g 12 -bf 2
ls -la "$out"
