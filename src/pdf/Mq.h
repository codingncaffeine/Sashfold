#pragma once

// The MQ arithmetic decoder JPEG 2000 and JBIG2 share, written from
// ITU-T T.800 Annex C (the same coder as T.88 Annex E): a decision at a
// time, each by a context whose probability state adapts as it is used.
// The software conventions of C.3: the C register keeps the code's bits,
// A the interval, CT the bits left before the next byte, 0xFF bytes
// followed by a byte above 0x8F being a marker that ends the data (ones
// are fed in from there).

#include <cstddef>
#include <cstdint>
#include <span>

namespace sashfold::pdf {

// One context: the index of its probability state (Table C.2) and the
// sense of its more probable symbol.
struct MqContext {
    std::uint8_t state = 0;
    std::uint8_t mps = 0;
};

class MqDecoder {
public:
    // Starts on the bytes from `data` (INITDEC, C.3.5).
    explicit MqDecoder(std::span<std::uint8_t const> data);

    // One decision in the context (DECODE, C.3.2).
    int decode(MqContext& context);

    // How far into the data the decoder has read.
    std::size_t position() const { return m_position; }

private:
    void byte_in(); // BYTEIN, C.3.4
    void renormalize(); // RENORMD, C.3.3

    std::span<std::uint8_t const> m_data;
    std::size_t m_position = 0;
    std::uint32_t m_c = 0;
    std::uint32_t m_a = 0;
    int m_ct = 0;
};

}
