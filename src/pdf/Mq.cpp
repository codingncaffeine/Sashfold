#include "pdf/Mq.h"

namespace sashfold::pdf {

namespace {

// Table C.2: each state's probability estimate of the less probable
// symbol (Qe), the state after a more or a less probable decision, and
// whether a less probable one swaps the senses.
struct State {
    std::uint16_t qe;
    std::uint8_t next_mps;
    std::uint8_t next_lps;
    std::uint8_t swap;
};

constexpr State states[47] = {
    { 0x5601, 1, 1, 1 }, { 0x3401, 2, 6, 0 }, { 0x1801, 3, 9, 0 }, { 0x0AC1, 4, 12, 0 }, { 0x0521, 5, 29, 0 },
    { 0x0221, 38, 33, 0 }, { 0x5601, 7, 6, 1 }, { 0x5401, 8, 14, 0 }, { 0x4801, 9, 14, 0 }, { 0x3801, 10, 14, 0 },
    { 0x3001, 11, 17, 0 }, { 0x2401, 12, 18, 0 }, { 0x1C01, 13, 20, 0 }, { 0x1601, 29, 21, 0 }, { 0x5601, 15, 14, 1 },
    { 0x5401, 16, 14, 0 }, { 0x5101, 17, 15, 0 }, { 0x4801, 18, 16, 0 }, { 0x3801, 19, 17, 0 }, { 0x3401, 20, 18, 0 },
    { 0x3001, 21, 19, 0 }, { 0x2801, 22, 19, 0 }, { 0x2401, 23, 20, 0 }, { 0x2201, 24, 21, 0 }, { 0x1C01, 25, 22, 0 },
    { 0x1801, 26, 23, 0 }, { 0x1601, 27, 24, 0 }, { 0x1401, 28, 25, 0 }, { 0x1201, 29, 26, 0 }, { 0x1101, 30, 27, 0 },
    { 0x0AC1, 31, 28, 0 }, { 0x09C1, 32, 29, 0 }, { 0x08A1, 33, 30, 0 }, { 0x0521, 34, 31, 0 }, { 0x0441, 35, 32, 0 },
    { 0x02A1, 36, 33, 0 }, { 0x0221, 37, 34, 0 }, { 0x0141, 38, 35, 0 }, { 0x0111, 39, 36, 0 }, { 0x0085, 40, 37, 0 },
    { 0x0049, 41, 38, 0 }, { 0x0025, 42, 39, 0 }, { 0x0015, 43, 40, 0 }, { 0x0009, 44, 41, 0 }, { 0x0005, 45, 42, 0 },
    { 0x0001, 45, 43, 0 }, { 0x5601, 46, 46, 0 },
};

} // namespace

MqDecoder::MqDecoder(std::span<std::uint8_t const> data)
    : m_data(data)
{
    // INITDEC: the first byte into C's high half, the next through BYTEIN,
    // seven bits shifted in.
    m_c = static_cast<std::uint32_t>(m_data.empty() ? 0xFF : m_data[0]) << 16;
    byte_in();
    m_c <<= 7;
    m_ct -= 7;
    m_a = 0x8000;
}

void MqDecoder::byte_in()
{
    // Past the data the code reads as a marker: ones from there on.
    auto const at = [this](std::size_t i) -> std::uint32_t { return i < m_data.size() ? m_data[i] : 0xFF; };
    if (at(m_position) == 0xFF) {
        if (at(m_position + 1) > 0x8F) {
            m_c += 0xFF00;
            m_ct = 8;
        } else {
            ++m_position;
            m_c += at(m_position) << 9; // a stuffed bit after 0xFF
            m_ct = 7;
        }
    } else {
        ++m_position;
        m_c += at(m_position) << 8;
        m_ct = 8;
    }
}

void MqDecoder::renormalize()
{
    do {
        if (m_ct == 0)
            byte_in();
        m_a <<= 1;
        m_c <<= 1;
        --m_ct;
    } while ((m_a & 0x8000) == 0);
}

int MqDecoder::decode(MqContext& context)
{
    State const& state = states[context.state];
    std::uint32_t const qe = state.qe;
    m_a -= qe;
    int decision;
    if ((m_c >> 16) < qe) {
        // LPS_EXCHANGE: the lower sub-interval.
        if (m_a < qe) {
            decision = context.mps;
            context.state = state.next_mps;
        } else {
            decision = 1 - context.mps;
            if (state.swap)
                context.mps = static_cast<std::uint8_t>(1 - context.mps);
            context.state = state.next_lps;
        }
        m_a = qe;
        renormalize();
    } else {
        m_c -= qe << 16;
        if ((m_a & 0x8000) == 0) {
            // MPS_EXCHANGE.
            if (m_a < qe) {
                decision = 1 - context.mps;
                if (state.swap)
                    context.mps = static_cast<std::uint8_t>(1 - context.mps);
                context.state = state.next_lps;
            } else {
                decision = context.mps;
                context.state = state.next_mps;
            }
            renormalize();
        } else {
            decision = context.mps;
        }
    }
    return decision;
}

}
