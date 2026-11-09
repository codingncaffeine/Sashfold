#include "Test.h"

#include "media/Aac.h"
#include "media/Mp4.h"
#include "media/Sbr.h"

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

// SBR's QMF banks (ISO/IEC 14496-3 §4.6.18.4): a tone through the 32-band
// analysis and the 64-band synthesis comes out the tone at twice the rate
// — the banks' modulation, prototype and scaling agree with each other,
// which the standard's perfect reconstruction promises. The delay the
// banks add is found where the two line up best.

using namespace sashfold;

using Bytes = std::vector<std::uint8_t>;

Bytes read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Bits written most significant first, for configurations built by hand.
struct BitWriter {
    Bytes bytes;
    int used = 8;
    void put(std::uint32_t value, int count)
    {
        for (int i = count - 1; i >= 0; --i) {
            if (used == 8) {
                bytes.push_back(0);
                used = 0;
            }
            bytes.back() = static_cast<std::uint8_t>(bytes.back() | (((value >> i) & 1u) << (7 - used)));
            ++used;
        }
    }
};

// An HE-AAC file written by an independent encoder (tools/sbr-make-tests.sh)
// against ffmpeg's decoding of it: SBR found in the stream (the file signals
// it implicitly), the output at twice the core's rate, sample for sample.
// `channels`: the output's; `coded` the configuration's (one, for HE-AAC
// v2's parametric stereo, which is met in the stream too). `skip`: output
// frames at the start left out of the comparison.
void check_file(std::string const& folder, std::string const& name, int channels, int core_rate, int coded = 0, std::size_t skip = 0)
{
    if (coded == 0)
        coded = channels;
    Bytes const file = read_file(folder + "/" + name + ".m4a");
    Bytes const truth_bytes = read_file(folder + "/" + name + ".f32");
    CHECK(!file.empty() && !truth_bytes.empty());
    if (file.empty() || truth_bytes.empty())
        return;
    std::vector<float> truth(truth_bytes.size() / 4);
    for (std::size_t i = 0; i < truth.size(); ++i) {
        std::uint32_t const u = truth_bytes[4 * i] | (truth_bytes[4 * i + 1] << 8) | (truth_bytes[4 * i + 2] << 16) | (static_cast<std::uint32_t>(truth_bytes[4 * i + 3]) << 24);
        float f;
        std::memcpy(&f, &u, 4);
        truth[i] = f;
    }
    media::Mp4Parser parser;
    CHECK(parser.append(file));
    media::Mp4Track const* audio = nullptr;
    for (media::Mp4Track const& t : parser.init().tracks) {
        if (t.kind == media::Mp4Track::Kind::Audio)
            audio = &t;
    }
    CHECK(audio != nullptr);
    if (!audio)
        return;
    std::optional<media::AacConfig> const config = media::parse_aac_config(audio->codec_private);
    CHECK(config && config->channels == coded && config->sample_rate == core_rate);
    if (!config)
        return;
    media::AacDecoder decoder(*config);
    std::vector<float> ours, block;
    int failed = 0;
    for (media::Mp4Frame const& frame : parser.take_frames()) {
        if (frame.track != audio->id)
            continue;
        if (!decoder.decode(frame.data, block)) {
            ++failed;
            block.assign(static_cast<std::size_t>(decoder.frames_per_block()) * static_cast<std::size_t>(channels), 0.0f);
        }
        ours.insert(ours.end(), block.begin(), block.end());
    }
    CHECK(failed == 0);
    CHECK_EQ(decoder.channels(), channels);
    CHECK_EQ(ours.size() % static_cast<std::size_t>(2048 * channels), 0u);
    CHECK_EQ(decoder.output_rate(), 2 * core_rate);
    CHECK_EQ(decoder.frames_per_block(), 2048);
    // Lined up where they agree best (ffmpeg may trim a priming delay).
    double best = -1e9;
    long best_lag = 0;
    for (long lag = -4096; lag <= 4096; ++lag) {
        double signal = 0, noise = 0;
        for (std::size_t i = skip * static_cast<std::size_t>(channels); i < truth.size(); i += (lag % 16 == 0 ? 1 : 13)) {
            long const j = static_cast<long>(i) + lag * channels;
            if (j < 0 || static_cast<std::size_t>(j) >= ours.size())
                continue;
            double const t = static_cast<double>(truth[i]);
            double const d = static_cast<double>(ours[static_cast<std::size_t>(j)]) - t;
            signal += t * t;
            noise += d * d;
        }
        double const snr = noise > 0 ? 10 * std::log10(signal / noise) : 200;
        if (snr > best) {
            best = snr;
            best_lag = lag;
        }
    }
    std::fprintf(stderr, "test_sbr: %s: SNR %.1f dB against ffmpeg's decoding at a lag of %ld\n", name.c_str(), best, best_lag);
    CHECK(best > 90);
}

// A tone's round trip: the SNR at the best lag, and that lag.
std::pair<double, int> round_trip(double f1, double a1, double f2, double a2)
{
    int const rate = 44100;
    std::size_t const frames = 8;
    std::vector<float> in(1024 * frames);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(a1 * std::sin(2 * 3.14159265358979 * f1 * static_cast<double>(i) / rate)
            + a2 * std::sin(2 * 3.14159265358979 * f2 * static_cast<double>(i) / rate));
    std::vector<float> out;
    media::sbr_qmf_round_trip(in, out);
    CHECK_EQ(out.size(), in.size() * 2);
    // The same tones at twice the rate: compared from the third frame on
    // (the banks' delay and settling before it), at the best lag.
    double best = -1e9;
    int best_lag = 0;
    for (int lag = 0; lag < 1500; ++lag) {
        double signal = 0, noise = 0;
        for (std::size_t n = 2 * 2048; n + static_cast<std::size_t>(lag) < out.size() - 2048; ++n) {
            double const t = a1 * std::sin(2 * 3.14159265358979 * f1 * static_cast<double>(n) / (2 * rate))
                + a2 * std::sin(2 * 3.14159265358979 * f2 * static_cast<double>(n) / (2 * rate));
            double const d = static_cast<double>(out[n + static_cast<std::size_t>(lag)]) - t;
            signal += t * t;
            noise += d * d;
        }
        double const snr = noise > 0 ? 10 * std::log10(signal / noise) : 200;
        if (snr > best) {
            best = snr;
            best_lag = lag;
        }
    }
    return { best, best_lag };
}

int main(int argc, char** argv)
{
    CHECK(argc >= 2 || !"the fixtures folder is the first argument");
    if (argc >= 2) {
        check_file(argv[1], "he-stereo-44k", 2, 22050);
        check_file(argv[1], "he-mono-48k", 1, 24000);
        // HE-AAC v2: one channel coded, parametric stereo making the pair.
        // From the second frame on: ffmpeg starts its mixing matrices at
        // zero, where the standard's baseline decoder (and the conformance
        // references, where parametric stereo starts mid-stream) starts
        // them at the neutral parameters' — the first frame fades in there.
        check_file(argv[1], "he-ps-44k", 2, 22050, 1, 2048);
    }
    {
        // Parametric stereo signalled explicitly (1.6.5.1): audio object
        // type 29 over a single channel — two channels out from the start.
        BitWriter w;
        w.put(29, 5);
        w.put(7, 4); // 22050
        w.put(1, 4); // mono
        w.put(4, 4); // extension at 44100
        w.put(2, 5); // AAC-LC
        w.put(0, 3);
        std::optional<media::AacConfig> const c = media::parse_aac_config(w.bytes);
        CHECK(c && c->sbr_signalled && c->ps_signalled && c->channels == 1);
        media::AacDecoder decoder(*c);
        CHECK_EQ(decoder.channels(), 2);
        CHECK_EQ(decoder.output_rate(), 44100);
    }
    {
        // Backward-compatible signalling (1.6.6): SBR's sync extension,
        // then parametric stereo's (0x548) with its present flag.
        BitWriter w;
        w.put(2, 5);
        w.put(7, 4); // 22050
        w.put(1, 4); // mono
        w.put(0, 3);
        w.put(0x2B7, 11);
        w.put(5, 5);
        w.put(1, 1);
        w.put(4, 4); // extension at 44100
        w.put(0x548, 11);
        w.put(1, 1);
        std::optional<media::AacConfig> const c = media::parse_aac_config(w.bytes);
        CHECK(c && c->sbr_signalled && c->ps_signalled && c->extension_sample_rate == 44100);
        media::AacDecoder decoder(*c);
        CHECK_EQ(decoder.channels(), 2);
    }
    {
        // Explicit signalling (1.6.5.1): audio object type 5, the core's
        // rate, the extension's, then the core's type.
        BitWriter w;
        w.put(5, 5); // SBR
        w.put(7, 4); // 22050
        w.put(2, 4); // stereo
        w.put(4, 4); // extension at 44100
        w.put(2, 5); // AAC-LC
        w.put(0, 3); // GASpecificConfig
        std::optional<media::AacConfig> const c = media::parse_aac_config(w.bytes);
        CHECK(c && c->sbr_signalled && c->sample_rate == 22050 && c->extension_sample_rate == 44100 && c->channels == 2);
        media::AacDecoder decoder(*c);
        CHECK_EQ(decoder.output_rate(), 44100);
    }
    {
        // Backward-compatible signalling (1.6.6): plain AAC-LC, then the
        // sync extension saying SBR is there — at the core's own rate, so
        // the downsampled bank: the output stays at 44100, 1024 a block.
        BitWriter w;
        w.put(2, 5);
        w.put(4, 4); // 44100
        w.put(2, 4);
        w.put(0, 3);
        w.put(0x2B7, 11);
        w.put(5, 5);
        w.put(1, 1);
        w.put(4, 4); // extension at 44100: downsampled
        std::optional<media::AacConfig> const c = media::parse_aac_config(w.bytes);
        CHECK(c && c->sbr_signalled && c->sample_rate == 44100 && c->extension_sample_rate == 44100);
        media::AacDecoder decoder(*c);
        CHECK_EQ(decoder.output_rate(), 44100);
        CHECK_EQ(decoder.frames_per_block(), 1024);
    }
    {
        // A program config element in place of the channel configuration:
        // one front channel pair.
        BitWriter w;
        w.put(2, 5);
        w.put(3, 4); // 48000
        w.put(0, 4); // channel configuration 0: a PCE follows
        w.put(0, 3); // GASpecificConfig
        w.put(0, 4); // element_instance_tag
        w.put(1, 2); // object_type
        w.put(3, 4); // sampling_frequency_index
        w.put(1, 4); // one front element
        w.put(0, 4);
        w.put(0, 4);
        w.put(0, 2);
        w.put(0, 3);
        w.put(0, 4);
        w.put(0, 3); // no mixdowns
        w.put(1, 1); // the front element is a pair
        w.put(0, 4);
        w.put(0, (8 - static_cast<int>((w.bytes.size() * 8 - static_cast<std::size_t>(8 - w.used)) % 8)) % 8); // byte_alignment
        w.put(0, 8); // no comment
        std::optional<media::AacConfig> const c = media::parse_aac_config(w.bytes);
        CHECK(c && c->channels == 2 && c->sample_rate == 48000);
    }
    // Tones at the middle of bands 1 and 5 (689 Hz a band), then at a band's
    // edge (where the aliasing of a bank that is not perfect would show), then
    // a pair of them.
    auto const [center, center_lag] = round_trip(1.5 * 44100 / 64, 0.5, 5.5 * 44100 / 64, 0.25);
    auto const [edge, edge_lag] = round_trip(6.0 * 44100 / 64, 0.5, 10.0 * 44100 / 64, 0.25);
    auto const [pair, pair_lag] = round_trip(1000.0, 0.5, 3777.0, 0.25);
    std::fprintf(stderr, "test_sbr: QMF round trip: band centers SNR %.1f dB (delay %d), band edges %.1f dB (delay %d), 1000 + 3777 Hz %.1f dB (delay %d)\n",
        center, center_lag, edge, edge_lag, pair, pair_lag);
    CHECK(center > 50 && edge > 50 && pair > 50);
    return test::report("test_sbr");
}
