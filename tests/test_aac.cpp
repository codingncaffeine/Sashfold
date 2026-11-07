#include "Test.h"

#include "media/Aac.h"
#include "media/Mp4.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

// The AAC-LC decoder over files ffmpeg encoded (tools/aac-make-tests.sh):
// stereo with mid/side, intensity stereo and TNS, mono, and 48 kHz, each
// decoded and compared sample by sample with ffmpeg's decoding of the same
// file — the signal-to-noise ratio between the two above 60 dB, which is
// two floating-point decoders' disagreement, not a fault (a wrong codebook,
// scalefactor or window shows as a ratio near zero).

using namespace sashfold;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void check_file(std::string const& folder, std::string const& name, int channels, int rate)
{
    Bytes const file = read_file(folder + "/" + name + ".m4a");
    Bytes const truth_bytes = read_file(folder + "/" + name + ".s16");
    CHECK(!file.empty() && !truth_bytes.empty());
    if (file.empty() || truth_bytes.empty())
        return;
    std::vector<float> truth(truth_bytes.size() / 2);
    for (std::size_t i = 0; i < truth.size(); ++i) {
        std::int16_t const v = static_cast<std::int16_t>(truth_bytes[2 * i] | (truth_bytes[2 * i + 1] << 8));
        truth[i] = v / 32768.0f;
    }
    media::Mp4Parser parser;
    CHECK(parser.append(file));
    media::Mp4Track const* audio = nullptr;
    for (media::Mp4Track const& t : parser.init().tracks) {
        if (t.kind == media::Mp4Track::Kind::Audio)
            audio = &t;
    }
    CHECK(audio && audio->codec_id == "A_AAC");
    if (!audio)
        return;
    std::optional<media::AacConfig> const config = media::parse_aac_config(audio->codec_private);
    CHECK(config && config->channels == channels && config->sample_rate == rate && !config->sbr_signalled);
    if (!config)
        return;
    media::AacDecoder decoder(*config);
    std::vector<float> ours;
    std::vector<float> block;
    int failed = 0;
    for (media::Mp4Frame const& frame : parser.take_frames()) {
        if (frame.track != audio->id)
            continue;
        if (!decoder.decode(frame.data, block)) {
            ++failed;
            block.assign(1024 * static_cast<std::size_t>(channels), 0.0f);
        }
        ours.insert(ours.end(), block.begin(), block.end());
    }
    CHECK(failed == 0);
    // The encoder's delay: ffmpeg trims it from its own output by the edit
    // list; ours begins at the first frame. Lined up by the edit the parser
    // applied (frames before time zero), the samples are compared where
    // both exist.
    std::int64_t first_pts_ns = 0;
    {
        media::Mp4Parser again;
        again.append(file);
        bool first = true;
        for (media::Mp4Frame const& frame : again.take_frames()) {
            if (frame.track == audio->id && first) {
                first_pts_ns = frame.time_ns;
                first = false;
            }
        }
    }
    std::size_t const skip = static_cast<std::size_t>(std::llround(-first_pts_ns * 1e-9 * rate)) * static_cast<std::size_t>(channels);
    std::size_t const count = std::min(truth.size(), ours.size() > skip ? ours.size() - skip : 0);
    CHECK(count > 1024 * static_cast<std::size_t>(channels) * 10);
    double signal = 0, noise = 0;
    for (std::size_t i = 0; i < count; ++i) {
        double const t = truth[i];
        double const d = static_cast<double>(ours[skip + i]) - t;
        signal += t * t;
        noise += d * d;
    }
    double const snr_db = noise > 0 ? 10 * std::log10(signal / noise) : 200;
    std::fprintf(stderr, "test_aac: %s: %zu samples compared, SNR %.1f dB\n", name.c_str(), count, snr_db);
    if (snr_db < 60) {
        // Where the two line up best, for whoever is fixing the decoder: the
        // lag (in frames of all channels) with the highest ratio.
        double best = -1e9;
        long best_lag = 0;
        for (long lag = -4096; lag <= 4096; lag += 1) {
            double sig = 0, err = 0;
            for (std::size_t i = 0; i + 8192 < count; i += 7) {
                long const j = static_cast<long>(skip + i) + lag * channels;
                if (j < 0 || static_cast<std::size_t>(j) >= ours.size())
                    continue;
                double const t = truth[i];
                double const d = static_cast<double>(ours[static_cast<std::size_t>(j)]) - t;
                sig += t * t;
                err += d * d;
            }
            double const ratio = err > 0 ? 10 * std::log10(sig / err) : 200;
            if (ratio > best) {
                best = ratio;
                best_lag = lag;
            }
        }
        std::fprintf(stderr, "    best alignment: lag %ld samples, SNR %.1f dB\n", best_lag, best);
    }
    CHECK(snr_db > 60);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        CHECK(!"the fixtures folder is the first argument");
        return test::report("test_aac");
    }
    std::string const folder = argv[1];
    check_file(folder, "stereo", 2, 44100);
    check_file(folder, "mono", 1, 44100);
    check_file(folder, "stereo-48k", 2, 48000);
    // A configuration this decoder does not read says so.
    CHECK(!media::parse_aac_config(Bytes { 0x12, 0x48 })); // AAC-LC at 44.1 kHz with channel configuration 9
    CHECK(media::parse_aac_config(Bytes { 0x12, 0x10 })); // ... and with 2 channels
    CHECK(!media::parse_aac_config(Bytes { 0x12 }));
    return test::report("test_aac");
}
