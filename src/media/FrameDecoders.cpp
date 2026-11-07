#include "media/FrameDecoder.h"

#include "media/H264Decoder.h"
#include "media/Vp9.h"

namespace sashfold::media {

namespace {

class Vp9FrameDecoder final : public FrameDecoder {
public:
    explicit Vp9FrameDecoder(std::unique_ptr<Vp9Accelerator> accelerator)
        : m_accelerator(std::move(accelerator))
    {
    }

    std::string const& device() const override { return m_accelerator->device(); }

    Result decode(std::span<std::uint8_t const> block, bool wanted) override
    {
        Result result;
        for (auto const frame : split_vp9_superframe(block)) {
            std::optional<Vp9FrameHeader> const header = m_reader.read(frame);
            if (!header) {
                result.ok = false;
                break;
            }
            if (header->key_frame)
                m_colour = vp9_colour(header->color_space, header->color_range, header->height);
            if (header->show_existing_frame) {
                if (wanted)
                    result.shown = m_accelerator->read_slot(header->frame_to_show);
                else
                    result.skipped++;
                continue;
            }
            if (!m_accelerator->decode(frame, *header)) {
                result.ok = false;
                break;
            }
            result.decoded++;
            if (!header->show_frame)
                continue;
            if (wanted)
                result.shown = m_accelerator->read_last();
            else
                result.skipped++;
        }
        result.colour = m_colour;
        return result;
    }

    void reset() override
    {
        m_reader.reset();
        m_accelerator->reset();
    }

private:
    std::unique_ptr<Vp9Accelerator> m_accelerator;
    Vp9HeaderReader m_reader;
    YuvColour m_colour; // a key frame's, for the frames after it
};

class H264FrameDecoder final : public FrameDecoder {
public:
    H264FrameDecoder(std::unique_ptr<H264Accelerator> accelerator, std::span<std::uint8_t const> avcc)
        : m_decoder(std::move(accelerator))
    {
        m_configured = m_decoder.set_configuration(avcc);
        if (std::optional<H264DecoderConfiguration> const config = parse_avcc(avcc); config && !config->sps.empty()) {
            if (std::optional<H264Sps> const sps = parse_h264_sps(config->sps[0]))
                m_colour = h264_colour(sps->vui.present && sps->vui.colour_description_present, sps->vui.matrix_coefficients,
                    sps->vui.video_full_range, sps->height());
        }
    }

    bool configured() const { return m_configured; }
    std::string const& device() const override { return m_decoder.device(); }

    Result decode(std::span<std::uint8_t const> block, bool wanted) override
    {
        Result result;
        // Every picture is shown: read back only when it is wanted.
        std::optional<Nv12Picture> picture = m_decoder.decode(block);
        H264Decoder::Counts const& counts = m_decoder.counts();
        if (counts.decoded > m_decoded) {
            result.decoded = counts.decoded - m_decoded;
            m_decoded = counts.decoded;
            if (wanted)
                result.shown = std::move(picture);
            else
                result.skipped++;
        } else if (counts.failed > m_failed) {
            m_failed = counts.failed;
            result.ok = false;
        }
        result.colour = m_colour;
        return result;
    }

    void reset() override { m_decoder.reset(); }

private:
    H264Decoder m_decoder;
    bool m_configured = false;
    std::uint64_t m_decoded = 0;
    std::uint64_t m_failed = 0;
    YuvColour m_colour;
};

} // namespace

std::unique_ptr<FrameDecoder> vp9_frame_decoder(std::unique_ptr<Vp9Accelerator> accelerator)
{
    return std::make_unique<Vp9FrameDecoder>(std::move(accelerator));
}

std::unique_ptr<FrameDecoder> open_frame_decoder(std::string const& codec_id, std::span<std::uint8_t const> codec_private, std::string& error)
{
    if (codec_id == "V_VP9") {
        std::unique_ptr<Vp9Accelerator> accelerator = Vp9Accelerator::open(error);
        return accelerator ? vp9_frame_decoder(std::move(accelerator)) : nullptr;
    }
    if (codec_id == "V_H264") {
        std::unique_ptr<H264Accelerator> accelerator = H264Accelerator::open(error);
        if (!accelerator)
            return nullptr;
        auto decoder = std::make_unique<H264FrameDecoder>(std::move(accelerator), codec_private);
        if (!decoder->configured()) {
            error = "the track's H.264 configuration record could not be read";
            return nullptr;
        }
        return decoder;
    }
    error = "no decoder for " + codec_id;
    return nullptr;
}

}
