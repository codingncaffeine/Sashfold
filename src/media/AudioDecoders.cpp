#include "media/AudioDecoder.h"

#include "media/Aac.h"
#include "media/Opus.h"
#include "media/Resampler.h"

#include <cmath>
#include <optional>

namespace sashfold::media {

namespace {

class OpusAudioDecoder final : public AudioDecoder {
public:
    OpusAudioDecoder()
        : m_opus(output_channels)
    {
    }
    int native_rate() const override { return 48000; }
    Result decode(std::span<std::uint8_t const> packet, std::vector<float>& out) override
    {
        OpusDecoder::Result const r = m_opus.decode(packet, out);
        Result result;
        if (r.outcome == OpusDecoder::Outcome::Invalid) {
            m_opus.conceal(960, out);
            result.input_frames = 960;
            return result;
        }
        result.ok = r.outcome == OpusDecoder::Outcome::Decoded;
        result.input_frames = static_cast<std::size_t>(r.samples);
        return result;
    }
    void reset() override { m_opus.reset(); }

private:
    OpusDecoder m_opus;
};

// AAC: its channels mixed down to two (ITU-R BS.775's weights, the center
// and surrounds at -3 dB), then brought to the speakers' rate.
class AacAudioDecoder final : public AudioDecoder {
public:
    explicit AacAudioDecoder(AacConfig const& config)
        : m_decoder(config)
        , m_config(config)
        , m_resampler(output_channels, config.sample_rate, output_rate)
    {
    }
    int native_rate() const override { return m_config.sample_rate; }
    Result decode(std::span<std::uint8_t const> packet, std::vector<float>& out) override
    {
        Result result;
        result.input_frames = 1024;
        m_frame.clear();
        result.ok = m_decoder.decode(packet, m_frame);
        if (!result.ok)
            m_frame.assign(1024 * static_cast<std::size_t>(m_config.channels), 0.0f);
        mix_down();
        m_resampler.resample(m_stereo, out);
        return result;
    }
    void reset() override
    {
        m_decoder.reset();
        m_resampler.reset();
    }

private:
    void mix_down()
    {
        int const channels = m_config.channels;
        m_stereo.assign(1024 * 2, 0.0f);
        float const k = 0.7071f;
        for (std::size_t i = 0; i < 1024; ++i) {
            float const* s = m_frame.data() + i * static_cast<std::size_t>(channels);
            float left = 0, right = 0;
            switch (m_config.channel_configuration) {
            case 1:
                left = right = s[0];
                break;
            case 2:
                left = s[0];
                right = s[1];
                break;
            case 3: // C, L, R
                left = s[1] + k * s[0];
                right = s[2] + k * s[0];
                break;
            case 4: // C, L, R, S
                left = s[1] + k * (s[0] + s[3]);
                right = s[2] + k * (s[0] + s[3]);
                break;
            case 5: // C, L, R, Ls, Rs
            case 6: // and LFE, left out of the mix as players leave it
                left = s[1] + k * (s[0] + s[3]);
                right = s[2] + k * (s[0] + s[4]);
                break;
            case 7: // C, Lc, Rc, L, R, Ls, Rs, LFE
                left = s[3] + k * (s[0] + s[1] + s[5]);
                right = s[4] + k * (s[0] + s[2] + s[6]);
                break;
            default:
                left = s[0];
                right = channels > 1 ? s[1] : s[0];
                break;
            }
            // Room for the mix: more than two channels are held back by 3 dB.
            float const headroom = channels > 2 ? k : 1.0f;
            m_stereo[2 * i] = left * headroom;
            m_stereo[2 * i + 1] = right * headroom;
        }
    }

    AacDecoder m_decoder;
    AacConfig m_config;
    Resampler m_resampler;
    std::vector<float> m_frame;
    std::vector<float> m_stereo;
};

} // namespace

std::unique_ptr<AudioDecoder> open_audio_decoder(std::string const& codec_id, std::span<std::uint8_t const> codec_private, std::string& error)
{
    if (codec_id == "A_OPUS")
        return std::make_unique<OpusAudioDecoder>();
    if (codec_id == "A_AAC") {
        std::optional<AacConfig> const config = parse_aac_config(codec_private);
        if (!config) {
            error = "the AAC configuration is not one this decoder reads";
            return nullptr;
        }
        return std::make_unique<AacAudioDecoder>(*config);
    }
    error = "no decoder for " + codec_id;
    return nullptr;
}

}
