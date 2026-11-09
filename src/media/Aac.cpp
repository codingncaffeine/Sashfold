#include "media/Aac.h"

#include "media/AacData.h"
#include "media/Sbr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <numbers>

namespace sashfold::media {

namespace {

constexpr int k_frame = 1024;
constexpr int k_short = 128;
constexpr int k_max_sfb = 64;
constexpr int k_max_channels = 8;

// --- Bits ---------------------------------------------------------------------

class Bits {
public:
    explicit Bits(std::span<std::uint8_t const> data)
        : m_data(data)
    {
    }
    bool failed() const { return m_failed; }
    std::size_t position() const { return m_position; }
    std::size_t left() const { return m_data.size() * 8 > m_position ? m_data.size() * 8 - m_position : 0; }
    std::uint32_t u(int n)
    {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | bit();
        return v;
    }
    bool flag() { return u(1) != 0; }
    std::uint32_t peek(int n) const
    {
        std::uint32_t v = 0;
        std::size_t p = m_position;
        for (int i = 0; i < n; ++i, ++p)
            v = (v << 1) | (p < m_data.size() * 8 ? (m_data[p >> 3] >> (7 - (p & 7))) & 1 : 0);
        return v;
    }
    void skip(std::size_t n) { m_position += n; }
    void align() { m_position = (m_position + 7) & ~std::size_t { 7 }; }

private:
    std::uint32_t bit()
    {
        if (m_position >= m_data.size() * 8) {
            m_failed = true;
            ++m_position;
            return 0;
        }
        std::uint32_t const b = (m_data[m_position >> 3] >> (7 - (m_position & 7))) & 1;
        ++m_position;
        return b;
    }
    std::span<std::uint8_t const> m_data;
    std::size_t m_position = 0;
    bool m_failed = false;
};

// A codebook as a lookup by codeword: read bit by bit, longest 19.
struct BookLookup {
    aac_data::HuffmanBook const* book = nullptr;
    // By (length, code): entries sorted by length then code, searched by
    // stepping through lengths.
    std::vector<aac_data::HuffmanEntry> sorted;
    std::array<std::size_t, 21> first_of_length {}; // index into sorted where each length begins

    explicit BookLookup(aac_data::HuffmanBook const& b)
        : book(&b)
        , sorted(b.entries, b.entries + b.count)
    {
        std::sort(sorted.begin(), sorted.end(), [](auto const& x, auto const& y) {
            return x.length != y.length ? x.length < y.length : x.code < y.code;
        });
        std::size_t at = 0;
        for (int length = 0; length <= 20; ++length) {
            first_of_length[static_cast<std::size_t>(length)] = at;
            while (at < sorted.size() && sorted[at].length == length)
                ++at;
        }
    }

    // The index the next codeword stands for; -1 when none matches.
    int read(Bits& bits) const
    {
        std::uint32_t code = 0;
        for (int length = 1; length <= 19; ++length) {
            code = (code << 1) | bits.u(1);
            std::size_t const from = first_of_length[static_cast<std::size_t>(length)];
            std::size_t const to = first_of_length[static_cast<std::size_t>(length) + 1];
            auto const it = std::lower_bound(sorted.begin() + static_cast<std::ptrdiff_t>(from), sorted.begin() + static_cast<std::ptrdiff_t>(to), code,
                [](aac_data::HuffmanEntry const& e, std::uint32_t c) { return e.code < c; });
            if (it != sorted.begin() + static_cast<std::ptrdiff_t>(to) && it->code == code)
                return it->index;
            if (bits.failed())
                return -1;
        }
        return -1;
    }
};

std::array<BookLookup, 12> const& lookups()
{
    static std::array<BookLookup, 12> const tables { BookLookup(aac_data::books[0]), BookLookup(aac_data::books[1]), BookLookup(aac_data::books[2]),
        BookLookup(aac_data::books[3]), BookLookup(aac_data::books[4]), BookLookup(aac_data::books[5]), BookLookup(aac_data::books[6]),
        BookLookup(aac_data::books[7]), BookLookup(aac_data::books[8]), BookLookup(aac_data::books[9]), BookLookup(aac_data::books[10]),
        BookLookup(aac_data::books[11]) };
    return tables;
}

// --- Windows and the inverse MDCT ----------------------------------------------

// Kaiser-Bessel-derived window halves (4.6.11.3.2), alpha 4 for long
// windows and 6 for short, and sine halves; each N/2 values.
std::vector<float> kbd_half(int n, double alpha)
{
    int const half = n / 2;
    auto const bessel_i0 = [](double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 50; ++k) {
            term *= (x / (2 * k)) * (x / (2 * k));
            sum += term;
            if (term < sum * 1e-17)
                break;
        }
        return sum;
    };
    std::vector<double> kaiser(static_cast<std::size_t>(half) + 1);
    double total = 0;
    for (int j = 0; j <= half; ++j) {
        double const r = 2.0 * j / half - 1.0;
        kaiser[static_cast<std::size_t>(j)] = bessel_i0(std::numbers::pi * alpha * std::sqrt(std::max(0.0, 1 - r * r)));
        total += kaiser[static_cast<std::size_t>(j)];
    }
    std::vector<float> out(static_cast<std::size_t>(half));
    double running = 0;
    for (int j = 0; j < half; ++j) {
        running += kaiser[static_cast<std::size_t>(j)];
        out[static_cast<std::size_t>(j)] = static_cast<float>(std::sqrt(running / total));
    }
    return out;
}

std::vector<float> sine_half(int n)
{
    std::vector<float> out(static_cast<std::size_t>(n / 2));
    for (int i = 0; i < n / 2; ++i)
        out[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(std::numbers::pi * (i + 0.5) / n));
    return out;
}

struct Windows {
    std::vector<float> long_sine = sine_half(2 * k_frame);
    std::vector<float> long_kbd = kbd_half(2 * k_frame, 4);
    std::vector<float> short_sine = sine_half(2 * k_short);
    std::vector<float> short_kbd = kbd_half(2 * k_short, 6);
    std::vector<float> const& half(bool kbd, bool is_short) const
    {
        return is_short ? (kbd ? short_kbd : short_sine) : (kbd ? long_kbd : long_sine);
    }
};

Windows const& windows()
{
    static Windows const w;
    return w;
}

// The inverse MDCT of 4.6.11.3.1, through a DCT-IV of half its size (M
// = N/2): cos(π(2j+1)(2k+1)/(4M)) repeats every 8M in the product, so one
// table of 8M cosines serves, and the three parts of the output are the
// DCT-IV read forwards, backwards and negated as the transform's symmetry
// has it (checked against the direct sum: 3e-8 relative).
class Imdct {
public:
    explicit Imdct(int n)
        : m_n(n)
        , m_cos(static_cast<std::size_t>(4 * n))
    {
        int const m = n / 2;
        for (int i = 0; i < 8 * m; ++i)
            m_cos[static_cast<std::size_t>(i)] = static_cast<float>(std::cos(2 * std::numbers::pi * i / (8 * m)));
    }

    // `in` has N/2 coefficients, `out` takes N samples.
    void run(float const* in, float* out) const
    {
        int const m = m_n / 2;
        int const mask = 8 * m - 1;
        std::vector<float> u(static_cast<std::size_t>(m));
        float const scale = 2.0f / static_cast<float>(m_n);
        for (int j = 0; j < m; ++j) {
            double sum = 0;
            int const step = 2 * j + 1;
            int index = step; // (2j+1)(2k+1) at k = 0, then + 2 * step each k
            for (int k = 0; k < m; ++k) {
                sum += static_cast<double>(in[k]) * static_cast<double>(m_cos[static_cast<std::size_t>(index & mask)]);
                index += 2 * step;
            }
            u[static_cast<std::size_t>(j)] = static_cast<float>(sum) * scale;
        }
        int const q = m / 2; // N/4
        for (int i = 0; i < q; ++i)
            out[i] = u[static_cast<std::size_t>(i + q)];
        for (int i = q; i < 3 * q; ++i)
            out[i] = -u[static_cast<std::size_t>(3 * q - 1 - i)];
        for (int i = 3 * q; i < 4 * q; ++i)
            out[i] = -u[static_cast<std::size_t>(i - 3 * q)];
    }

private:
    int m_n;
    std::vector<float> m_cos;
};

Imdct const& imdct_long()
{
    static Imdct const t(2 * k_frame);
    return t;
}
Imdct const& imdct_short()
{
    static Imdct const t(2 * k_short);
    return t;
}

// --- The syntax ------------------------------------------------------------------

enum WindowSequence { OnlyLong = 0,
    LongStart = 1,
    EightShort = 2,
    LongStop = 3 };

enum : int { BookZero = 0,
    BookNoise = 13,
    BookIntensity2 = 14,
    BookIntensity = 15 };

struct Section {
    int book = 0;
    int start = 0; // sfb
    int end = 0;
};

struct Tns {
    struct Filter {
        int length = 0; // bands
        int order = 0;
        bool backward = false;
        std::array<float, 12> coefficients {}; // parcor, as sent (dequantized)
    };
    std::array<std::vector<Filter>, 8> filters; // per window
    bool present = false;
};

struct Ics {
    int global_gain = 0;
    int window_sequence = OnlyLong;
    bool window_shape_kbd = false;
    int max_sfb = 0;
    int num_windows = 1;
    int num_window_groups = 1;
    std::array<int, 8> group_length {};
    std::array<std::vector<Section>, 8> sections; // per group
    std::array<std::array<int, k_max_sfb>, 8> scalefactors {}; // per group, per sfb: scalefactor, noise energy or intensity position
    std::array<std::array<int, k_max_sfb>, 8> books {}; // per group, per sfb
    Tns tns;
    // Pulses (4.6.3.3).
    int pulse_count = 0;
    int pulse_start_sfb = 0;
    std::array<int, 4> pulse_offset {};
    std::array<int, 4> pulse_amp {};
    std::array<float, k_frame> spectrum {}; // dequantized, per window in order
};

struct ChannelState {
    std::array<float, k_frame> overlap {};
    bool previous_kbd = false;
    bool have_previous = false;
};

} // namespace

// --- Configuration ----------------------------------------------------------------

namespace {

// A program_config_element (4.4.1.1): the channels its front, side and back
// elements and its LFEs make, nothing when it cannot be read.
std::optional<int> read_program_config(Bits& bits)
{
    bits.u(4); // element_instance_tag
    bits.u(2); // object_type
    bits.u(4); // sampling_frequency_index
    int const front = static_cast<int>(bits.u(4));
    int const side = static_cast<int>(bits.u(4));
    int const back = static_cast<int>(bits.u(4));
    int const lfe = static_cast<int>(bits.u(2));
    int const assoc = static_cast<int>(bits.u(3));
    int const cc = static_cast<int>(bits.u(4));
    if (bits.flag())
        bits.u(4); // mono_mixdown_element_number
    if (bits.flag())
        bits.u(4); // stereo_mixdown_element_number
    if (bits.flag())
        bits.u(3); // matrix_mixdown_idx, pseudo_surround_enable
    int channels = 0;
    for (int i = 0; i < front + side + back; ++i) {
        channels += bits.flag() ? 2 : 1;
        bits.u(4);
    }
    channels += lfe;
    bits.skip(static_cast<std::size_t>(lfe) * 4 + static_cast<std::size_t>(assoc) * 4 + static_cast<std::size_t>(cc) * 5);
    bits.align();
    int const comment = static_cast<int>(bits.u(8));
    bits.skip(static_cast<std::size_t>(comment) * 8);
    if (bits.failed() || channels <= 0)
        return std::nullopt;
    return channels;
}

} // namespace

std::optional<AacConfig> parse_aac_config(std::span<std::uint8_t const> config)
{
    static constexpr int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
    Bits bits(config);
    auto const read_object_type = [&]() {
        int type = static_cast<int>(bits.u(5));
        if (type == 31)
            type = 32 + static_cast<int>(bits.u(6));
        return type;
    };
    auto const read_rate = [&]() {
        int const index = static_cast<int>(bits.u(4));
        return index == 15 ? static_cast<int>(bits.u(24)) : rates[index];
    };
    AacConfig out;
    out.object_type = read_object_type();
    out.sample_rate = read_rate();
    out.channel_configuration = static_cast<int>(bits.u(4));
    if (out.object_type == 5 || out.object_type == 29) {
        // SBR (and PS) signalled explicitly: the core's own type follows.
        out.sbr_signalled = true;
        out.ps_signalled = out.object_type == 29;
        out.extension_sample_rate = read_rate();
        out.object_type = read_object_type();
    }
    if (bits.failed() || out.object_type != 2 || out.sample_rate <= 0)
        return std::nullopt;
    // GASpecificConfig (4.4.1).
    out.frame_960 = bits.flag();
    if (bits.flag()) // dependsOnCoreCoder
        bits.u(14);
    bool const extension = bits.flag();
    (void)extension;
    if (out.channel_configuration == 0) {
        // The layout in a program config element: taken as the standard
        // layout of as many channels (front, then side and back, then LFE,
        // in the order the elements come).
        std::optional<int> const channels = read_program_config(bits);
        if (!channels || *channels > 8 || *channels == 7)
            return std::nullopt;
        out.channels = *channels;
        out.channel_configuration = *channels == 8 ? 7 : *channels;
    } else if (out.channel_configuration > 7) {
        return std::nullopt;
    } else {
        out.channels = out.channel_configuration == 7 ? 8 : out.channel_configuration;
    }
    // Backward-compatible signalling after the core's configuration
    // (1.6.6): SBR's presence and output rate, and parametric stereo's.
    if (!out.sbr_signalled && bits.left() >= 16 && bits.peek(11) == 0x2B7) {
        bits.u(11);
        if (static_cast<int>(bits.u(5)) == 5 && bits.flag()) {
            out.sbr_signalled = true;
            out.extension_sample_rate = read_rate();
            // And parametric stereo's after it.
            if (bits.left() >= 12 && bits.peek(11) == 0x548) {
                bits.u(11);
                out.ps_signalled = bits.flag();
            }
        }
    }
    // Parametric stereo is a single channel's.
    out.ps_signalled = out.ps_signalled && out.channels == 1;
    if (out.frame_960)
        return std::nullopt;
    bool known_rate = false;
    for (std::size_t i = 0; i < aac_data::band_table_count; ++i)
        known_rate = known_rate || static_cast<int>(aac_data::band_tables[i].sampling_rate) == out.sample_rate;
    if (!known_rate)
        return std::nullopt;
    return out;
}

// --- The decoder --------------------------------------------------------------------

struct AacDecoder::Impl {
    AacConfig config;
    aac_data::BandTable const* bands = nullptr;
    std::array<ChannelState, k_max_channels> state {};
    std::uint32_t noise_seed = 0x2545F491;
    // Spectral band replication, one decoder per channel element (by the
    // element's place in the block), once the configuration says or the
    // stream shows there is any: every channel then comes out doubled.
    std::map<int, std::unique_ptr<SbrDecoder>> sbr;
    bool sbr_output = false;
    // A single channel that parametric stereo makes two, once the
    // configuration says or the stream shows it.
    bool ps_output = false;
    int output_channels() const { return ps_output ? 2 : config.channels; }
    // The configuration's SBR output rate is the core's own: the downsampled
    // bank, at the core's rate (4.6.18.4.3).
    bool downsampled() const { return config.sbr_signalled && config.extension_sample_rate == config.sample_rate; }
    int sbr_frames() const { return downsampled() ? k_frame : 2 * k_frame; }

    explicit Impl(AacConfig const& c)
        : config(c)
        , sbr_output(c.sbr_signalled)
        , ps_output(c.ps_signalled)
    {
        for (std::size_t i = 0; i < aac_data::band_table_count; ++i) {
            if (static_cast<int>(aac_data::band_tables[i].sampling_rate) == config.sample_rate)
                bands = &aac_data::band_tables[i];
        }
    }

    std::uint16_t const* offsets(Ics const& ics) const { return ics.window_sequence == EightShort ? bands->short_offsets : bands->long_offsets; }
    int band_count(Ics const& ics) const { return ics.window_sequence == EightShort ? bands->short_bands : bands->long_bands; }

    // 4.4.2.1: the ics_info.
    bool read_ics_info(Bits& bits, Ics& ics)
    {
        bits.u(1); // ics_reserved_bit
        ics.window_sequence = static_cast<int>(bits.u(2));
        ics.window_shape_kbd = bits.flag();
        ics.group_length.fill(0);
        if (ics.window_sequence == EightShort) {
            ics.max_sfb = static_cast<int>(bits.u(4));
            std::uint32_t const grouping = bits.u(7);
            ics.num_windows = 8;
            ics.num_window_groups = 1;
            ics.group_length[0] = 1;
            for (int w = 1; w < 8; ++w) {
                if (grouping & (1u << (7 - w))) {
                    ics.group_length[static_cast<std::size_t>(ics.num_window_groups) - 1]++;
                } else {
                    ics.group_length[static_cast<std::size_t>(ics.num_window_groups)] = 1;
                    ics.num_window_groups++;
                }
            }
        } else {
            ics.max_sfb = static_cast<int>(bits.u(6));
            ics.num_windows = 1;
            ics.num_window_groups = 1;
            ics.group_length[0] = 1;
            if (bits.flag())
                return false; // predictor_data_present: not AAC-LC
        }
        return ics.max_sfb <= band_count(ics) && !bits.failed();
    }

    // 4.4.2.7: the sections, each a codebook over a run of bands.
    bool read_sections(Bits& bits, Ics& ics)
    {
        int const length_bits = ics.window_sequence == EightShort ? 3 : 5;
        int const escape = (1 << length_bits) - 1;
        for (int g = 0; g < ics.num_window_groups; ++g) {
            auto& sections = ics.sections[static_cast<std::size_t>(g)];
            sections.clear();
            int k = 0;
            while (k < ics.max_sfb) {
                Section s;
                s.book = static_cast<int>(bits.u(4));
                int length = 0;
                int increment;
                while ((increment = static_cast<int>(bits.u(length_bits))) == escape)
                    length += escape;
                length += increment;
                s.start = k;
                s.end = k + length;
                if (s.end > ics.max_sfb || bits.failed())
                    return false;
                for (int sfb = s.start; sfb < s.end; ++sfb)
                    ics.books[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)] = s.book;
                sections.push_back(s);
                k = s.end;
            }
            for (int sfb = ics.max_sfb; sfb < k_max_sfb; ++sfb)
                ics.books[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)] = BookZero;
        }
        return true;
    }

    // 4.4.2.8: the scalefactors, each a difference from the one before.
    bool read_scalefactors(Bits& bits, Ics& ics)
    {
        BookLookup const& scl = lookups()[11];
        int scalefactor = ics.global_gain;
        int noise_energy = ics.global_gain - 90;
        int intensity_position = 0;
        bool first_noise = true;
        for (int g = 0; g < ics.num_window_groups; ++g) {
            for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
                int const book = ics.books[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)];
                int& out = ics.scalefactors[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)];
                if (book == BookZero) {
                    out = 0;
                    continue;
                }
                if (book == BookIntensity || book == BookIntensity2) {
                    int const delta = scl.read(bits);
                    if (delta < 0)
                        return false;
                    intensity_position += delta - 60;
                    out = intensity_position;
                    continue;
                }
                if (book == BookNoise) {
                    if (first_noise) {
                        first_noise = false;
                        noise_energy += static_cast<int>(bits.u(9)) - 256;
                    } else {
                        int const delta = scl.read(bits);
                        if (delta < 0)
                            return false;
                        noise_energy += delta - 60;
                    }
                    out = noise_energy;
                    continue;
                }
                int const delta = scl.read(bits);
                if (delta < 0)
                    return false;
                scalefactor += delta - 60;
                if (scalefactor < 0 || scalefactor > 255)
                    return false;
                out = scalefactor;
            }
        }
        return !bits.failed();
    }

    // 4.4.2.9 and 4.4.2.10: the pulses and TNS.
    bool read_pulses(Bits& bits, Ics& ics)
    {
        ics.pulse_count = static_cast<int>(bits.u(2)) + 1;
        ics.pulse_start_sfb = static_cast<int>(bits.u(6));
        if (ics.window_sequence == EightShort || ics.pulse_start_sfb > band_count(ics))
            return false;
        for (int i = 0; i < ics.pulse_count; ++i) {
            ics.pulse_offset[static_cast<std::size_t>(i)] = static_cast<int>(bits.u(5));
            ics.pulse_amp[static_cast<std::size_t>(i)] = static_cast<int>(bits.u(4));
        }
        return !bits.failed();
    }

    bool read_tns(Bits& bits, Ics& ics)
    {
        bool const is_short = ics.window_sequence == EightShort;
        ics.tns.present = true;
        for (int w = 0; w < ics.num_windows; ++w) {
            auto& filters = ics.tns.filters[static_cast<std::size_t>(w)];
            filters.clear();
            int const n_filt = static_cast<int>(bits.u(is_short ? 1 : 2));
            int coef_res = 0;
            if (n_filt)
                coef_res = static_cast<int>(bits.u(1));
            for (int f = 0; f < n_filt; ++f) {
                Tns::Filter filter;
                filter.length = static_cast<int>(bits.u(is_short ? 4 : 6));
                filter.order = static_cast<int>(bits.u(is_short ? 3 : 5));
                if (filter.order > (is_short ? 7 : 12))
                    return false;
                if (filter.order) {
                    filter.backward = bits.flag();
                    bool const compress = bits.flag();
                    int const resolution = coef_res + 3; // bits of the coefficients' scale
                    int const coef_bits = resolution - (compress ? 1 : 0);
                    // 4.6.9.3: the sent values back to parcor coefficients.
                    double const iqfac = ((1 << (resolution - 1)) - 0.5) / (std::numbers::pi / 2);
                    double const iqfac_m = ((1 << (resolution - 1)) + 0.5) / (std::numbers::pi / 2);
                    for (int i = 0; i < filter.order; ++i) {
                        int value = static_cast<int>(bits.u(coef_bits));
                        if (value & (1 << (coef_bits - 1)))
                            value -= 1 << coef_bits; // two's complement in coef_bits bits
                        filter.coefficients[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(value / (value >= 0 ? iqfac : iqfac_m)));
                    }
                }
                filters.push_back(filter);
            }
        }
        return !bits.failed();
    }

    // 4.4.2.11: the spectrum, codeword by codeword, into each group's
    // interleaved order (band by band, window by window within it).
    bool read_spectrum(Bits& bits, Ics& ics, std::array<int, k_frame>& quantized)
    {
        quantized.fill(0);
        std::uint16_t const* const swb = offsets(ics);
        int window_base = 0; // the first window of the group
        for (int g = 0; g < ics.num_window_groups; ++g) {
            int const group_windows = ics.group_length[static_cast<std::size_t>(g)];
            // The group's coefficients begin at window_base * 128 (short) or 0 (long).
            int const base = ics.window_sequence == EightShort ? window_base * k_short : 0;
            for (Section const& s : ics.sections[static_cast<std::size_t>(g)]) {
                if (s.book == BookZero || s.book == BookNoise || s.book == BookIntensity || s.book == BookIntensity2)
                    continue;
                if (s.book > 11)
                    return false;
                BookLookup const& lookup = lookups()[static_cast<std::size_t>(s.book) - 1];
                aac_data::HuffmanBook const& book = *lookup.book;
                int const from = swb[s.start] * group_windows;
                int const to = swb[s.end] * group_windows;
                for (int k = from; k < to; k += book.dimension) {
                    int const index = lookup.read(bits);
                    if (index < 0)
                        return false;
                    int values[4];
                    int const mask = (1 << book.bits) - 1;
                    int packed = index;
                    for (int d = 0; d < book.dimension; ++d) {
                        values[d] = (packed & mask) - book.offset;
                        packed >>= book.bits;
                    }
                    if (book.offset == 0) {
                        // Unsigned codebooks: a sign bit after each value that is not zero.
                        for (int d = 0; d < book.dimension; ++d) {
                            if (values[d] != 0 && bits.flag())
                                values[d] = -values[d];
                        }
                    }
                    if (s.book == 11) {
                        // 4.6.3.3: an escape for a magnitude of 16, N ones, a
                        // zero and N + 4 bits, 2^(N+4) and those.
                        for (int d = 0; d < 2; ++d) {
                            if (std::abs(values[d]) != 16)
                                continue;
                            int ones = 0;
                            while (bits.u(1) == 1 && ones < 13)
                                ++ones;
                            int const magnitude = (1 << (ones + 4)) + static_cast<int>(bits.u(ones + 4));
                            values[d] = values[d] < 0 ? -magnitude : magnitude;
                        }
                    }
                    for (int d = 0; d < book.dimension; ++d) {
                        int const position = base + k + d;
                        if (position >= k_frame)
                            return false;
                        quantized[static_cast<std::size_t>(position)] = values[d];
                    }
                }
            }
            window_base += group_windows;
        }
        return !bits.failed();
    }

    // 4.6.2: quantized values to the spectrum, the scalefactors applied, and
    // each group's interleaving undone into window order.
    void dequantize(Ics& ics, std::array<int, k_frame> const& quantized)
    {
        std::uint16_t const* const swb = offsets(ics);
        bool const is_short = ics.window_sequence == EightShort;
        ics.spectrum.fill(0);
        // Pulses go into the quantized values of a long window (4.6.3.3).
        std::array<int, k_frame> q = quantized;
        if (ics.pulse_count > 0 && !is_short) {
            int k = swb[ics.pulse_start_sfb];
            for (int i = 0; i < ics.pulse_count; ++i) {
                k += ics.pulse_offset[static_cast<std::size_t>(i)];
                if (k >= k_frame)
                    break;
                int const amp = ics.pulse_amp[static_cast<std::size_t>(i)];
                q[static_cast<std::size_t>(k)] += q[static_cast<std::size_t>(k)] > 0 ? amp : -amp;
            }
        }
        int window_base = 0;
        for (int g = 0; g < ics.num_window_groups; ++g) {
            int const group_windows = ics.group_length[static_cast<std::size_t>(g)];
            int const base = is_short ? window_base * k_short : 0;
            for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
                int const book = ics.books[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)];
                if (book == BookZero || book == BookNoise || book == BookIntensity || book == BookIntensity2)
                    continue;
                int const sf = ics.scalefactors[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)];
                float const gain = static_cast<float>(std::pow(2.0, 0.25 * (sf - 100)));
                int const width = swb[sfb + 1] - swb[sfb];
                for (int w = 0; w < group_windows; ++w) {
                    int const in_at = base + swb[sfb] * group_windows + w * width;
                    int const out_at = base + w * k_short + swb[sfb];
                    for (int i = 0; i < width; ++i) {
                        int const v = q[static_cast<std::size_t>(in_at + i)];
                        float const magnitude = static_cast<float>(std::pow(static_cast<double>(std::abs(v)), 4.0 / 3.0));
                        ics.spectrum[static_cast<std::size_t>(out_at + i)] = (v < 0 ? -magnitude : magnitude) * gain;
                    }
                }
            }
            window_base += group_windows;
        }
    }

    // 4.6.13: noise where the encoder sent none, at the energy it said.
    float noise()
    {
        noise_seed = noise_seed * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<std::int32_t>(noise_seed)) / 2147483648.0f;
    }
    void fill_noise(Ics& ics, int g, int sfb, int window_base)
    {
        std::uint16_t const* const swb = offsets(ics);
        bool const is_short = ics.window_sequence == EightShort;
        int const width = swb[sfb + 1] - swb[sfb];
        float const target = static_cast<float>(std::pow(2.0, 0.25 * ics.scalefactors[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)]));
        for (int w = 0; w < ics.group_length[static_cast<std::size_t>(g)]; ++w) {
            int const at = (is_short ? (window_base + w) * k_short : 0) + swb[sfb];
            double energy = 0;
            for (int i = 0; i < width; ++i) {
                float const v = noise();
                ics.spectrum[static_cast<std::size_t>(at + i)] = v;
                energy += static_cast<double>(v) * static_cast<double>(v);
            }
            float const scale = energy > 0 ? target / static_cast<float>(std::sqrt(energy / width)) : 0;
            for (int i = 0; i < width; ++i)
                ics.spectrum[static_cast<std::size_t>(at + i)] *= scale;
        }
    }

    // 4.6.9: temporal noise shaping, an all-pole filter over each window's
    // bands from the parcor coefficients sent.
    void apply_tns(Ics& ics)
    {
        if (!ics.tns.present)
            return;
        bool const is_short = ics.window_sequence == EightShort;
        std::uint16_t const* const swb = offsets(ics);
        int const max_bands = is_short ? bands->tns_max_short : bands->tns_max_long;
        int const swb_count = band_count(ics);
        for (int w = 0; w < ics.num_windows; ++w) {
            int top = swb_count;
            for (Tns::Filter const& filter : ics.tns.filters[static_cast<std::size_t>(w)]) {
                int bottom = std::max(0, top - filter.length);
                int const start_band = std::min({ bottom, max_bands, ics.max_sfb });
                int const end_band = std::min({ top, max_bands, ics.max_sfb });
                top = bottom;
                if (filter.order == 0 || end_band <= start_band)
                    continue;
                // Parcor to the filter's coefficients (the step-up recursion).
                std::array<double, 13> lpc {};
                std::array<double, 13> tmp {};
                lpc[0] = 1;
                for (int m = 1; m <= filter.order; ++m) {
                    double const k = static_cast<double>(filter.coefficients[static_cast<std::size_t>(m) - 1]);
                    for (int i = 1; i < m; ++i)
                        tmp[static_cast<std::size_t>(i)] = lpc[static_cast<std::size_t>(i)] + k * lpc[static_cast<std::size_t>(m - i)];
                    for (int i = 1; i < m; ++i)
                        lpc[static_cast<std::size_t>(i)] = tmp[static_cast<std::size_t>(i)];
                    lpc[static_cast<std::size_t>(m)] = k;
                }
                int const start = (is_short ? w * k_short : 0) + swb[start_band];
                int const end = (is_short ? w * k_short : 0) + swb[end_band];
                int const size = end - start;
                float* const x = ics.spectrum.data() + start;
                int const step = filter.backward ? -1 : 1;
                int at = filter.backward ? size - 1 : 0;
                std::array<double, 13> history {};
                for (int n = 0; n < size; ++n, at += step) {
                    double y = static_cast<double>(x[at]);
                    for (int i = 1; i <= filter.order; ++i)
                        y -= lpc[static_cast<std::size_t>(i)] * history[static_cast<std::size_t>(i)];
                    for (int i = filter.order; i > 1; --i)
                        history[static_cast<std::size_t>(i)] = history[static_cast<std::size_t>(i) - 1];
                    history[1] = y;
                    x[at] = static_cast<float>(y);
                }
            }
        }
    }

    // 4.6.11: the spectrum to time, windowed, and overlap-added onto the
    // half the frame before left behind.
    void synthesize(Ics const& ics, ChannelState& st, float* out)
    {
        Windows const& w = windows();
        std::array<float, 2 * k_frame> time {};
        bool const previous_kbd = st.have_previous ? st.previous_kbd : ics.window_shape_kbd;
        if (ics.window_sequence == EightShort) {
            std::vector<float> const& left = w.half(previous_kbd, true);
            std::vector<float> const& right = w.half(ics.window_shape_kbd, true);
            std::array<float, 2 * k_short> block {};
            for (int s = 0; s < 8; ++s) {
                imdct_short().run(ics.spectrum.data() + s * k_short, block.data());
                int const at = 448 + s * k_short;
                for (int i = 0; i < k_short; ++i) {
                    // The first short window's rise joins what came before;
                    // the others rise with this frame's shape.
                    float const rise = s == 0 ? left[static_cast<std::size_t>(i)] : right[static_cast<std::size_t>(i)];
                    time[static_cast<std::size_t>(at + i)] += block[static_cast<std::size_t>(i)] * rise;
                    time[static_cast<std::size_t>(at + k_short + i)] += block[static_cast<std::size_t>(k_short + i)] * right[static_cast<std::size_t>(k_short - 1 - i)];
                }
            }
        } else {
            imdct_long().run(ics.spectrum.data(), time.data());
            std::vector<float> const& left_long = w.half(previous_kbd, false);
            std::vector<float> const& right_long = w.half(ics.window_shape_kbd, false);
            std::vector<float> const& left_short = w.half(previous_kbd, true);
            std::vector<float> const& right_short = w.half(ics.window_shape_kbd, true);
            // The first half: a long rise, or (after short windows) silence,
            // a short rise and ones.
            if (ics.window_sequence == LongStop) {
                for (int i = 0; i < 448; ++i)
                    time[static_cast<std::size_t>(i)] = 0;
                for (int i = 0; i < k_short; ++i)
                    time[static_cast<std::size_t>(448 + i)] *= left_short[static_cast<std::size_t>(i)];
            } else {
                for (int i = 0; i < k_frame; ++i)
                    time[static_cast<std::size_t>(i)] *= left_long[static_cast<std::size_t>(i)];
            }
            // The second half: a long fall, or (before short windows) ones,
            // a short fall and silence.
            if (ics.window_sequence == LongStart) {
                for (int i = 0; i < k_short; ++i)
                    time[static_cast<std::size_t>(k_frame + 448 + i)] *= right_short[static_cast<std::size_t>(k_short - 1 - i)];
                for (int i = k_frame + 448 + k_short; i < 2 * k_frame; ++i)
                    time[static_cast<std::size_t>(i)] = 0;
            } else {
                for (int i = 0; i < k_frame; ++i)
                    time[static_cast<std::size_t>(k_frame + i)] *= right_long[static_cast<std::size_t>(k_frame - 1 - i)];
            }
        }
        // The standard's samples are in 16-bit full-scale units: [-1, 1] here.
        constexpr float full_scale = 1.0f / 32768.0f;
        for (int i = 0; i < k_frame; ++i) {
            out[i] = (time[static_cast<std::size_t>(i)] + st.overlap[static_cast<std::size_t>(i)]) * full_scale;
            st.overlap[static_cast<std::size_t>(i)] = time[static_cast<std::size_t>(k_frame + i)];
        }
        st.previous_kbd = ics.window_shape_kbd;
        st.have_previous = true;
    }

    // 4.4.2.6: an individual channel stream.
    bool read_ics(Bits& bits, Ics& ics, bool common_window, std::array<int, k_frame>& quantized)
    {
        ics.global_gain = static_cast<int>(bits.u(8));
        if (!common_window && !read_ics_info(bits, ics))
            return false;
        if (!read_sections(bits, ics) || !read_scalefactors(bits, ics))
            return false;
        ics.pulse_count = 0;
        if (bits.flag() && !read_pulses(bits, ics))
            return false;
        ics.tns.present = false;
        if (bits.flag() && !read_tns(bits, ics))
            return false;
        if (bits.flag())
            return false; // gain_control_data: the SSR profile's, not read
        return read_spectrum(bits, ics, quantized);
    }

    void finish_channel(Ics& ics, std::array<int, k_frame> const& quantized)
    {
        dequantize(ics, quantized);
        int window_base = 0;
        for (int g = 0; g < ics.num_window_groups; ++g) {
            for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
                if (ics.books[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)] == BookNoise)
                    fill_noise(ics, g, sfb, window_base);
            }
            window_base += ics.group_length[static_cast<std::size_t>(g)];
        }
    }

    // 4.6.8: a channel pair's mid/side bands and intensity stereo.
    void apply_stereo(Ics& left, Ics& right, bool ms_present_all, std::array<std::array<bool, k_max_sfb>, 8> const& ms_used)
    {
        std::uint16_t const* const swb = offsets(left);
        bool const is_short = left.window_sequence == EightShort;
        int window_base = 0;
        for (int g = 0; g < left.num_window_groups; ++g) {
            for (int sfb = 0; sfb < left.max_sfb; ++sfb) {
                int const right_book = right.books[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)];
                int const left_book = left.books[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)];
                bool const ms = ms_present_all || ms_used[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)];
                int const width = swb[sfb + 1] - swb[sfb];
                for (int w = 0; w < left.group_length[static_cast<std::size_t>(g)]; ++w) {
                    int const at = (is_short ? (window_base + w) * k_short : 0) + swb[sfb];
                    if (right_book == BookIntensity || right_book == BookIntensity2) {
                        // 4.6.8.2: the right channel from the left, scaled.
                        float scale = static_cast<float>(std::pow(0.5, 0.25 * right.scalefactors[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)]));
                        if (right_book == BookIntensity2)
                            scale = -scale;
                        if (ms)
                            scale = -scale;
                        for (int i = 0; i < width; ++i)
                            right.spectrum[static_cast<std::size_t>(at + i)] = left.spectrum[static_cast<std::size_t>(at + i)] * scale;
                    } else if (ms && left_book != BookNoise && right_book != BookNoise && left_book != BookIntensity && left_book != BookIntensity2) {
                        // 4.6.8.1: mid and side back to left and right.
                        for (int i = 0; i < width; ++i) {
                            float const m = left.spectrum[static_cast<std::size_t>(at + i)];
                            float const s = right.spectrum[static_cast<std::size_t>(at + i)];
                            left.spectrum[static_cast<std::size_t>(at + i)] = m + s;
                            right.spectrum[static_cast<std::size_t>(at + i)] = m - s;
                        }
                    }
                }
            }
            window_base += left.group_length[static_cast<std::size_t>(g)];
        }
    }

    bool decode(std::span<std::uint8_t const> block, std::vector<float>& out)
    {
        if (!bands)
            return false;
        int const channels = config.channels;
        Bits bits(block);
        int channel = 0; // the next output channel to fill
        std::array<float, k_frame> samples {};
        // The core's samples by channel, and the elements they came from:
        // SBR's data follows its element and is applied once the block is read.
        std::vector<std::array<float, k_frame>> core(static_cast<std::size_t>(channels));
        struct Element {
            int index;
            bool pair;
            int first_channel;
        };
        std::vector<Element> elements;
        int element_index = 0;
        auto const emit = [&](float const* data) {
            if (channel >= channels)
                return;
            std::copy(data, data + k_frame, core[static_cast<std::size_t>(channel)].begin());
            ++channel;
        };
        auto const finish = [&]() {
            if (!sbr_output) {
                out.assign(static_cast<std::size_t>(k_frame) * static_cast<std::size_t>(channels), 0.0f);
                for (int c = 0; c < channel; ++c)
                    for (int i = 0; i < k_frame; ++i)
                        out[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = core[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)];
                return;
            }
            int const frames = sbr_frames();
            int const out_channels = output_channels();
            out.assign(static_cast<std::size_t>(frames) * static_cast<std::size_t>(out_channels), 0.0f);
            std::vector<float> left(static_cast<std::size_t>(frames)), right(static_cast<std::size_t>(frames));
            for (Element const& e : elements) {
                if (e.first_channel >= channel)
                    continue;
                std::unique_ptr<SbrDecoder>& decoder = sbr[e.index];
                if (!decoder)
                    decoder = std::make_unique<SbrDecoder>(config.sample_rate, e.pair, downsampled());
                bool const two = e.pair && e.first_channel + 1 < channel;
                // A single channel's parametric stereo: its right channel is the output's second.
                bool const stereo = !e.pair && ps_output && e.first_channel == 0;
                decoder->process(core[static_cast<std::size_t>(e.first_channel)].data(), two ? core[static_cast<std::size_t>(e.first_channel + 1)].data() : nullptr,
                    left.data(), right.data());
                for (int i = 0; i < frames; ++i)
                    out[static_cast<std::size_t>(i) * static_cast<std::size_t>(out_channels) + static_cast<std::size_t>(e.first_channel)] = left[static_cast<std::size_t>(i)];
                if (two || stereo) {
                    for (int i = 0; i < frames; ++i)
                        out[static_cast<std::size_t>(i) * static_cast<std::size_t>(out_channels) + static_cast<std::size_t>(e.first_channel + 1)] = right[static_cast<std::size_t>(i)];
                }
            }
        };
        auto ics_a = std::make_unique<Ics>();
        auto ics_b = std::make_unique<Ics>();
        auto quantized_a = std::make_unique<std::array<int, k_frame>>();
        auto quantized_b = std::make_unique<std::array<int, k_frame>>();
        for (int guard = 0; guard < 64; ++guard) {
            if (bits.left() < 3)
                break;
            int const id = static_cast<int>(bits.u(3));
            if (id == 7) // END
                break;
            if (id == 0 || id == 3) { // SCE, LFE
                bits.u(4); // element_instance_tag
                elements.push_back({ element_index++, false, channel });
                if (!read_ics(bits, *ics_a, false, *quantized_a))
                    return false;
                finish_channel(*ics_a, *quantized_a);
                apply_tns(*ics_a);
                synthesize(*ics_a, state[static_cast<std::size_t>(std::min(channel, k_max_channels - 1))], samples.data());
                emit(samples.data());
            } else if (id == 1) { // CPE
                bits.u(4);
                elements.push_back({ element_index++, true, channel });
                bool const common_window = bits.flag();
                bool ms_all = false;
                std::array<std::array<bool, k_max_sfb>, 8> ms_used {};
                if (common_window) {
                    if (!read_ics_info(bits, *ics_a))
                        return false;
                    int const ms_mask_present = static_cast<int>(bits.u(2));
                    if (ms_mask_present == 1) {
                        for (int g = 0; g < ics_a->num_window_groups; ++g) {
                            for (int sfb = 0; sfb < ics_a->max_sfb; ++sfb)
                                ms_used[static_cast<std::size_t>(g)][static_cast<std::size_t>(sfb)] = bits.flag();
                        }
                    } else if (ms_mask_present == 2) {
                        ms_all = true;
                    } else if (ms_mask_present == 3) {
                        return false;
                    }
                    // The right channel shares the window and grouping.
                    ics_b->window_sequence = ics_a->window_sequence;
                    ics_b->window_shape_kbd = ics_a->window_shape_kbd;
                    ics_b->max_sfb = ics_a->max_sfb;
                    ics_b->num_windows = ics_a->num_windows;
                    ics_b->num_window_groups = ics_a->num_window_groups;
                    ics_b->group_length = ics_a->group_length;
                }
                if (!read_ics(bits, *ics_a, common_window, *quantized_a) || !read_ics(bits, *ics_b, common_window, *quantized_b))
                    return false;
                finish_channel(*ics_a, *quantized_a);
                finish_channel(*ics_b, *quantized_b);
                if (common_window)
                    apply_stereo(*ics_a, *ics_b, ms_all, ms_used);
                apply_tns(*ics_a);
                apply_tns(*ics_b);
                synthesize(*ics_a, state[static_cast<std::size_t>(std::min(channel, k_max_channels - 1))], samples.data());
                emit(samples.data());
                synthesize(*ics_b, state[static_cast<std::size_t>(std::min(channel, k_max_channels - 1))], samples.data());
                emit(samples.data());
            } else if (id == 2) { // CCE: coupling, not read
                return false;
            } else if (id == 4) { // DSE
                bits.u(4);
                bool const align = bits.flag();
                int count = static_cast<int>(bits.u(8));
                if (count == 255)
                    count += static_cast<int>(bits.u(8));
                if (align)
                    bits.align();
                bits.skip(static_cast<std::size_t>(count) * 8);
            } else if (id == 5) { // PCE: the layout the configuration already gave
                if (!read_program_config(bits))
                    return false;
            } else if (id == 6) { // FIL: an extension — SBR's data for the element before it
                int count = static_cast<int>(bits.u(4));
                if (count == 15)
                    count += static_cast<int>(bits.u(8)) - 1;
                if (count > 0) {
                    int const type = static_cast<int>(bits.u(4));
                    std::size_t const payload_bits = static_cast<std::size_t>(count) * 8 - 4;
                    if ((type == 0xD || type == 0xE) && !elements.empty()) {
                        Element const& e = elements.back();
                        std::unique_ptr<SbrDecoder>& decoder = sbr[e.index];
                        if (!decoder)
                            decoder = std::make_unique<SbrDecoder>(config.sample_rate, e.pair, downsampled());
                        sbr_output = true;
                        SbrBits sbr_bits(block, bits.position());
                        decoder->read(sbr_bits, payload_bits, type == 0xE);
                        if (!e.pair && config.channels == 1 && decoder->parametric_stereo())
                            ps_output = true;
                    }
                    bits.skip(payload_bits);
                }
            }
            if (bits.failed())
                return false;
        }
        if (channel == 0)
            return false;
        finish();
        return true;
    }
};

AacDecoder::AacDecoder(AacConfig const& config)
    : m_impl(std::make_unique<Impl>(config))
{
}

AacDecoder::~AacDecoder() = default;

int AacDecoder::sample_rate() const { return m_impl->config.sample_rate; }
int AacDecoder::channels() const { return m_impl->output_channels(); }
int AacDecoder::output_rate() const { return m_impl->sbr_output && !m_impl->downsampled() ? 2 * m_impl->config.sample_rate : m_impl->config.sample_rate; }
int AacDecoder::frames_per_block() const { return m_impl->sbr_output ? m_impl->sbr_frames() : k_frame; }

bool AacDecoder::decode(std::span<std::uint8_t const> block, std::vector<float>& out)
{
    return m_impl->decode(block, out);
}

void AacDecoder::reset()
{
    for (ChannelState& st : m_impl->state)
        st = {};
    for (auto& [index, decoder] : m_impl->sbr)
        decoder->reset();
}

}
