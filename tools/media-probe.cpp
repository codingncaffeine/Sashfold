// media_probe: an MP4 (or WebM) file's tracks as the engine reads them,
// and its sound through the element's own path — the track's decoder, the
// mix-down to stereo and the resampler to 48 kHz — written out as 16-bit
// samples, or compared with another program's decoding of the same file.
//
//   media_probe <file>                          the tracks and their frames
//   media_probe --sound <file> --out <s16>      stereo 48 kHz samples, interleaved
//   media_probe --sound <file> --truth <s16>    the SNR against ffmpeg's
//                                               (ffmpeg -i f -ar 48000 -ac 2 -f s16le t.s16)
//   media_probe --native <file> --wav <wav>     an AAC track decoded at its own rate and
//                                               channels (no mix-down, no resampler),
//                                               compared with a reference decoder's WAVE
//
// A file that plays silent in the window but decodes here points at the
// device; one that fails here points at the codec.

#include "media/Aac.h"
#include "media/AudioDecoder.h"
#include "media/Mp4.h"
#include "media/StreamBuffer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
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
    std::fprintf(stderr, "usage: media_probe <file> | media_probe --sound <file> [--out <s16>] [--truth <s16>] | media_probe --native <file> --wav <wav>\n");
    return 2;
}

// A WAVE file's PCM (16 or 24 bit, or 32-bit float) as floats in [-1, 1].
struct Wave {
    int rate = 0;
    int channels = 0;
    std::vector<float> samples; // interleaved
};

std::optional<Wave> read_wave(std::vector<std::uint8_t> const& b)
{
    auto const le16 = [&](std::size_t p) { return static_cast<std::uint32_t>(b[p] | (b[p + 1] << 8)); };
    auto const le32 = [&](std::size_t p) { return le16(p) | (le16(p + 2) << 16); };
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return std::nullopt;
    Wave w;
    int bits = 0, format = 0;
    for (std::size_t p = 12; p + 8 <= b.size();) {
        std::uint32_t const size = le32(p + 4);
        std::size_t const body = p + 8;
        std::size_t const end = std::min<std::size_t>(b.size(), body + size);
        if (std::memcmp(b.data() + p, "fmt ", 4) == 0 && size >= 16) {
            format = static_cast<int>(le16(body));
            w.channels = static_cast<int>(le16(body + 2));
            w.rate = static_cast<int>(le32(body + 4));
            bits = static_cast<int>(le16(body + 14));
            if (format == 0xFFFE && size >= 26)
                format = static_cast<int>(le16(body + 24));
        } else if (std::memcmp(b.data() + p, "data", 4) == 0) {
            int const bytes = bits / 8;
            if (bytes <= 0 || w.channels <= 0)
                return std::nullopt;
            for (std::size_t q = body; q + static_cast<std::size_t>(bytes) <= end; q += static_cast<std::size_t>(bytes)) {
                if (format == 3 && bits == 32) {
                    std::uint32_t const u = le32(q);
                    float f;
                    std::memcpy(&f, &u, 4);
                    w.samples.push_back(f);
                } else if (bits == 16) {
                    w.samples.push_back(static_cast<float>(static_cast<std::int16_t>(le16(q))) / 32768.0f);
                } else if (bits == 24) {
                    std::int32_t const v = static_cast<std::int32_t>((le16(q) << 8) | (static_cast<std::uint32_t>(b[q + 2]) << 24)) >> 8;
                    w.samples.push_back(static_cast<float>(v) / 8388608.0f);
                } else {
                    return std::nullopt;
                }
            }
        }
        p = body + size + (size & 1);
    }
    if (w.rate <= 0 || w.channels <= 0)
        return std::nullopt;
    return w;
}

}

int main(int argc, char** argv)
{
    std::string file, out, truth, wav;
    bool sound = false, native = false;
    for (int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--sound")
            sound = true;
        else if (arg == "--native")
            native = true;
        else if (arg == "--wav" && i + 1 < argc)
            wav = argv[++i];
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
    if (native) {
        if (!audio || wav.empty())
            return usage();
        std::optional<media::AacConfig> const config = media::parse_aac_config(audio->description.codec_private);
        std::optional<Wave> const truth_wave = read_wave(read_file(wav));
        if (!config || !truth_wave) {
            std::fprintf(stderr, "media_probe: %s\n", !config ? "not an AAC configuration this decoder reads" : "cannot read the WAVE");
            return 1;
        }
        media::AacDecoder aac(*config);
        std::vector<float> decoded, block;
        std::size_t failed = 0, packets = 0;
        int layout = aac.channels();
        for (auto const& [time_ns, frame] : audio->frames) {
            ++packets;
            block.clear();
            if (!aac.decode(frame.data, block)) {
                ++failed;
                block.assign(static_cast<std::size_t>(aac.frames_per_block()) * static_cast<std::size_t>(aac.channels()), 0.0f);
            }
            // A single channel becomes two where parametric stereo is first
            // met (its SBR header may come late in the stream): what came
            // before is the one channel on both sides, as a decoder that
            // outputs the pair throughout gives it.
            if (aac.channels() != layout) {
                std::printf("native: %d channels from packet %zu on\n", aac.channels(), packets);
                if (layout == 1 && aac.channels() == 2) {
                    std::vector<float> both(decoded.size() * 2);
                    for (std::size_t i = 0; i < decoded.size(); ++i)
                        both[2 * i] = both[2 * i + 1] = decoded[i];
                    decoded = std::move(both);
                }
                layout = aac.channels();
            }
            decoded.insert(decoded.end(), block.begin(), block.end());
        }
        int const channels = aac.channels();
        if (!out.empty()) {
            // 32-bit float WAVE at the decoder's rate and layout.
            std::ofstream o(out, std::ios::binary);
            auto const put32 = [&](std::uint32_t v) { for (int b = 0; b < 4; ++b) o.put(static_cast<char>((v >> (8 * b)) & 0xFF)); };
            auto const put16 = [&](std::uint32_t v) { o.put(static_cast<char>(v & 0xFF)).put(static_cast<char>((v >> 8) & 0xFF)); };
            std::uint32_t const data = static_cast<std::uint32_t>(decoded.size() * 4);
            o.write("RIFF", 4);
            put32(36 + data);
            o.write("WAVEfmt ", 8);
            put32(16);
            put16(3);
            put16(static_cast<std::uint32_t>(channels));
            put32(static_cast<std::uint32_t>(aac.output_rate()));
            put32(static_cast<std::uint32_t>(aac.output_rate() * channels * 4));
            put16(static_cast<std::uint32_t>(channels * 4));
            put16(32);
            o.write("data", 4);
            put32(data);
            o.write(reinterpret_cast<char const*>(decoded.data()), static_cast<std::streamsize>(data));
        }
        auto const level = [](std::vector<float> const& v) {
            double peak = 0, energy = 0;
            std::size_t bad = 0;
            for (float const x : v) {
                if (!std::isfinite(x)) {
                    ++bad;
                    continue;
                }
                peak = std::max(peak, std::fabs(static_cast<double>(x)));
                energy += static_cast<double>(x) * static_cast<double>(x);
            }
            std::printf("  peak %.4g, rms %.4g, %zu not finite\n", peak, v.empty() ? 0 : std::sqrt(energy / static_cast<double>(v.size())), bad);
        };
        std::printf("native: decoded");
        level(decoded);
        std::printf("native: truth  ");
        level(truth_wave->samples);
        std::printf("native: object type %d, core %d Hz, out %d Hz, %d channels, %zu packets, %zu failed, %zu frames; truth %d Hz, %d channels, %zu frames\n",
            config->object_type, aac.sample_rate(), aac.output_rate(), channels, packets, failed, decoded.size() / static_cast<std::size_t>(std::max(1, channels)),
            truth_wave->rate, truth_wave->channels, truth_wave->samples.size() / static_cast<std::size_t>(truth_wave->channels));
        if (channels != truth_wave->channels || aac.output_rate() != truth_wave->rate) {
            std::printf("native: the layout or the rate differs from the truth's\n");
            return 1;
        }
        // Per channel, at the lag where they agree best (a reference decoder
        // may or may not cut the bank's delay), then the worst channel.
        std::size_t const n = decoded.size() / static_cast<std::size_t>(channels);
        std::size_t const tn = truth_wave->samples.size() / static_cast<std::size_t>(channels);
        double worst = 1e9;
        long worst_lag = 0;
        for (int c = 0; c < channels; ++c) {
            double best = -1e9;
            long best_lag = 0;
            for (long lag = -4096; lag <= 4096; ++lag) {
                double sig = 0, err = 0;
                for (std::size_t i = 0; i < tn; i += (lag % 64 == 0 ? 1 : 7)) {
                    long const j = static_cast<long>(i) + lag;
                    if (j < 0 || static_cast<std::size_t>(j) >= n)
                        continue;
                    double const r = static_cast<double>(truth_wave->samples[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)]);
                    double const d = static_cast<double>(decoded[static_cast<std::size_t>(j) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)]) - r;
                    sig += r * r;
                    err += d * d;
                }
                double const ratio = err > 0 ? 10 * std::log10(sig / err) : 200;
                if (ratio > best) {
                    best = ratio;
                    best_lag = lag;
                }
            }
            // The winner measured on every sample.
            double sig = 0, err = 0;
            for (std::size_t i = 0; i < tn; ++i) {
                long const j = static_cast<long>(i) + best_lag;
                if (j < 0 || static_cast<std::size_t>(j) >= n)
                    continue;
                double const r = static_cast<double>(truth_wave->samples[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)]);
                double const d = static_cast<double>(decoded[static_cast<std::size_t>(j) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)]) - r;
                sig += r * r;
                err += d * d;
            }
            best = err > 0 ? 10 * std::log10(sig / err) : 200;
            if (best < worst) {
                worst = best;
                worst_lag = best_lag;
            }
        }
        std::printf("against the truth: worst channel SNR %.1f dB at a lag of %ld samples\n", worst, worst_lag);
        return failed ? 1 : 0;
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
