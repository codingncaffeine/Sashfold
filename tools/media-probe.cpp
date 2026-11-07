// media_probe: an MP4 (or WebM) file's tracks as the engine reads them,
// and its sound through the element's own path — the track's decoder, the
// mix-down to stereo and the resampler to 48 kHz — written out as 16-bit
// samples, or compared with another program's decoding of the same file.
//
//   media_probe <file>                          the tracks and their frames
//   media_probe --sound <file> --out <s16>      stereo 48 kHz samples, interleaved
//   media_probe --sound <file> --truth <s16>    the SNR against ffmpeg's
//                                               (ffmpeg -i f -ar 48000 -ac 2 -f s16le t.s16)
//
// A file that plays silent in the window but decodes here points at the
// device; one that fails here points at the codec.

#include "media/AudioDecoder.h"
#include "media/Mp4.h"
#include "media/StreamBuffer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

std::vector<std::uint8_t> read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int usage()
{
    std::fprintf(stderr, "usage: media_probe <file> | media_probe --sound <file> [--out <s16>] [--truth <s16>]\n");
    return 2;
}

}

int main(int argc, char** argv)
{
    std::string file, out, truth;
    bool sound = false;
    for (int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--sound")
            sound = true;
        else if (arg == "--out" && i + 1 < argc)
            out = argv[++i];
        else if (arg == "--truth" && i + 1 < argc)
            truth = argv[++i];
        else if (file.empty())
            file = arg;
        else
            return usage();
    }
    if (file.empty())
        return usage();
    std::vector<std::uint8_t> const bytes = read_file(file);
    if (bytes.empty()) {
        std::fprintf(stderr, "media_probe: cannot read %s\n", file.c_str());
        return 1;
    }
    media::StreamBuffer buffer;
    buffer.expect(bytes.size() > 8 && std::memcmp(bytes.data() + 4, "ftyp", 4) == 0 ? media::StreamBuffer::Container::Mp4
                                                                                  : media::StreamBuffer::Container::WebM);
    if (!buffer.append(bytes).ok) {
        std::fprintf(stderr, "media_probe: the file does not parse as its container\n");
        return 1;
    }
    media::StreamBuffer::Track const* audio = nullptr;
    for (media::StreamBuffer::Track const& track : buffer.tracks()) {
        char const* kind = track.description.kind == media::WebmTrack::Kind::Video ? "video"
            : track.description.kind == media::WebmTrack::Kind::Audio             ? "audio"
                                                                                  : "other";
        std::int64_t first = track.frames.empty() ? 0 : track.frames.begin()->first;
        std::int64_t last = track.frames.empty() ? 0 : std::prev(track.frames.end())->first;
        std::printf("track %llu: %s %s, %zu frames, %.3f s to %.3f s, %zu bytes of configuration\n",
            static_cast<unsigned long long>(track.description.number), kind,
            track.description.codec_id.c_str(), track.frames.size(), static_cast<double>(first) / 1e9, static_cast<double>(last) / 1e9,
            track.description.codec_private.size());
        if (track.description.kind == media::WebmTrack::Kind::Audio && !audio)
            audio = &track;
    }
    if (!sound)
        return 0;
    if (!audio) {
        std::fprintf(stderr, "media_probe: no audio track\n");
        return 1;
    }
    std::string error;
    std::unique_ptr<media::AudioDecoder> decoder = media::open_audio_decoder(audio->description.codec_id, audio->description.codec_private, error);
    if (!decoder) {
        std::fprintf(stderr, "media_probe: no decoder: %s\n", error.c_str());
        return 1;
    }
    std::vector<float> samples;
    std::size_t failed = 0, packets = 0;
    for (auto const& [time_ns, frame] : audio->frames) {
        media::AudioDecoder::Result const result = decoder->decode(frame.data, samples);
        ++packets;
        if (!result.ok)
            ++failed;
    }
    double peak = 0, energy = 0;
    for (float const v : samples) {
        peak = std::max(peak, std::fabs(static_cast<double>(v)));
        energy += static_cast<double>(v) * static_cast<double>(v);
    }
    std::printf("sound: %zu packets, %zu failed, %zu stereo frames at 48 kHz (%.2f s), peak %.4f, rms %.4f\n", packets, failed,
        samples.size() / 2, static_cast<double>(samples.size()) / 2 / 48000, peak, samples.empty() ? 0 : std::sqrt(energy / static_cast<double>(samples.size())));
    if (!out.empty()) {
        std::ofstream o(out, std::ios::binary);
        for (float const v : samples) {
            auto const s = static_cast<std::int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767));
            o.put(static_cast<char>(s & 0xFF)).put(static_cast<char>((s >> 8) & 0xFF));
        }
    }
    if (!truth.empty()) {
        std::vector<std::uint8_t> const t = read_file(truth);
        std::vector<float> ref(t.size() / 2);
        for (std::size_t i = 0; i < ref.size(); ++i)
            ref[i] = static_cast<std::int16_t>(t[2 * i] | (t[2 * i + 1] << 8)) / 32768.0f;
        // Lined up where they agree best (the encoder's delay, the edit list).
        double best = -1e9;
        long best_lag = 0;
        for (long lag = -8192; lag <= 8192; lag += 2) {
            double sig = 0, err = 0;
            for (std::size_t i = 0; i < ref.size(); i += 5) {
                long const j = static_cast<long>(i) + lag;
                if (j < 0 || static_cast<std::size_t>(j) >= samples.size())
                    continue;
                double const r = static_cast<double>(ref[i]);
                double const d = static_cast<double>(samples[static_cast<std::size_t>(j)]) - r;
                sig += r * r;
                err += d * d;
            }
            double const ratio = err > 0 ? 10 * std::log10(sig / err) : 200;
            if (ratio > best) {
                best = ratio;
                best_lag = lag;
            }
        }
        std::printf("against the truth: SNR %.1f dB at a lag of %ld samples (%zu truth samples)\n", best, best_lag, ref.size());
    }
    return failed ? 1 : 0;
}
