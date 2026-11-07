#!/usr/bin/env bash
# Writes the MP4 fixtures test_mp4 reads, with ffmpeg as the independent
# writer and ffprobe as the truth: a second of a test picture (H.264, with
# B-frames so composition offsets and an edit list appear) and a tone
# (AAC), as a plain file with the tables first and last, as the fragmented
# files MSE and DASH send (runs by moof, and by absolute offsets), and as
# sound or picture alone. Beside each file, its packets as ffprobe lists
# them: "stream pts_time dts_time duration_time size key" per line.
#   tools/mp4-make-tests.sh <out-folder>
set -euo pipefail
out="${1:?the out folder}"
mkdir -p "$out"
command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null || { echo "ffmpeg and ffprobe are needed" >&2; exit 2; }

video=(-f lavfi -i "testsrc2=size=160x120:rate=24:duration=1")
audio=(-f lavfi -i "sine=frequency=440:sample_rate=44100:duration=1")
codecs=(-c:v libx264 -preset veryfast -crf 35 -g 12 -bf 2 -pix_fmt yuv420p -c:a aac -b:a 32k)

make() {
    local name="$1"; shift
    ffmpeg -hide_banner -loglevel error -y "$@" "$out/$name.mp4"
    ffprobe -hide_banner -loglevel error -show_packets -of compact=p=0:nk=1 \
        -show_entries packet=stream_index,pts_time,dts_time,duration_time,size,flags "$out/$name.mp4" \
        | awk -F'|' '{ key = (substr($6, 1, 1) == "K") ? 1 : 0; print $1, $2, $3, $4, $5, key }' > "$out/$name.packets"
    # The picture type of each video frame by its presentation time, as the
    # decoder names them: "pts_time pict_type".
    if ffprobe -hide_banner -loglevel error -select_streams v -show_streams "$out/$name.mp4" | grep -q codec_type=video; then
        ffprobe -hide_banner -loglevel error -select_streams v -show_frames -of compact=p=0:nk=1 \
            -show_entries frame=pts_time,pict_type "$out/$name.mp4" | awk -F'|' '{ print $1, $2 }' > "$out/$name.frames"
        # Each picture as ffmpeg decodes it, in presentation order: the SHA-256
        # of its planes (Y, then Cb, then Cr), one a line. H.264 decoding is
        # exact by the standard, so the hardware must give the same bytes.
        ffmpeg -hide_banner -loglevel error -i "$out/$name.mp4" -an -fps_mode passthrough -f rawvideo -pix_fmt yuv420p - \
            | python3 -I -c '
import hashlib, sys
w, h = int(sys.argv[1]), int(sys.argv[2])
size = w * h * 3 // 2
data = sys.stdin.buffer.read()
for i in range(len(data) // size):
    print(hashlib.sha256(data[i * size:(i + 1) * size]).hexdigest())
' 160 120 > "$out/$name.pictures"
    fi
}

make plain-faststart "${video[@]}" "${audio[@]}" "${codecs[@]}" -movflags +faststart
make plain-moov-last "${video[@]}" "${audio[@]}" "${codecs[@]}"
make fragmented "${video[@]}" "${audio[@]}" "${codecs[@]}" -movflags frag_keyframe+empty_moov+default_base_moof
make fragmented-absolute "${video[@]}" "${audio[@]}" "${codecs[@]}" -movflags frag_keyframe+empty_moov
make audio-only "${audio[@]}" -c:a aac -b:a 32k
make video-only "${video[@]}" -c:v libx264 -preset veryfast -crf 35 -g 12 -bf 0 -pix_fmt yuv420p
# A fade with several references, B-pyramids and explicit weights: slices
# with reference list modifications and prediction weight tables, which the
# header parser must read exactly for the decoders that are handed them.
make weighted -f lavfi -i "testsrc2=size=160x120:rate=24:duration=2,fade=t=in:st=0:d=1" -c:v libx264 -preset medium -crf 30 -g 24 -bf 3 \
    -x264-params "weightp=2:b-pyramid=normal:ref=4:mixed-refs=1:weightb=1" -pix_fmt yuv420p
ls -la "$out"
