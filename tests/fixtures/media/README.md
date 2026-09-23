# Media fixtures

Synthetic streams, made with ffmpeg's test sources (no recorded content):

    ffmpeg -f lavfi -i "testsrc2=size=64x48:rate=10:duration=1" -c:v libvpx-vp9 -b:v 20k -g 5 -dash 1 tiny-vp9.webm
    ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" -c:a libopus -b:a 16k -dash 1 tiny-opus.webm
    ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" -c:a libopus -b:a 32k -application lowdelay -dash 1 tiny-opus-celt.webm

At 16 kbit/s the encoder moves between its speech and music layers, so
tiny-opus.webm holds SILK, hybrid and CELT frames; `-application lowdelay`
keeps tiny-opus-celt.webm to CELT alone.

Each `.packets` file is what another reader found in the stream, one packet
a line as `pts_ms,duration_ms,size,flags`:

    ffprobe -show_entries packet=pts,duration,flags,size -of csv=p=0 <file>

ffprobe takes the Opus codec delay (6.5 ms) off the audio times; the blocks
in the file begin at 0.
