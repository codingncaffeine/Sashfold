# Media fixtures

Synthetic streams, made with ffmpeg's test sources (no recorded content):

    ffmpeg -f lavfi -i "testsrc2=size=64x48:rate=10:duration=1" -c:v libvpx-vp9 -b:v 20k -g 5 -dash 1 tiny-vp9.webm
    ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" -c:a libopus -b:a 16k -dash 1 tiny-opus.webm
    ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" -c:a libopus -b:a 32k -application lowdelay -dash 1 tiny-opus-celt.webm

vp9-144p.webm is the hardware decoders' stream, at YouTube's smallest size
(the GPU decoders do not take pictures under 128x128). Two passes give it
hidden alternate references read backwards; variance quantization gives it
segmentation:

    for pass in 1 2; do ffmpeg -f lavfi -i testsrc2=size=256x144:rate=30 -frames:v 30 -c:v libvpx-vp9 \
        -deadline good -cpu-used 2 -b:v 120k -g 15 -auto-alt-ref 1 -lag-in-frames 25 -aq-mode 1 \
        -pass $pass -an $([ $pass = 1 ] && echo '-f null /dev/null' || echo vp9-144p.webm); done

The SHA-256 of each picture in tests/test_vp9.cpp is ffmpeg's decoding of it,
planar, a frame at a time (its own decoder and libvpx's agree byte for byte):

    ffmpeg -i vp9-144p.webm -f rawvideo -pix_fmt yuv420p - | split -b 55296 -d - frame && sha256sum frame*

At 16 kbit/s the encoder moves between its speech and music layers, so
tiny-opus.webm holds SILK, hybrid and CELT frames; `-application lowdelay`
keeps tiny-opus-celt.webm to CELT alone.

Each `.packets` file is what another reader found in the stream, one packet
a line as `pts_ms,duration_ms,size,flags`:

    ffprobe -show_entries packet=pts,duration,flags,size -of csv=p=0 <file>

ffprobe takes the Opus codec delay (6.5 ms) off the audio times; the blocks
in the file begin at 0.
