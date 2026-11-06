#include "pdf/JpxTier1.h"

#include "pdf/Mq.h"

#include <algorithm>
#include <array>
#include <optional>

namespace sashfold::pdf::jpx {

namespace {

// The contexts (T.800 D.3): 0-8 zero coding, 9-13 sign, 14-16 magnitude
// refinement, 17 run length, 18 uniform.
constexpr int context_run_length = 17;
constexpr int context_uniform = 18;
constexpr int contexts = 19;

// A coefficient's state, in a grid with a border of one around the block.
constexpr std::uint8_t significant = 1;
constexpr std::uint8_t negative = 2;
constexpr std::uint8_t visited = 4; // coded in this plane's significance pass
constexpr std::uint8_t refined = 8; // refined once already

enum class Pass { Significance, Refinement, Cleanup };

// Raw bits, for the passes the bypass option leaves to them (D.6): a
// byte after 0xFF gives seven bits, its first a stuffed zero.
class RawBits {
public:
    explicit RawBits(std::span<std::uint8_t const> data)
        : m_data(data)
    {
    }
    int bit()
    {
        if (m_left == 0) {
            bool const after_ff = m_byte == 0xFF;
            m_byte = m_position < m_data.size() ? m_data[m_position] : 0xFF;
            ++m_position;
            m_left = after_ff ? 7 : 8;
        }
        --m_left;
        return (m_byte >> m_left) & 1;
    }

private:
    std::span<std::uint8_t const> m_data;
    std::size_t m_position = 0;
    std::uint32_t m_byte = 0;
    int m_left = 0;
};

class Block {
public:
    Block(int width, int height, Band band, BlockStyle const& style)
        : m_width(width)
        , m_height(height)
        , m_stride(width + 2)
        , m_band(band)
        , m_style(style)
        , m_flags(static_cast<std::size_t>((width + 2) * (height + 2)), 0)
        , m_magnitude(static_cast<std::size_t>(width * height), 0)
        , m_known(static_cast<std::size_t>(width * height), -1)
    {
        reset_contexts();
    }

    void reset_contexts()
    {
        for (MqContext& context : m_contexts)
            context = MqContext {};
        m_contexts[0].state = 4;
        m_contexts[context_run_length].state = 3;
        m_contexts[context_uniform].state = 46;
    }

    void start_mq(std::span<std::uint8_t const> data)
    {
        m_mq.emplace(data);
        m_raw.reset();
    }
    void start_raw(std::span<std::uint8_t const> data)
    {
        m_raw.emplace(data);
        m_mq.reset();
    }

    void run(Pass pass, int plane)
    {
        // Stripes of four rows, column by column, each column top down.
        for (int top = 0; top < m_height; top += 4) {
            int const rows = std::min(4, m_height - top);
            for (int x = 0; x < m_width; ++x) {
                int y0 = 0;
                if (pass == Pass::Cleanup && rows == 4 && run_length_applies(x, top)) {
                    if (decide(context_run_length) == 0)
                        continue; // all four stay insignificant
                    int const first = (decide(context_uniform) << 1) | decide(context_uniform);
                    becomes_significant(x, top + first, plane);
                    y0 = first + 1;
                }
                for (int dy = y0; dy < rows; ++dy)
                    code(pass, x, top + dy, plane);
            }
        }
        if (pass == Pass::Cleanup) {
            for (std::uint8_t& flag : m_flags)
                flag &= static_cast<std::uint8_t>(~visited);
            if (m_style.segmentation) {
                for (int i = 0; i < 4; ++i)
                    decide(context_uniform);
            }
        }
    }

    void finish(std::vector<std::int32_t>& out) const
    {
        out.assign(static_cast<std::size_t>(m_width * m_height), 0);
        for (int y = 0; y < m_height; ++y) {
            for (int x = 0; x < m_width; ++x) {
                std::size_t const i = static_cast<std::size_t>(y * m_width + x);
                if (!(flags(x, y) & significant))
                    continue;
                // Times two, and half the step of the lowest plane not known.
                std::int64_t value = static_cast<std::int64_t>(m_magnitude[i]) * 2;
                if (m_known[i] > 0)
                    value += std::int64_t { 1 } << m_known[i];
                value = std::min<std::int64_t>(value, 0x7FFFFFFF);
                out[i] = static_cast<std::int32_t>((flags(x, y) & negative) ? -value : value);
            }
        }
    }

private:
    std::uint8_t& flags(int x, int y) { return m_flags[static_cast<std::size_t>((y + 1) * m_stride + x + 1)]; }
    std::uint8_t flags(int x, int y) const { return m_flags[static_cast<std::size_t>((y + 1) * m_stride + x + 1)]; }

    // A neighbor's flags, as the contexts see it: with the causal option the
    // next stripe's row is not yet coded, so not significant.
    std::uint8_t neighbor(int x, int y, int from_y) const
    {
        if (m_style.causal && from_y % 4 == 3 && y > from_y)
            return 0;
        return flags(x, y);
    }

    int decide(int context)
    {
        if (m_raw)
            return m_raw->bit();
        return m_mq->decode(m_contexts[static_cast<std::size_t>(context)]);
    }

    // Table D.1: from the significant neighbors across, up and down, and
    // on the diagonals, by the subband's orientation.
    int zero_context(int x, int y) const
    {
        auto const sig = [&](int nx, int ny) { return (neighbor(nx, ny, y) & significant) ? 1 : 0; };
        int h = sig(x - 1, y) + sig(x + 1, y);
        int v = sig(x, y - 1) + sig(x, y + 1);
        int const d = sig(x - 1, y - 1) + sig(x + 1, y - 1) + sig(x - 1, y + 1) + sig(x + 1, y + 1);
        if (m_band == Band::HH) {
            int const hv = h + v;
            if (d >= 3)
                return 8;
            if (d == 2)
                return hv >= 1 ? 7 : 6;
            if (d == 1)
                return hv >= 2 ? 5 : hv == 1 ? 4 : 3;
            return hv >= 2 ? 2 : hv == 1 ? 1 : 0;
        }
        if (m_band == Band::HL)
            std::swap(h, v);
        if (h == 2)
            return 8;
        if (h == 1)
            return v >= 1 ? 7 : d >= 1 ? 6 : 5;
        if (v == 2)
            return 4;
        if (v == 1)
            return 3;
        return d >= 2 ? 2 : d == 1 ? 1 : 0;
    }

    bool run_length_applies(int x, int top) const
    {
        for (int dy = 0; dy < 4; ++dy) {
            if (flags(x, top + dy) & (significant | visited))
                return false;
            if (zero_context(x, top + dy) != 0)
                return false;
        }
        return true;
    }

    // Tables D.2 and D.3: the sign's context and the bit it is XORed with.
    int decode_sign(int x, int y)
    {
        auto const contribution = [&](int nx, int ny) {
            std::uint8_t const f = neighbor(nx, ny, y);
            return (f & significant) ? ((f & negative) ? -1 : 1) : 0;
        };
        int const h = std::clamp(contribution(x - 1, y) + contribution(x + 1, y), -1, 1);
        int const v = std::clamp(contribution(x, y - 1) + contribution(x, y + 1), -1, 1);
        int context;
        int flip = 0;
        if (h == 1)
            context = 12 + v;
        else if (h == 0)
            context = v == 0 ? 9 : 10, flip = v < 0 ? 1 : 0;
        else
            context = 12 - v, flip = 1;
        if (m_raw)
            return m_raw->bit();
        return decide(context) ^ flip;
    }

    void becomes_significant(int x, int y, int plane)
    {
        int const sign = decode_sign(x, y);
        std::uint8_t& f = flags(x, y);
        f |= significant;
        if (sign)
            f |= negative;
        std::size_t const i = static_cast<std::size_t>(y * m_width + x);
        m_magnitude[i] = std::uint32_t { 1 } << plane;
        m_known[i] = static_cast<std::int8_t>(plane);
    }

    void code(Pass pass, int x, int y, int plane)
    {
        std::uint8_t& f = flags(x, y);
        switch (pass) {
        case Pass::Significance: {
            if (f & significant)
                return;
            int const context = zero_context(x, y);
            if (context == 0)
                return;
            f |= visited;
            if (decide(context))
                becomes_significant(x, y, plane);
            return;
        }
        case Pass::Refinement: {
            if (!(f & significant) || (f & visited))
                return;
            int context = 16;
            if (!(f & refined)) {
                bool any = false;
                for (int ny = y - 1; ny <= y + 1 && !any; ++ny) {
                    for (int nx = x - 1; nx <= x + 1; ++nx) {
                        if ((nx != x || ny != y) && (neighbor(nx, ny, y) & significant)) {
                            any = true;
                            break;
                        }
                    }
                }
                context = any ? 15 : 14;
            }
            std::size_t const i = static_cast<std::size_t>(y * m_width + x);
            if (decide(context))
                m_magnitude[i] |= std::uint32_t { 1 } << plane;
            m_known[i] = static_cast<std::int8_t>(plane);
            f |= refined;
            return;
        }
        case Pass::Cleanup:
            if (f & (significant | visited))
                return;
            if (decide(zero_context(x, y)))
                becomes_significant(x, y, plane);
            return;
        }
    }

    int m_width;
    int m_height;
    int m_stride;
    Band m_band;
    BlockStyle m_style;
    std::vector<std::uint8_t> m_flags;
    std::vector<std::uint32_t> m_magnitude;
    std::vector<std::int8_t> m_known; // the lowest plane whose bit is known; -1 none
    std::array<MqContext, contexts> m_contexts {};
    std::optional<MqDecoder> m_mq;
    std::optional<RawBits> m_raw;
};

// Pass `index` of a block (0 the first cleanup): its kind.
Pass pass_kind(int index)
{
    if (index == 0)
        return Pass::Cleanup;
    switch ((index - 1) % 3) {
    case 0:
        return Pass::Significance;
    case 1:
        return Pass::Refinement;
    default:
        return Pass::Cleanup;
    }
}

bool raw_pass(BlockStyle const& style, int index)
{
    return style.bypass && index >= 10 && pass_kind(index) != Pass::Cleanup;
}

} // namespace

int segment_pass_limit(BlockStyle const& style, int first)
{
    if (style.terminate_all)
        return 1;
    if (!style.bypass)
        return 1 << 20;
    if (first < 10)
        return 10 - first;
    // From the tenth on: a raw significance + refinement pair, then a cleanup.
    return pass_kind(first) == Pass::Cleanup ? 1 : pass_kind(first) == Pass::Significance ? 2 : 1;
}

bool decode_code_block(std::span<Segment const> segments, int width, int height, int planes, Band band,
    BlockStyle const& style, std::vector<std::int32_t>& coefficients)
{
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024 || width * height > 4096 || planes > 30) {
        coefficients.assign(static_cast<std::size_t>(std::max(0, width) * std::max(0, height)), 0);
        return false;
    }
    Block block(width, height, band, style);
    int index = 0;
    int const total_passes = planes * 3 - 2;
    for (Segment const& segment : segments) {
        if (segment.passes <= 0)
            continue;
        if (raw_pass(style, index))
            block.start_raw(segment.data);
        else
            block.start_mq(segment.data);
        for (int p = 0; p < segment.passes && index < total_passes; ++p, ++index) {
            int const plane = planes - 1 - (index + 2) / 3;
            block.run(pass_kind(index), plane);
            if (style.reset)
                block.reset_contexts();
        }
    }
    block.finish(coefficients);
    return true;
}

}
