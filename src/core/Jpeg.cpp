#include "core/Jpeg.h"

#include <algorithm>
#include <array>

namespace sashfold {

namespace {

// Zigzag position k -> natural (row-major) index.
constexpr std::array<std::uint8_t, 64> zigzag_to_natural { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

struct HuffmanTable {
    bool present = false;
    std::array<int, 17> min_code {};
    std::array<int, 18> max_code {}; // -1 where no code has that length
    std::array<int, 17> value_index {};
    std::vector<std::uint8_t> values;

    // Annex C: canonical codes from the counts per length.
    bool build(std::array<std::uint8_t, 17> const& counts, std::vector<std::uint8_t> symbols)
    {
        values = std::move(symbols);
        int code = 0;
        int index = 0;
        for (int length = 1; length <= 16; ++length) {
            value_index[static_cast<std::size_t>(length)] = index;
            min_code[static_cast<std::size_t>(length)] = code;
            int const count = counts[static_cast<std::size_t>(length)];
            code += count;
            index += count;
            max_code[static_cast<std::size_t>(length)] = count ? code - 1 : -1;
            if (code > (1 << length))
                return false; // more codes than the length can hold
            code <<= 1;
        }
        present = index == static_cast<int>(values.size());
        return present;
    }
};

struct Component {
    int id = 0;
    int h = 1; // sampling factors
    int v = 1;
    int quant_table = 0;
    int dc_table = 0;
    int ac_table = 0;
    int dc_prediction = 0;
    // The sample plane, padded to whole MCUs.
    int plane_width = 0;
    int plane_height = 0;
    std::vector<std::uint8_t> plane;
};

// The entropy-coded segment: bits most significant first, 0xFF00 unstuffed
// to 0xFF, and a marker stops the flow — the reader then feeds zeros, so a
// truncated scan decodes to gray rather than reading past the end.
class BitReader {
public:
    BitReader(std::vector<std::uint8_t> const& bytes, std::size_t at)
        : m_bytes(bytes)
        , m_pos(at)
    {
    }

    std::size_t position() const { return m_pos; }
    bool at_marker() const { return m_at_marker; }
    // True once the data ran out — at a marker or the end of the file — and
    // the reader had to feed zeros: what decoded so far is all there is.
    bool starved() const { return m_at_marker || m_padded; }

    int bit() { return bits(1); }

    int bits(int count)
    {
        while (m_count < count)
            fill();
        int const value = static_cast<int>(m_buffer >> (32 - count));
        m_buffer <<= count;
        m_count -= count;
        return value;
    }

    // Drops buffered bits and stands at the next byte: what a restart wants.
    void restart()
    {
        m_buffer = 0;
        m_count = 0;
        m_at_marker = false;
        m_padded = false;
    }

    // Consumes an RSTn marker at the current byte position; false when it is not there.
    bool take_restart_marker()
    {
        if (m_pos + 1 < m_bytes.size() && m_bytes[m_pos] == 0xFF && m_bytes[m_pos + 1] >= 0xD0
            && m_bytes[m_pos + 1] <= 0xD7) {
            m_pos += 2;
            return true;
        }
        return false;
    }

private:
    void fill()
    {
        std::uint32_t byte = 0;
        if (!m_at_marker && m_pos < m_bytes.size()) {
            byte = m_bytes[m_pos];
            if (byte == 0xFF) {
                std::uint8_t const next = m_pos + 1 < m_bytes.size() ? m_bytes[m_pos + 1] : 0xD9;
                if (next == 0x00) {
                    m_pos += 2; // a stuffed 0xFF data byte
                } else {
                    m_at_marker = true; // a marker: the segment ends here
                    byte = 0;
                }
            } else {
                ++m_pos;
            }
        } else if (!m_at_marker) {
            m_padded = true; // past the end of the file
        }
        m_buffer |= byte << (24 - m_count);
        m_count += 8;
    }

    std::vector<std::uint8_t> const& m_bytes;
    std::size_t m_pos;
    std::uint32_t m_buffer = 0;
    int m_count = 0;
    bool m_at_marker = false;
    bool m_padded = false;
};

// Decodes one Huffman symbol, reading a bit at a time down the lengths (F.2.2.3).
std::optional<int> decode_symbol(BitReader& reader, HuffmanTable const& table)
{
    int code = 0;
    for (int length = 1; length <= 16; ++length) {
        code = (code << 1) | reader.bit();
        auto const l = static_cast<std::size_t>(length);
        if (table.max_code[l] >= 0 && code <= table.max_code[l] && code >= table.min_code[l])
            return table.values[static_cast<std::size_t>(table.value_index[l] + code - table.min_code[l])];
    }
    return std::nullopt;
}

// A magnitude category and its bits to a signed value (F.2.2.1 EXTEND).
int extend(int value, int size)
{
    if (size == 0)
        return 0;
    return value < (1 << (size - 1)) ? value - (1 << size) + 1 : value;
}

// The inverse DCT in 12-bit fixed point: the AAN-derived even/odd
// decomposition every decoder uses, one pass down the columns and one
// across the rows, rounded and level-shifted at the end. Integer only.
constexpr int f2f(double value) { return static_cast<int>(value * 4096 + (value >= 0 ? 0.5 : -0.5)); }

void idct_1d(int s0, int s1, int s2, int s3, int s4, int s5, int s6, int s7, int& x0, int& x1, int& x2, int& x3,
    int& t0, int& t1, int& t2, int& t3)
{
    int p2 = s2;
    int p3 = s6;
    int p1 = (p2 + p3) * f2f(0.5411961);
    t2 = p1 + p3 * f2f(-1.847759065);
    t3 = p1 + p2 * f2f(0.765366865);
    p2 = s0;
    p3 = s4;
    t0 = (p2 + p3) * 4096;
    t1 = (p2 - p3) * 4096;
    x0 = t0 + t3;
    x3 = t0 - t3;
    x1 = t1 + t2;
    x2 = t1 - t2;
    t0 = s7;
    t1 = s5;
    t2 = s3;
    t3 = s1;
    p3 = t0 + t2;
    int p4 = t1 + t3;
    p1 = t0 + t3;
    p2 = t1 + t2;
    int const p5 = (p3 + p4) * f2f(1.175875602);
    t0 = t0 * f2f(0.298631336);
    t1 = t1 * f2f(2.053119869);
    t2 = t2 * f2f(3.072711026);
    t3 = t3 * f2f(1.501321110);
    p1 = p5 + p1 * f2f(-0.899976223);
    p2 = p5 + p2 * f2f(-2.562915447);
    p3 = p3 * f2f(-1.961570560);
    p4 = p4 * f2f(-0.390180644);
    t3 += p1 + p4;
    t2 += p2 + p3;
    t1 += p2 + p4;
    t0 += p1 + p3;
}

std::uint8_t clamp_sample(int value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

// Dequantized coefficients in natural order -> 8x8 samples at `out` with the given stride.
void idct_block(std::array<int, 64> const& in, std::uint8_t* out, int stride)
{
    std::array<int, 64> v {};
    for (int i = 0; i < 8; ++i) {
        if (in[8 + i] == 0 && in[16 + i] == 0 && in[24 + i] == 0 && in[32 + i] == 0 && in[40 + i] == 0
            && in[48 + i] == 0 && in[56 + i] == 0) {
            int const dc = in[i] * 4;
            for (int row = 0; row < 8; ++row)
                v[static_cast<std::size_t>(row * 8 + i)] = dc;
            continue;
        }
        int x0, x1, x2, x3, t0, t1, t2, t3;
        idct_1d(in[i], in[8 + i], in[16 + i], in[24 + i], in[32 + i], in[40 + i], in[48 + i], in[56 + i], x0, x1, x2,
            x3, t0, t1, t2, t3);
        x0 += 512;
        x1 += 512;
        x2 += 512;
        x3 += 512;
        v[static_cast<std::size_t>(0 * 8 + i)] = (x0 + t3) >> 10;
        v[static_cast<std::size_t>(7 * 8 + i)] = (x0 - t3) >> 10;
        v[static_cast<std::size_t>(1 * 8 + i)] = (x1 + t2) >> 10;
        v[static_cast<std::size_t>(6 * 8 + i)] = (x1 - t2) >> 10;
        v[static_cast<std::size_t>(2 * 8 + i)] = (x2 + t1) >> 10;
        v[static_cast<std::size_t>(5 * 8 + i)] = (x2 - t1) >> 10;
        v[static_cast<std::size_t>(3 * 8 + i)] = (x3 + t0) >> 10;
        v[static_cast<std::size_t>(4 * 8 + i)] = (x3 - t0) >> 10;
    }
    for (int row = 0; row < 8; ++row) {
        int const* r = &v[static_cast<std::size_t>(row * 8)];
        int x0, x1, x2, x3, t0, t1, t2, t3;
        idct_1d(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], x0, x1, x2, x3, t0, t1, t2, t3);
        // Round at 17 fractional bits and add the 128 level shift.
        int const bias = 65536 + (128 << 17);
        x0 += bias;
        x1 += bias;
        x2 += bias;
        x3 += bias;
        std::uint8_t* o = out + row * stride;
        o[0] = clamp_sample((x0 + t3) >> 17);
        o[7] = clamp_sample((x0 - t3) >> 17);
        o[1] = clamp_sample((x1 + t2) >> 17);
        o[6] = clamp_sample((x1 - t2) >> 17);
        o[2] = clamp_sample((x2 + t1) >> 17);
        o[5] = clamp_sample((x2 - t1) >> 17);
        o[3] = clamp_sample((x3 + t0) >> 17);
        o[4] = clamp_sample((x3 - t0) >> 17);
    }
}

std::uint16_t read_u16(std::vector<std::uint8_t> const& bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(bytes[at] << 8 | bytes[at + 1]);
}

} // namespace

bool looks_like_jpeg(std::vector<std::uint8_t> const& bytes)
{
    return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
}

std::optional<Bitmap> decode_jpeg(std::vector<std::uint8_t> const& bytes, std::size_t max_pixels)
{
    if (!looks_like_jpeg(bytes))
        return std::nullopt;

    std::array<std::array<std::uint16_t, 64>, 4> quant {}; // in zigzag order
    std::array<bool, 4> quant_present {};
    std::array<HuffmanTable, 4> dc_tables;
    std::array<HuffmanTable, 4> ac_tables;
    std::vector<Component> components;
    int width = 0;
    int height = 0;
    int h_max = 1;
    int v_max = 1;
    int mcus_x = 0;
    int mcus_y = 0;
    int restart_interval = 0;
    bool frame_seen = false;
    bool adobe_rgb = false; // Adobe APP14 transform 0 with three components
    bool any_scan = false;

    std::size_t at = 2;
    while (at + 4 <= bytes.size()) {
        if (bytes[at] != 0xFF) {
            ++at; // stray bytes between segments
            continue;
        }
        std::uint8_t const marker = bytes[at + 1];
        if (marker == 0xFF) {
            ++at; // fill bytes
            continue;
        }
        at += 2;
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7))
            continue; // SOI again, or a stray restart: no payload
        if (marker == 0xD9)
            break; // EOI
        if (at + 2 > bytes.size())
            return std::nullopt;
        std::size_t const length = read_u16(bytes, at);
        if (length < 2 || at + length > bytes.size())
            return std::nullopt;
        std::size_t const segment = at + 2;
        std::size_t const segment_end = at + length;
        at = segment_end;

        switch (marker) {
        case 0xDB: { // DQT
            std::size_t p = segment;
            while (p < segment_end) {
                int const precision = bytes[p] >> 4;
                int const id = bytes[p] & 0x0F;
                if (id > 3 || precision > 1)
                    return std::nullopt;
                std::size_t const entry = precision ? 2 : 1;
                if (p + 1 + 64 * entry > segment_end)
                    return std::nullopt;
                for (int k = 0; k < 64; ++k) {
                    std::size_t const q = p + 1 + static_cast<std::size_t>(k) * entry;
                    quant[static_cast<std::size_t>(id)][static_cast<std::size_t>(k)]
                        = precision ? read_u16(bytes, q) : bytes[q];
                }
                quant_present[static_cast<std::size_t>(id)] = true;
                p += 1 + 64 * entry;
            }
            break;
        }
        case 0xC4: { // DHT
            std::size_t p = segment;
            while (p + 17 <= segment_end) {
                int const table_class = bytes[p] >> 4;
                int const id = bytes[p] & 0x0F;
                if (table_class > 1 || id > 3)
                    return std::nullopt;
                std::array<std::uint8_t, 17> counts {};
                std::size_t total = 0;
                for (int code_length = 1; code_length <= 16; ++code_length) {
                    counts[static_cast<std::size_t>(code_length)] = bytes[p + static_cast<std::size_t>(code_length)];
                    total += counts[static_cast<std::size_t>(code_length)];
                }
                if (total > 256 || p + 17 + total > segment_end)
                    return std::nullopt;
                std::vector<std::uint8_t> symbols(bytes.begin() + static_cast<std::ptrdiff_t>(p + 17),
                    bytes.begin() + static_cast<std::ptrdiff_t>(p + 17 + total));
                HuffmanTable& table = table_class == 0 ? dc_tables[static_cast<std::size_t>(id)]
                                                       : ac_tables[static_cast<std::size_t>(id)];
                if (!table.build(counts, std::move(symbols)))
                    return std::nullopt;
                p += 17 + total;
            }
            break;
        }
        case 0xDD: // DRI
            if (length < 4)
                return std::nullopt;
            restart_interval = read_u16(bytes, segment);
            break;
        case 0xEE: // APP14: Adobe's color transform flag
            if (length >= 14 && bytes[segment] == 'A' && bytes[segment + 1] == 'd' && bytes[segment + 2] == 'o'
                && bytes[segment + 3] == 'b' && bytes[segment + 4] == 'e')
                adobe_rgb = bytes[segment + 11] == 0;
            break;
        case 0xC0:
        case 0xC1: { // SOF0 baseline, SOF1 extended sequential: the same decoding at 8 bits
            if (frame_seen || length < 8)
                return std::nullopt;
            int const precision = bytes[segment];
            height = read_u16(bytes, segment + 1);
            width = read_u16(bytes, segment + 3);
            int const count = bytes[segment + 5];
            if (precision != 8 || width == 0 || height == 0 || (count != 1 && count != 3)
                || static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > max_pixels
                || length < 8 + static_cast<std::size_t>(count) * 3)
                return std::nullopt;
            for (int i = 0; i < count; ++i) {
                std::size_t const c = segment + 6 + static_cast<std::size_t>(i) * 3;
                Component component;
                component.id = bytes[c];
                component.h = bytes[c + 1] >> 4;
                component.v = bytes[c + 1] & 0x0F;
                component.quant_table = bytes[c + 2];
                if (component.h < 1 || component.h > 4 || component.v < 1 || component.v > 4
                    || component.quant_table > 3)
                    return std::nullopt;
                h_max = std::max(h_max, component.h);
                v_max = std::max(v_max, component.v);
                components.push_back(component);
            }
            mcus_x = (width + 8 * h_max - 1) / (8 * h_max);
            mcus_y = (height + 8 * v_max - 1) / (8 * v_max);
            for (Component& component : components) {
                component.plane_width = mcus_x * component.h * 8;
                component.plane_height = mcus_y * component.v * 8;
                component.plane.assign(static_cast<std::size_t>(component.plane_width)
                        * static_cast<std::size_t>(component.plane_height),
                    128);
            }
            frame_seen = true;
            break;
        }
        case 0xC2:
        case 0xC3:
        case 0xC5:
        case 0xC6:
        case 0xC7:
        case 0xC9:
        case 0xCA:
        case 0xCB:
        case 0xCD:
        case 0xCE:
        case 0xCF:
            return std::nullopt; // progressive, lossless, hierarchical, arithmetic: declined
        case 0xDA: { // SOS: the scan header, then the entropy-coded data
            if (!frame_seen || length < 6)
                return std::nullopt;
            int const scan_count = bytes[segment];
            if (scan_count < 1 || scan_count > static_cast<int>(components.size())
                || length < 6 + static_cast<std::size_t>(scan_count) * 2)
                return std::nullopt;
            std::vector<Component*> scan;
            for (int i = 0; i < scan_count; ++i) {
                std::size_t const s = segment + 1 + static_cast<std::size_t>(i) * 2;
                int const id = bytes[s];
                Component* found = nullptr;
                for (Component& component : components) {
                    if (component.id == id)
                        found = &component;
                }
                if (!found)
                    return std::nullopt;
                found->dc_table = bytes[s + 1] >> 4;
                found->ac_table = bytes[s + 1] & 0x0F;
                if (found->dc_table > 3 || found->ac_table > 3
                    || !dc_tables[static_cast<std::size_t>(found->dc_table)].present
                    || !ac_tables[static_cast<std::size_t>(found->ac_table)].present
                    || !quant_present[static_cast<std::size_t>(found->quant_table)])
                    return std::nullopt;
                scan.push_back(found);
            }
            // Ss, Se, Ah/Al: baseline has one value each; anything else is progressive.
            std::size_t const s = segment + 1 + static_cast<std::size_t>(scan_count) * 2;
            if (bytes[s] != 0 || bytes[s + 1] != 63 || bytes[s + 2] != 0)
                return std::nullopt;

            for (Component& component : components)
                component.dc_prediction = 0;
            BitReader reader(bytes, segment_end);
            any_scan = true;
            auto const decode_block = [&](Component& component, int block_x, int block_y) {
                std::array<int, 64> coefficients {};
                std::array<std::uint16_t, 64> const& q = quant[static_cast<std::size_t>(component.quant_table)];
                std::optional<int> const dc_size
                    = decode_symbol(reader, dc_tables[static_cast<std::size_t>(component.dc_table)]);
                if (!dc_size || *dc_size > 11)
                    return false;
                int const diff = extend(*dc_size ? reader.bits(*dc_size) : 0, *dc_size);
                component.dc_prediction += diff;
                coefficients[0] = component.dc_prediction * q[0];
                for (int k = 1; k < 64;) {
                    std::optional<int> const symbol
                        = decode_symbol(reader, ac_tables[static_cast<std::size_t>(component.ac_table)]);
                    if (!symbol)
                        return false;
                    int const run = *symbol >> 4;
                    int const size = *symbol & 0x0F;
                    if (size == 0) {
                        if (run != 15)
                            break; // end of block
                        k += 16;
                        continue;
                    }
                    k += run;
                    if (k > 63)
                        return false;
                    coefficients[zigzag_to_natural[static_cast<std::size_t>(k)]]
                        = extend(reader.bits(size), size) * q[static_cast<std::size_t>(k)];
                    ++k;
                }
                if (block_x * 8 + 8 > component.plane_width || block_y * 8 + 8 > component.plane_height)
                    return true; // a block past the plane: nothing to place
                idct_block(coefficients,
                    component.plane.data() + static_cast<std::size_t>(block_y) * 8 * static_cast<std::size_t>(component.plane_width)
                        + static_cast<std::size_t>(block_x) * 8,
                    component.plane_width);
                return true;
            };

            // Non-interleaved scans walk one component's own block grid;
            // interleaved scans walk MCUs.
            int units_x;
            int units_y;
            if (scan.size() == 1) {
                Component const& only = *scan[0];
                units_x = (width * only.h / h_max + 7) / 8;
                units_y = (height * only.v / v_max + 7) / 8;
                units_x = std::max(units_x, ((width * only.h + h_max - 1) / h_max + 7) / 8);
                units_y = std::max(units_y, ((height * only.v + v_max - 1) / v_max + 7) / 8);
            } else {
                units_x = mcus_x;
                units_y = mcus_y;
            }
            bool stopped = false;
            int until_restart = restart_interval;
            for (int uy = 0; uy < units_y && !stopped; ++uy) {
                for (int ux = 0; ux < units_x && !stopped; ++ux) {
                    if (restart_interval && until_restart == 0) {
                        reader.restart();
                        if (!reader.take_restart_marker()) {
                            stopped = true; // out of step: keep what decoded
                            break;
                        }
                        for (Component* component : scan)
                            component->dc_prediction = 0;
                        until_restart = restart_interval;
                    }
                    if (scan.size() == 1) {
                        if (!decode_block(*scan[0], ux, uy))
                            stopped = true;
                    } else {
                        for (Component* component : scan) {
                            for (int v = 0; v < component->v && !stopped; ++v) {
                                for (int h = 0; h < component->h && !stopped; ++h) {
                                    if (!decode_block(*component, ux * component->h + h, uy * component->v + v))
                                        stopped = true;
                                }
                            }
                        }
                    }
                    // Data that ran out mid-scan: the rest of the picture stays gray.
                    if (reader.starved())
                        stopped = true;
                    if (restart_interval)
                        --until_restart;
                }
            }
            // Resume at the byte after the scan's data: from the reader's
            // position, skip to the next marker.
            at = reader.position();
            while (at + 1 < bytes.size() && !(bytes[at] == 0xFF && bytes[at + 1] != 0 && !(bytes[at + 1] >= 0xD0 && bytes[at + 1] <= 0xD7)))
                ++at;
            break;
        }
        default:
            break; // APPn, COM, DNL and the rest: skipped
        }
    }
    if (!frame_seen || !any_scan)
        return std::nullopt;

    // Samples to pixels: each component read at its own sampling, nearest.
    Bitmap out(width, height, Color::rgb(0, 0, 0));
    bool const color = components.size() == 3;
    bool const ycbcr = color && !adobe_rgb
        && !(components[0].id == 'R' && components[1].id == 'G' && components[2].id == 'B');
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto const sample = [&](Component const& component) {
                int const sx = x * component.h / h_max;
                int const sy = y * component.v / v_max;
                return static_cast<int>(component.plane[static_cast<std::size_t>(sy) * static_cast<std::size_t>(component.plane_width)
                    + static_cast<std::size_t>(sx)]);
            };
            if (!color) {
                auto const g = static_cast<std::uint8_t>(sample(components[0]));
                out.set_pixel(x, y, Color::rgb(g, g, g));
                continue;
            }
            int const c0 = sample(components[0]);
            int const c1 = sample(components[1]);
            int const c2 = sample(components[2]);
            if (!ycbcr) {
                out.set_pixel(x, y, Color::rgb(static_cast<std::uint8_t>(c0), static_cast<std::uint8_t>(c1),
                    static_cast<std::uint8_t>(c2)));
                continue;
            }
            // ITU-R BT.601 in 16.16 fixed point, rounded.
            int const cb = c1 - 128;
            int const cr = c2 - 128;
            int const r = c0 + ((91881 * cr + 32768) >> 16);
            int const g = c0 - ((22554 * cb + 46802 * cr + 32768) >> 16);
            int const b = c0 + ((116130 * cb + 32768) >> 16);
            out.set_pixel(x, y, Color::rgb(clamp_sample(r), clamp_sample(g), clamp_sample(b)));
        }
    }
    return out;
}

}
