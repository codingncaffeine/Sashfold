#include "media/Sbr.h"

#include "media/Ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>

// Spectral band replication, written from ISO/IEC 14496-3 §4.6.18: the
// bitstream (4.6.18.3), the QMF banks (4.6.18.4), the high frequency
// generator (4.6.18.6) and adjuster (4.6.18.7), and the assembly (4.6.18.8).
// The tables are the standard's (media/SbrData.h). Times are in QMF
// columns: a frame is 32 of them (16 time slots of two), kept in buffers
// of 40 so that the eight before the frame (tHFGen) are at hand, and an
// envelope's border t (in slots, 0 to 19) is at column 2t + tHFAdj (2).

namespace sashfold::media {

std::uint32_t SbrBits::u(int n)
{
    std::uint32_t value = 0;
    for (int i = 0; i < n; ++i) {
        std::size_t const byte = m_position >> 3;
        if (byte >= m_data.size()) {
            m_failed = true;
            return value << (n - i);
        }
        value = (value << 1) | ((m_data[byte] >> (7 - (m_position & 7))) & 1u);
        ++m_position;
    }
    return value;
}

namespace {

using Complex = std::complex<float>;

constexpr int k_slots = 16; // time slots a frame has
constexpr int k_columns = 32; // QMF columns a frame has
constexpr int k_buffer = 40; // columns kept: tHFGen before the frame, then the frame
constexpr int k_hf_gen = 8;
constexpr int k_hf_adj = 2;
constexpr int k_max_env = 5;
constexpr int k_low_bands = 32;
constexpr int k_bands = 64;
constexpr float k_pi = std::numbers::pi_v<float>;
constexpr float k_pcm_scale = 32768.0f;

struct Header {
    bool amp_res = true;
    int start_freq = 5;
    int stop_freq = 0;
    int xover_band = 0;
    int freq_scale = 2;
    bool alter_scale = true;
    int noise_bands = 2;
    int limiter_bands = 2;
    int limiter_gains = 2;
    bool interpol_freq = true;
    bool smoothing_mode = true;
    friend bool operator==(Header const&, Header const&) = default;
};

// 4.6.18.3.2: the frequency band tables a header gives.
struct Tables {
    int k0 = 0, k2 = 0, kx = 0, m = 0;
    int n_master = 0, n_high = 0, n_low = 0, n_q = 0, n_l = 0;
    std::array<int, 65> f_master {};
    std::array<int, 65> f_high {};
    std::array<int, 65> f_low {};
    std::array<int, 6> f_noise {};
    std::array<int, 70> f_lim {}; // relative to kx
    int num_patches = 0;
    std::array<int, 6> patch_subbands {};
    std::array<int, 6> patch_start {};
    bool valid = false;
};

int ceil_log2(int n)
{
    int bits = 0;
    while ((1 << bits) < n)
        ++bits;
    return bits;
}

// The bands between two frequencies, spaced geometrically, as differences.
std::vector<int> make_bands(int start, int stop, int count)
{
    std::vector<int> out;
    int previous = start;
    for (int k = 0; k < count; ++k) {
        int const present = static_cast<int>(std::lround(start * std::pow(static_cast<double>(stop) / start, static_cast<double>(k + 1) / count)));
        out.push_back(present - previous);
        previous = present;
    }
    return out;
}

// 4.6.18.3.2.1: k0 for a start_freq, by the SBR rate (the output's, twice
// the core's) — the tables of Table 4.82 up to 48 kHz, and above it the
// standard's startMin with the offsets of its last two rows.
std::optional<int> start_band(int sbr_rate, int start_freq)
{
    switch (sbr_rate) {
    case 16000: return sbr_data::start_freq_16[start_freq];
    case 22050: return sbr_data::start_freq_22[start_freq];
    case 24000: return sbr_data::start_freq_24[start_freq];
    case 32000: return sbr_data::start_freq_32[start_freq];
    case 44100: return sbr_data::start_freq_44[start_freq];
    case 48000: return sbr_data::start_freq_48[start_freq];
    default: break;
    }
    if (sbr_rate < 64000 || sbr_rate > 96000)
        return std::nullopt;
    static constexpr int offsets_64[16] = { -4, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 16, 20 };
    static constexpr int offsets_above[16] = { -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 16, 20, 24 };
    int const start_min = static_cast<int>(std::lround(5000.0 * 128 / sbr_rate));
    return start_min + (sbr_rate == 64000 ? offsets_64 : offsets_above)[start_freq];
}

Tables make_tables(Header const& h, int core_rate)
{
    Tables t;
    int const fs = core_rate * 2;
    std::optional<int> const k0 = start_band(fs, h.start_freq);
    if (!k0)
        return t;
    t.k0 = *k0;
    // 4.6.18.3.2.1: k2.
    if (h.stop_freq == 15) {
        t.k2 = std::min(64, 3 * t.k0);
    } else if (h.stop_freq == 14) {
        t.k2 = std::min(64, 2 * t.k0);
    } else {
        int const stop_min = static_cast<int>(std::lround((fs < 32000 ? 6000.0 : fs < 64000 ? 8000.0 : 10000.0) * 128 / fs));
        std::vector<int> dk = make_bands(stop_min, 64, 13);
        std::sort(dk.begin(), dk.end());
        t.k2 = stop_min;
        for (int i = 0; i < h.stop_freq; ++i)
            t.k2 += dk[static_cast<std::size_t>(i)];
    }
    t.k2 = std::min(t.k2, 64);
    if (t.k0 >= t.k2 || t.k2 - t.k0 > 48)
        return t;
    // The master table.
    std::vector<int> master;
    if (h.freq_scale == 0) {
        int const dk = h.alter_scale ? 2 : 1;
        int bands = (t.k2 - t.k0) / dk / 2 * 2; // even
        if (bands <= 0)
            return t;
        int diff = t.k2 - (t.k0 + bands * dk);
        std::vector<int> vdk(static_cast<std::size_t>(bands), dk);
        if (diff != 0) {
            int const incr = diff > 0 ? -1 : 1;
            int k = diff > 0 ? bands - 1 : 0;
            while (diff != 0) {
                vdk[static_cast<std::size_t>(k)] -= incr;
                k += incr;
                diff += incr;
            }
        }
        master.push_back(t.k0);
        for (int const d : vdk)
            master.push_back(master.back() + d);
    } else {
        int const bands_per_octave = h.freq_scale == 1 ? 12 : h.freq_scale == 2 ? 10 : 8;
        double const warp = h.alter_scale ? 1.3 : 1.0;
        bool const two_regions = static_cast<double>(t.k2) / t.k0 > 2.2449;
        int const k1 = two_regions ? 2 * t.k0 : t.k2;
        int const bands0 = 2 * static_cast<int>(std::lround(bands_per_octave * std::log2(static_cast<double>(k1) / t.k0) / 2));
        if (bands0 <= 0)
            return t;
        std::vector<int> vdk0 = make_bands(t.k0, k1, bands0);
        std::sort(vdk0.begin(), vdk0.end());
        master.push_back(t.k0);
        for (int const d : vdk0)
            master.push_back(master.back() + d);
        if (two_regions) {
            int const bands1 = 2 * static_cast<int>(std::lround(bands_per_octave * std::log2(static_cast<double>(t.k2) / k1) / (2 * warp)));
            if (bands1 > 0) {
                std::vector<int> vdk1 = make_bands(k1, t.k2, bands1);
                std::sort(vdk1.begin(), vdk1.end());
                if (vdk1.front() < vdk0.back()) {
                    int const change = vdk0.back() - vdk1.front();
                    vdk1.front() += change;
                    vdk1.back() -= change;
                }
                std::sort(vdk1.begin(), vdk1.end());
                for (int const d : vdk1)
                    master.push_back(master.back() + d);
            }
        }
    }
    if (master.size() < 2 || master.size() > 65)
        return t;
    for (std::size_t i = 1; i < master.size(); ++i) {
        if (master[i] <= master[i - 1])
            return t;
    }
    t.n_master = static_cast<int>(master.size()) - 1;
    std::copy(master.begin(), master.end(), t.f_master.begin());
    // 4.6.18.3.2.2: the derived tables.
    if (h.xover_band >= t.n_master)
        return t;
    t.n_high = t.n_master - h.xover_band;
    for (int i = 0; i <= t.n_high; ++i)
        t.f_high[static_cast<std::size_t>(i)] = t.f_master[static_cast<std::size_t>(i + h.xover_band)];
    t.kx = t.f_high[0];
    t.m = t.f_high[static_cast<std::size_t>(t.n_high)] - t.kx;
    if (t.kx + t.m > 64 || t.m <= 0)
        return t;
    t.n_low = t.n_high - t.n_high / 2;
    t.f_low[0] = t.f_high[0];
    for (int k = 1; k <= t.n_low; ++k)
        t.f_low[static_cast<std::size_t>(k)] = t.f_high[static_cast<std::size_t>(t.n_high & 1 ? 2 * k - 1 : 2 * k)];
    t.n_q = std::clamp(static_cast<int>(std::lround(h.noise_bands * std::log2(static_cast<double>(t.k2) / t.kx))), 1, 5);
    t.f_noise[0] = t.f_low[0];
    {
        int i = 0;
        for (int k = 1; k <= t.n_q; ++k) {
            i += (t.n_low - i) / (t.n_q + 1 - k);
            t.f_noise[static_cast<std::size_t>(k)] = t.f_low[static_cast<std::size_t>(i)];
        }
    }
    // 4.6.18.6.3: the patches.
    {
        int msb = t.k0;
        int usb = t.kx;
        int const goal = static_cast<int>(std::lround(2.048e6 / fs));
        int k = 0;
        if (goal < t.kx + t.m) {
            while (k < t.n_master && t.f_master[static_cast<std::size_t>(k)] < goal)
                ++k;
        } else {
            k = t.n_master;
        }
        int sb = 0;
        int guard = 0;
        do {
            int odd = 0;
            int i = k;
            do {
                sb = t.f_master[static_cast<std::size_t>(i)];
                odd = (sb + t.k0) & 1;
                --i;
            } while (i >= 0 && sb > t.k0 - 1 + msb - odd);
            if (t.num_patches >= 6)
                return t;
            t.patch_subbands[static_cast<std::size_t>(t.num_patches)] = std::max(sb - usb, 0);
            t.patch_start[static_cast<std::size_t>(t.num_patches)] = t.k0 - odd - t.patch_subbands[static_cast<std::size_t>(t.num_patches)];
            if (t.patch_subbands[static_cast<std::size_t>(t.num_patches)] > 0) {
                usb = sb;
                msb = sb;
                ++t.num_patches;
            } else {
                msb = t.kx;
            }
            if (t.f_master[static_cast<std::size_t>(k)] - sb < 3)
                k = t.n_master;
        } while (sb != t.kx + t.m && ++guard < 64);
        if (guard >= 64)
            return t;
        if (t.num_patches > 1 && t.patch_subbands[static_cast<std::size_t>(t.num_patches - 1)] < 3)
            --t.num_patches;
        if (t.num_patches == 0)
            return t;
    }
    // 4.6.18.3.2.3: the limiter bands.
    if (h.limiter_bands == 0) {
        t.f_lim[0] = 0;
        t.f_lim[1] = t.m;
        t.n_l = 1;
    } else {
        double const lim_bands = static_cast<double>(sbr_data::limiter_bands_per_octave[h.limiter_bands]);
        std::vector<int> borders;
        borders.push_back(t.kx);
        for (int i = 0; i < t.num_patches; ++i)
            borders.push_back(borders.back() + t.patch_subbands[static_cast<std::size_t>(i)]);
        std::vector<int> table(t.f_low.begin(), t.f_low.begin() + t.n_low + 1);
        for (std::size_t i = 1; i + 1 < borders.size(); ++i)
            table.push_back(borders[i]);
        std::sort(table.begin(), table.end());
        auto const is_border = [&](int v) { return std::find(borders.begin(), borders.end(), v) != borders.end(); };
        std::size_t k = 0;
        while (k + 1 < table.size()) {
            double const octaves = std::log2(static_cast<double>(table[k + 1]) / table[k]);
            if (octaves * lim_bands < 0.49) {
                if (table[k + 1] == table[k] || !is_border(table[k + 1])) {
                    table.erase(table.begin() + static_cast<std::ptrdiff_t>(k + 1));
                } else if (!is_border(table[k])) {
                    table.erase(table.begin() + static_cast<std::ptrdiff_t>(k));
                } else {
                    ++k; // both patch borders: both stay
                    continue;
                }
                if (k > 0)
                    --k;
                continue;
            }
            ++k;
        }
        t.n_l = static_cast<int>(table.size()) - 1;
        for (std::size_t i = 0; i < table.size() && i < t.f_lim.size(); ++i)
            t.f_lim[i] = table[i] - t.kx;
    }
    t.valid = t.n_l >= 1;
    return t;
}

// One frame's data of one channel, as read.
struct ChannelFrame {
    int frame_class = 0;
    int num_env = 0;
    int num_noise = 0;
    bool amp_res = true;
    std::array<int, 6> t_e {};
    std::array<int, 3> t_q {};
    int l_a = -1;
    std::array<bool, k_max_env> freq_res {};
    std::array<bool, k_max_env> df_env {};
    std::array<bool, 2> df_noise {};
    std::array<int, 5> invf {};
    std::array<std::array<int, 64>, k_max_env> env_q {}; // quantized, by band of the envelope's resolution
    std::array<std::array<int, 5>, 2> noise_q {};
    bool add_harmonic_flag = false;
    std::array<bool, 64> add_harmonic {};
    // Dequantized.
    std::array<std::array<float, 64>, k_max_env> env {};
    std::array<std::array<float, 5>, 2> noise {};
    bool present = false;
};

struct ChannelState {
    // The QMF banks.
    std::array<float, 320> x {}; // analysis delay line, newest first
    std::array<float, 1280> v {}; // synthesis delay line (the downsampled bank uses its first 640)
    std::array<std::array<Complex, k_buffer>, k_low_bands> low {};
    std::array<std::array<Complex, k_buffer>, k_bands> high {}; // generated, then adjusted
    std::array<int, k_buffer> column_kx {}; // the crossover each column was made with
    int low_kx_prev = 32; // the crossover the eight columns before the frame were cut at
    // What carries over between frames.
    std::array<int, 64> env_prev {};
    bool res_prev = true;
    std::array<int, 5> noise_prev {};
    std::array<float, 5> bw_prev {};
    std::array<int, 5> invf_prev {};
    bool delta_first = false; // the previous frame's transient was at its end
    std::array<std::array<float, 4>, 64> g_hist {}; // the gains of the four columns before, newest first
    std::array<std::array<float, 4>, 64> q_hist {};
    bool hist_primed = false;
    std::array<bool, 64> s_index_prev {};
    int noise_index = 0;
    int sine_index = 0;
    ChannelFrame frame;
};

// The modulation tables.
struct Modulation {
    std::array<std::array<Complex, 64>, k_low_bands> analysis {}; // [k][n]
    std::array<std::array<Complex, 64>, 128> synthesis {}; // [n][k]
    std::array<std::array<Complex, 32>, 64> synthesis_down {}; // [n][k]: the 32-band bank (4.6.18.4.3)
    Modulation()
    {
        for (int k = 0; k < k_low_bands; ++k)
            for (int n = 0; n < 64; ++n) {
                float const a = k_pi / 64 * (static_cast<float>(k) + 0.5f) * (static_cast<float>(2 * n) - 0.5f);
                analysis[static_cast<std::size_t>(k)][static_cast<std::size_t>(n)] = Complex(std::cos(a), std::sin(a));
            }
        // The standard's phases (4.6.18.4): any other pair of offsets that
        // passes the low band through unchanged turns a band patched from p
        // to k by an angle that grows with k - p.
        for (int n = 0; n < 128; ++n)
            for (int k = 0; k < 64; ++k) {
                float const a = k_pi / 128 * (static_cast<float>(k) + 0.5f) * static_cast<float>(2 * n - 255);
                synthesis[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] = Complex(std::cos(a), std::sin(a));
            }
        for (int n = 0; n < 64; ++n)
            for (int k = 0; k < 32; ++k) {
                // The 64-band phase pi (k + 1/2)(2n - 255) / 128 taken at every
                // other sample (n = 2n'): pi (k + 1/2)(2n' - 127.5) / 64.
                float const a = k_pi / 64 * (static_cast<float>(k) + 0.5f) * (static_cast<float>(2 * n) - 127.5f);
                synthesis_down[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] = Complex(std::cos(a), std::sin(a));
            }
    }
};

Modulation const& modulation()
{
    static Modulation const tables;
    return tables;
}

// 4.6.18.4.1: 32 bands of 32 new samples, into the buffer's column.
void analyze(ChannelState& s, float const* in, int column)
{
    std::copy_backward(s.x.begin(), s.x.begin() + 288, s.x.end());
    for (int n = 0; n < 32; ++n)
        s.x[static_cast<std::size_t>(n)] = in[31 - n];
    std::array<float, 64> u {};
    for (int n = 0; n < 64; ++n) {
        float sum = 0;
        for (int j = 0; j < 5; ++j)
            sum += s.x[static_cast<std::size_t>(n + 64 * j)] * sbr_data::qmf_prototype[2 * (n + 64 * j)];
        u[static_cast<std::size_t>(n)] = sum;
    }
    Modulation const& mod = modulation();
    for (int k = 0; k < k_low_bands; ++k) {
        Complex sum(0, 0);
        auto const& row = mod.analysis[static_cast<std::size_t>(k)];
        for (int n = 0; n < 64; ++n)
            sum += u[static_cast<std::size_t>(n)] * row[static_cast<std::size_t>(n)];
        // Twice the sum: the 32-band bank uses every other tap of the
        // prototype, and its bands must carry the scale the encoder's
        // 64-band analysis gave the envelopes.
        s.low[static_cast<std::size_t>(k)][static_cast<std::size_t>(column)] = 2.0f * sum;
    }
}

// 4.6.18.4.2: 64 bands of one column to 64 samples.
void synthesize(ChannelState& s, std::array<Complex, 64> const& x, float* out)
{
    std::copy_backward(s.v.begin(), s.v.begin() + 1152, s.v.end());
    Modulation const& mod = modulation();
    for (int n = 0; n < 128; ++n) {
        float sum = 0;
        auto const& row = mod.synthesis[static_cast<std::size_t>(n)];
        for (int k = 0; k < 64; ++k) {
            Complex const& c = x[static_cast<std::size_t>(k)];
            Complex const& m = row[static_cast<std::size_t>(k)];
            sum += c.real() * m.real() - c.imag() * m.imag();
        }
        s.v[static_cast<std::size_t>(n)] = sum / 64;
    }
    std::array<float, 640> w {};
    for (int n = 0; n < 5; ++n) {
        for (int m = 0; m < 64; ++m) {
            w[static_cast<std::size_t>(128 * n + m)] = s.v[static_cast<std::size_t>(256 * n + m)];
            w[static_cast<std::size_t>(128 * n + 64 + m)] = s.v[static_cast<std::size_t>(256 * n + 192 + m)];
        }
    }
    for (int n = 0; n < 640; ++n)
        w[static_cast<std::size_t>(n)] *= sbr_data::qmf_prototype[n];
    for (int n = 0; n < 64; ++n) {
        float sum = 0;
        for (int j = 0; j < 10; ++j)
            sum += w[static_cast<std::size_t>(64 * j + n)];
        out[n] = sum;
    }
}

// 4.6.18.4.3: the downsampled bank — the lower 32 bands of a column to 32
// samples at the core's rate, for a stream whose SBR output rate is its
// core's.
void synthesize_down(ChannelState& s, std::array<Complex, 64> const& x, float* out)
{
    std::copy_backward(s.v.begin(), s.v.begin() + 576, s.v.begin() + 640);
    Modulation const& mod = modulation();
    for (int n = 0; n < 64; ++n) {
        float sum = 0;
        auto const& row = mod.synthesis_down[static_cast<std::size_t>(n)];
        for (int k = 0; k < 32; ++k) {
            Complex const& c = x[static_cast<std::size_t>(k)];
            Complex const& m = row[static_cast<std::size_t>(k)];
            sum += c.real() * m.real() - c.imag() * m.imag();
        }
        s.v[static_cast<std::size_t>(n)] = sum / 64;
    }
    std::array<float, 320> w {};
    for (int n = 0; n < 5; ++n) {
        for (int m = 0; m < 32; ++m) {
            w[static_cast<std::size_t>(64 * n + m)] = s.v[static_cast<std::size_t>(128 * n + m)];
            w[static_cast<std::size_t>(64 * n + 32 + m)] = s.v[static_cast<std::size_t>(128 * n + 96 + m)];
        }
    }
    for (int n = 0; n < 320; ++n)
        w[static_cast<std::size_t>(n)] *= sbr_data::qmf_prototype[2 * n];
    for (int n = 0; n < 32; ++n) {
        float sum = 0;
        for (int j = 0; j < 10; ++j)
            sum += w[static_cast<std::size_t>(32 * j + n)];
        out[n] = sum;
    }
}

} // namespace

std::optional<int> sbr_read_code(SbrBits& bits, sbr_data::HuffmanBook const& book)
{
    std::uint32_t code = 0;
    int length = 0;
    std::size_t at = 0;
    for (int step = 0; step < 20; ++step) {
        code = (code << 1) | bits.u(1);
        ++length;
        while (at < book.count && book.entries[at].length < length)
            ++at;
        for (std::size_t i = at; i < book.count && book.entries[i].length == length; ++i) {
            if (book.entries[i].code == code)
                return book.entries[i].delta;
        }
        if (bits.failed())
            return std::nullopt;
    }
    return std::nullopt;
}

struct SbrDecoder::Impl {
    int core_rate;
    bool pair;
    bool downsampled;
    Header header;
    bool have_header = false;
    Tables tables;
    bool coupling = false;
    std::array<ChannelState, 2> ch {};
    // A single channel's parametric stereo; its right channel is
    // synthesized through the second channel's bank.
    std::unique_ptr<PsDecoder> ps;

    Impl(int rate, bool channel_pair, bool down)
        : core_rate(rate)
        , pair(channel_pair)
        , downsampled(down)
    {
        if (!pair)
            ps = std::make_unique<PsDecoder>();
    }

    void reset_states()
    {
        for (ChannelState& s : ch) {
            ChannelFrame const frame = s.frame;
            s = ChannelState {};
            (void)frame;
        }
    }

    // --- The bitstream (4.6.18.3) ---

    bool read_header(SbrBits& bits)
    {
        Header h;
        h.amp_res = bits.flag();
        h.start_freq = static_cast<int>(bits.u(4));
        h.stop_freq = static_cast<int>(bits.u(4));
        h.xover_band = static_cast<int>(bits.u(3));
        bits.u(2);
        bool const extra1 = bits.flag();
        bool const extra2 = bits.flag();
        if (extra1) {
            h.freq_scale = static_cast<int>(bits.u(2));
            h.alter_scale = bits.flag();
            h.noise_bands = static_cast<int>(bits.u(2));
        }
        if (extra2) {
            h.limiter_bands = static_cast<int>(bits.u(2));
            h.limiter_gains = static_cast<int>(bits.u(2));
            h.interpol_freq = bits.flag();
            h.smoothing_mode = bits.flag();
        }
        if (bits.failed())
            return false;
        if (!have_header || !(h == header)) {
            Tables t = make_tables(h, core_rate);
            if (!t.valid)
                return false;
            // A new frequency layout: what carried over no longer fits.
            // A reset (4.6.18.3.3): any of the fields the tables come from.
            bool const reset = !have_header || h.start_freq != header.start_freq || h.stop_freq != header.stop_freq || h.freq_scale != header.freq_scale
                || h.alter_scale != header.alter_scale || h.xover_band != header.xover_band || h.noise_bands != header.noise_bands;
            if (reset) {
                for (ChannelState& s : ch) {
                    s.hist_primed = false;
                }
            }
            tables = t;
            header = h;
            have_header = true;
        }
        return true;
    }

    bool read_grid(SbrBits& bits, ChannelFrame& f)
    {
        f.frame_class = static_cast<int>(bits.u(2));
        f.amp_res = header.amp_res;
        std::array<int, 4> rel0 {}, rel1 {};
        int num_rel0 = 0, num_rel1 = 0, var0 = 0, var1 = 0, pointer = 0;
        switch (f.frame_class) {
        case 0: { // FIXFIX
            f.num_env = 1 << bits.u(2);
            if (f.num_env > 4)
                return false;
            if (f.num_env == 1)
                f.amp_res = false;
            bool const res = bits.flag();
            for (int l = 0; l < f.num_env; ++l)
                f.freq_res[static_cast<std::size_t>(l)] = res;
            for (int l = 0; l <= f.num_env; ++l)
                f.t_e[static_cast<std::size_t>(l)] = l * k_slots / f.num_env;
            f.l_a = -1;
            break;
        }
        case 1: { // FIXVAR
            var1 = static_cast<int>(bits.u(2));
            num_rel1 = static_cast<int>(bits.u(2));
            f.num_env = num_rel1 + 1;
            for (int i = 0; i < num_rel1; ++i)
                rel1[static_cast<std::size_t>(i)] = 2 * static_cast<int>(bits.u(2)) + 2;
            pointer = static_cast<int>(bits.u(ceil_log2(f.num_env + 1)));
            for (int l = 0; l < f.num_env; ++l)
                f.freq_res[static_cast<std::size_t>(f.num_env - 1 - l)] = bits.flag();
            f.t_e[0] = 0;
            f.t_e[static_cast<std::size_t>(f.num_env)] = k_slots + var1;
            for (int i = 0; i < num_rel1; ++i)
                f.t_e[static_cast<std::size_t>(f.num_env - 1 - i)] = f.t_e[static_cast<std::size_t>(f.num_env - i)] - rel1[static_cast<std::size_t>(i)];
            f.l_a = pointer == 0 ? -1 : f.num_env + 1 - pointer;
            break;
        }
        case 2: { // VARFIX
            var0 = static_cast<int>(bits.u(2));
            num_rel0 = static_cast<int>(bits.u(2));
            f.num_env = num_rel0 + 1;
            for (int i = 0; i < num_rel0; ++i)
                rel0[static_cast<std::size_t>(i)] = 2 * static_cast<int>(bits.u(2)) + 2;
            pointer = static_cast<int>(bits.u(ceil_log2(f.num_env + 1)));
            for (int l = 0; l < f.num_env; ++l)
                f.freq_res[static_cast<std::size_t>(l)] = bits.flag();
            f.t_e[0] = var0;
            for (int i = 0; i < num_rel0; ++i)
                f.t_e[static_cast<std::size_t>(i + 1)] = f.t_e[static_cast<std::size_t>(i)] + rel0[static_cast<std::size_t>(i)];
            f.t_e[static_cast<std::size_t>(f.num_env)] = k_slots;
            f.l_a = pointer > 1 ? pointer - 1 : -1;
            break;
        }
        default: { // VARVAR: both borders and both counts, then the two lists
            var0 = static_cast<int>(bits.u(2));
            var1 = static_cast<int>(bits.u(2));
            num_rel0 = static_cast<int>(bits.u(2));
            num_rel1 = static_cast<int>(bits.u(2));
            for (int i = 0; i < num_rel0; ++i)
                rel0[static_cast<std::size_t>(i)] = 2 * static_cast<int>(bits.u(2)) + 2;
            for (int i = 0; i < num_rel1; ++i)
                rel1[static_cast<std::size_t>(i)] = 2 * static_cast<int>(bits.u(2)) + 2;
            f.num_env = num_rel0 + num_rel1 + 1;
            if (f.num_env > k_max_env)
                return false;
            pointer = static_cast<int>(bits.u(ceil_log2(f.num_env + 1)));
            for (int l = 0; l < f.num_env; ++l)
                f.freq_res[static_cast<std::size_t>(l)] = bits.flag();
            f.t_e[0] = var0;
            for (int i = 0; i < num_rel0; ++i)
                f.t_e[static_cast<std::size_t>(i + 1)] = f.t_e[static_cast<std::size_t>(i)] + rel0[static_cast<std::size_t>(i)];
            f.t_e[static_cast<std::size_t>(f.num_env)] = k_slots + var1;
            for (int i = 0; i < num_rel1; ++i)
                f.t_e[static_cast<std::size_t>(f.num_env - 1 - i)] = f.t_e[static_cast<std::size_t>(f.num_env - i)] - rel1[static_cast<std::size_t>(i)];
            f.l_a = pointer == 0 ? -1 : f.num_env + 1 - pointer;
            break;
        }
        }
        // The borders must climb within the room the buffers have.
        for (int l = 0; l < f.num_env; ++l) {
            if (f.t_e[static_cast<std::size_t>(l)] < 0 || f.t_e[static_cast<std::size_t>(l + 1)] <= f.t_e[static_cast<std::size_t>(l)]
                || f.t_e[static_cast<std::size_t>(l + 1)] > k_slots + 3)
                return false;
        }
        f.num_noise = f.num_env > 1 ? 2 : 1;
        f.t_q[0] = f.t_e[0];
        if (f.num_env > 1) {
            int index = 0;
            if (f.frame_class == 0)
                index = f.num_env / 2;
            else if (f.frame_class & 1)
                index = f.num_env - std::max(pointer - 1, 1);
            else
                index = pointer == 0 ? 1 : pointer == 1 ? f.num_env - 1 : pointer - 1;
            f.t_q[1] = f.t_e[static_cast<std::size_t>(std::clamp(index, 0, f.num_env))];
            f.t_q[2] = f.t_e[static_cast<std::size_t>(f.num_env)];
        } else {
            f.t_q[1] = f.t_e[1];
        }
        return !bits.failed();
    }

    void read_dtdf(SbrBits& bits, ChannelFrame& f)
    {
        for (int l = 0; l < f.num_env; ++l)
            f.df_env[static_cast<std::size_t>(l)] = bits.flag();
        for (int l = 0; l < f.num_noise; ++l)
            f.df_noise[static_cast<std::size_t>(l)] = bits.flag();
    }

    void read_invf(SbrBits& bits, ChannelFrame& f)
    {
        for (int i = 0; i < tables.n_q; ++i)
            f.invf[static_cast<std::size_t>(i)] = static_cast<int>(bits.u(2));
    }

    // 4.6.18.3.4 with Table 4.65: the envelope's quantized values, each
    // envelope from the one before in time or from its first band in
    // frequency; a band of the resolution the envelope has.
    bool read_envelope(SbrBits& bits, ChannelState& s, ChannelFrame& f, bool balance)
    {
        sbr_data::HuffmanBook const* t_book;
        sbr_data::HuffmanBook const* f_book;
        int start_bits;
        if (balance) {
            t_book = &sbr_data::books[f.amp_res ? 6 : 2];
            f_book = &sbr_data::books[f.amp_res ? 7 : 3];
            start_bits = f.amp_res ? 5 : 6;
        } else {
            t_book = &sbr_data::books[f.amp_res ? 4 : 0];
            f_book = &sbr_data::books[f.amp_res ? 5 : 1];
            start_bits = f.amp_res ? 6 : 7;
        }
        int const scale = balance ? 2 : 1;
        for (int l = 0; l < f.num_env; ++l) {
            bool const res = f.freq_res[static_cast<std::size_t>(l)];
            int const bands = res ? tables.n_high : tables.n_low;
            auto& e = f.env_q[static_cast<std::size_t>(l)];
            if (f.df_env[static_cast<std::size_t>(l)]) {
                // From the envelope before (the previous frame's last for l = 0).
                std::array<int, 64> const& prev = l == 0 ? s.env_prev : f.env_q[static_cast<std::size_t>(l - 1)];
                bool const prev_res = l == 0 ? s.res_prev : f.freq_res[static_cast<std::size_t>(l - 1)];
                for (int k = 0; k < bands; ++k) {
                    std::optional<int> const delta = sbr_read_code(bits, *t_book);
                    if (!delta)
                        return false;
                    int source = k;
                    if (res != prev_res) {
                        if (res) {
                            // The low band holding this high band's start.
                            int const start = tables.f_high[static_cast<std::size_t>(k)];
                            source = 0;
                            while (source + 1 < tables.n_low && tables.f_low[static_cast<std::size_t>(source + 1)] <= start)
                                ++source;
                        } else {
                            // The high band that starts where this low band starts.
                            int const start = tables.f_low[static_cast<std::size_t>(k)];
                            source = 0;
                            while (source < tables.n_high && tables.f_high[static_cast<std::size_t>(source)] < start)
                                ++source;
                        }
                    }
                    e[static_cast<std::size_t>(k)] = prev[static_cast<std::size_t>(std::min(source, 63))] + scale * *delta;
                }
            } else {
                e[0] = scale * static_cast<int>(bits.u(start_bits));
                for (int k = 1; k < bands; ++k) {
                    std::optional<int> const delta = sbr_read_code(bits, *f_book);
                    if (!delta)
                        return false;
                    e[static_cast<std::size_t>(k)] = e[static_cast<std::size_t>(k - 1)] + scale * *delta;
                }
            }
        }
        return !bits.failed();
    }

    bool read_noise(SbrBits& bits, ChannelState& s, ChannelFrame& f, bool balance)
    {
        sbr_data::HuffmanBook const& t_book = sbr_data::books[balance ? 9 : 8];
        sbr_data::HuffmanBook const& f_book = sbr_data::books[balance ? 7 : 5];
        int const scale = balance ? 2 : 1;
        for (int l = 0; l < f.num_noise; ++l) {
            auto& q = f.noise_q[static_cast<std::size_t>(l)];
            if (f.df_noise[static_cast<std::size_t>(l)]) {
                std::array<int, 5> const& prev = l == 0 ? s.noise_prev : f.noise_q[0];
                for (int k = 0; k < tables.n_q; ++k) {
                    std::optional<int> const delta = sbr_read_code(bits, t_book);
                    if (!delta)
                        return false;
                    q[static_cast<std::size_t>(k)] = prev[static_cast<std::size_t>(k)] + scale * *delta;
                }
            } else {
                q[0] = scale * static_cast<int>(bits.u(5));
                for (int k = 1; k < tables.n_q; ++k) {
                    std::optional<int> const delta = sbr_read_code(bits, f_book);
                    if (!delta)
                        return false;
                    q[static_cast<std::size_t>(k)] = q[static_cast<std::size_t>(k - 1)] + scale * *delta;
                }
            }
        }
        return !bits.failed();
    }

    void read_sinusoids(SbrBits& bits, ChannelFrame& f)
    {
        f.add_harmonic.fill(false);
        f.add_harmonic_flag = bits.flag();
        if (f.add_harmonic_flag) {
            for (int i = 0; i < tables.n_high; ++i)
                f.add_harmonic[static_cast<std::size_t>(i)] = bits.flag();
        }
    }

    // 4.6.18.7.1: the envelopes and noise floors as energies.
    void dequantize()
    {
        if (pair && coupling) {
            ChannelFrame& a = ch[0].frame;
            ChannelFrame& b = ch[1].frame;
            float const alpha = a.amp_res ? 1.0f : 0.5f;
            int const pan = a.amp_res ? 12 : 24;
            for (int l = 0; l < a.num_env; ++l) {
                int const bands = a.freq_res[static_cast<std::size_t>(l)] ? tables.n_high : tables.n_low;
                for (int k = 0; k < bands; ++k) {
                    float const temp1 = std::exp2(static_cast<float>(a.env_q[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)]) * alpha + 7);
                    float const temp2 = std::exp2(static_cast<float>(pan - b.env_q[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)]) * alpha);
                    float const fac = temp1 / (1 + temp2);
                    a.env[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)] = fac;
                    b.env[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)] = fac * temp2;
                }
            }
            for (int l = 0; l < a.num_noise; ++l) {
                for (int k = 0; k < tables.n_q; ++k) {
                    float const temp1 = std::exp2(static_cast<float>(7 - a.noise_q[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)]));
                    float const temp2 = std::exp2(static_cast<float>(12 - b.noise_q[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)]));
                    float const fac = temp1 / (1 + temp2);
                    a.noise[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)] = fac;
                    b.noise[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)] = fac * temp2;
                }
            }
            return;
        }
        for (int c = 0; c < (pair ? 2 : 1); ++c) {
            ChannelFrame& f = ch[static_cast<std::size_t>(c)].frame;
            float const alpha = f.amp_res ? 1.0f : 0.5f;
            for (int l = 0; l < f.num_env; ++l) {
                int const bands = f.freq_res[static_cast<std::size_t>(l)] ? tables.n_high : tables.n_low;
                for (int k = 0; k < bands; ++k)
                    f.env[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)]
                        = std::exp2(static_cast<float>(f.env_q[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)]) * alpha + 6);
            }
            for (int l = 0; l < f.num_noise; ++l)
                for (int k = 0; k < tables.n_q; ++k)
                    f.noise[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)] = std::exp2(static_cast<float>(6 - f.noise_q[static_cast<std::size_t>(l)][static_cast<std::size_t>(k)]));
        }
    }

    bool read_data(SbrBits& bits)
    {
        for (ChannelState& s : ch) {
            ChannelFrame fresh;
            // The last envelope's values stay for the delta coding; the rest starts over.
            s.frame = fresh;
        }
        ChannelState& a = ch[0];
        ChannelState& b = ch[1];
        if (!pair) {
            if (bits.flag())
                bits.u(4); // bs_data_extra: reserved
            if (!read_grid(bits, a.frame))
                return false;
            read_dtdf(bits, a.frame);
            read_invf(bits, a.frame);
            if (!read_envelope(bits, a, a.frame, false) || !read_noise(bits, a, a.frame, false))
                return false;
            read_sinusoids(bits, a.frame);
            coupling = false;
        } else {
            if (bits.flag()) {
                bits.u(4);
                bits.u(4);
            }
            coupling = bits.flag();
            if (coupling) {
                if (!read_grid(bits, a.frame))
                    return false;
                b.frame = a.frame;
                read_dtdf(bits, a.frame);
                read_dtdf(bits, b.frame);
                read_invf(bits, a.frame);
                b.frame.invf = a.frame.invf;
                if (!read_envelope(bits, a, a.frame, false) || !read_noise(bits, a, a.frame, false))
                    return false;
                if (!read_envelope(bits, b, b.frame, true) || !read_noise(bits, b, b.frame, true))
                    return false;
            } else {
                if (!read_grid(bits, a.frame) || !read_grid(bits, b.frame))
                    return false;
                read_dtdf(bits, a.frame);
                read_dtdf(bits, b.frame);
                read_invf(bits, a.frame);
                read_invf(bits, b.frame);
                if (!read_envelope(bits, a, a.frame, false) || !read_envelope(bits, b, b.frame, false))
                    return false;
                if (!read_noise(bits, a, a.frame, false) || !read_noise(bits, b, b.frame, false))
                    return false;
            }
            read_sinusoids(bits, a.frame);
            read_sinusoids(bits, b.frame);
        }
        if (bits.failed())
            return false;
        // bs_extended_data: extensions by id, each running to the end
        // of the bits left — parametric stereo (id 2) in a single channel's,
        // once a frame; the others passed over.
        if (bits.flag()) {
            std::size_t count = bits.u(4);
            if (count == 15)
                count += bits.u(8);
            std::size_t left = count * 8;
            bool ps_read = false;
            while (left > 7 && !bits.failed()) {
                int const id = static_cast<int>(bits.u(2));
                left -= 2;
                if (id == 2 && ps && !ps_read) {
                    left -= ps->read(bits, left);
                    ps_read = true;
                } else {
                    std::size_t const bytes = left / 8 * 8;
                    bits.skip(bytes);
                    left -= bytes;
                }
            }
            bits.skip(left);
        }
        dequantize();
        a.frame.present = true;
        if (pair)
            b.frame.present = true;
        return !bits.failed();
    }

    // --- The signal (4.6.18.6 to 4.6.18.8) ---

    // `right`: the state and output of parametric stereo's right channel,
    // made from this one's columns when the stream carries it.
    void process_channel(ChannelState& s, float const* in, float* out, ChannelState* right = nullptr, float* out_right = nullptr)
    {
        // The frame before moves up; the new columns are analysed in.
        for (auto& band : s.low)
            std::copy(band.begin() + k_columns, band.end(), band.begin());
        for (auto& band : s.high)
            std::copy(band.begin() + k_columns, band.end(), band.begin());
        std::copy(s.column_kx.begin() + k_columns, s.column_kx.end(), s.column_kx.begin());
        // The envelopes are energies of the 16-bit signal the encoder had
        // (and the gains' "1 +" assumes that scale): the core's [-1, 1]
        // goes in at it, and comes out of the synthesis back at [-1, 1].
        std::array<float, 32> scaled {};
        for (int l = 0; l < k_columns; ++l) {
            for (int n = 0; n < 32; ++n)
                scaled[static_cast<std::size_t>(n)] = in[32 * l + n] * k_pcm_scale;
            analyze(s, scaled.data(), k_hf_gen + l);
        }

        ChannelFrame& f = s.frame;
        bool const active = have_header && tables.valid && f.present;
        int const history_kx = s.low_kx_prev;
        if (active) {
            generate(s);
            adjust(s);
            // What the next frame continues from.
            int const last = f.num_env - 1;
            s.env_prev = f.env_q[static_cast<std::size_t>(last)];
            s.res_prev = f.freq_res[static_cast<std::size_t>(last)];
            s.noise_prev = f.noise_q[static_cast<std::size_t>(f.num_noise - 1)];
            s.delta_first = f.l_a == f.num_env;
            s.low_kx_prev = tables.kx;
        } else {
            s.low_kx_prev = 32;
            // No high band: the columns of this frame are silent above kx.
            for (int c = k_hf_gen; c < k_buffer; ++c) {
                s.column_kx[static_cast<std::size_t>(c)] = active ? tables.kx : 32;
                for (int k = 0; k < k_bands; ++k)
                    s.high[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)] = Complex(0, 0);
            }
        }
        // Parametric stereo works up to the top of the SBR range.
        if (right)
            ps->begin_frame(have_header && tables.valid ? tables.kx + tables.m : k_low_bands);
        // 4.6.18.8: assembled and synthesized, each output column from the
        // buffer's column two on (tHFAdj).
        for (int l = 0; l < k_columns; ++l) {
            int const c = l + k_hf_adj;
            int const kx = s.column_kx[static_cast<std::size_t>(c)];
            std::array<Complex, 64> x {};
            // X_Low's columns before the frame hold the bands below the
            // previous frame's crossover only.
            int const low_bands = std::min(c < k_hf_gen ? std::min(kx, history_kx) : kx, k_low_bands);
            for (int k = 0; k < low_bands; ++k)
                x[static_cast<std::size_t>(k)] = s.low[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)];
            for (int k = kx; k < k_bands; ++k)
                x[static_cast<std::size_t>(k)] = s.high[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)];
            int const per_column = downsampled ? 32 : 64;
            auto const emit = [&](ChannelState& state, std::array<Complex, 64> const& column, float* to) {
                if (downsampled)
                    synthesize_down(state, column, to + per_column * l);
                else
                    synthesize(state, column, to + per_column * l);
                for (int n = 0; n < per_column; ++n)
                    to[per_column * l + n] /= k_pcm_scale;
            };
            if (right) {
                // The hybrid filters delay QMF bands 0 to 2 by six columns:
                // they are taken six columns on, from the low band's analysis.
                std::array<Complex, 3> ahead {};
                for (int k = 0; k < 3; ++k)
                    ahead[static_cast<std::size_t>(k)] = s.low[static_cast<std::size_t>(k)][static_cast<std::size_t>(c + 6)];
                std::array<Complex, 64> xr {};
                ps->apply(l, x, ahead, xr);
                emit(*right, xr, out_right);
            }
            emit(s, x, out);
        }
    }

    // 4.6.18.6: the high band made from the low by patching through the
    // chirped predictor, over the frame's envelopes' columns.
    void generate(ChannelState& s)
    {
        ChannelFrame const& f = s.frame;
        int const first = 2 * f.t_e[0] + k_hf_adj;
        int const last = 2 * f.t_e[static_cast<std::size_t>(f.num_env)] + k_hf_adj; // exclusive
        // 4.6.18.6.1: the chirp factors by noise band, smoothed over frames.
        std::array<float, 5> bw {};
        for (int i = 0; i < tables.n_q; ++i) {
            // Table 4.170: off and low depend on what the band had before.
            int const mode = f.invf[static_cast<std::size_t>(i)] & 3;
            int const prev_mode = s.invf_prev[static_cast<std::size_t>(i)];
            float value = mode == 0 ? (prev_mode == 1 ? 0.6f : 0.0f) : mode == 1 ? (prev_mode == 0 ? 0.6f : 0.75f) : mode == 2 ? 0.9f : 0.98f;
            float const prev = s.bw_prev[static_cast<std::size_t>(i)];
            value = value < prev ? 0.75f * value + 0.25f * prev : 0.90625f * value + 0.09375f * prev;
            if (value < 0.015625f)
                value = 0;
            if (value > 0.99609375f)
                value = 0.99609375f;
            bw[static_cast<std::size_t>(i)] = value;
        }
        s.bw_prev = bw;
        s.invf_prev = f.invf;
        for (int c = first; c < last && c < k_buffer; ++c)
            s.column_kx[static_cast<std::size_t>(c)] = tables.kx;
        int target = tables.kx;
        for (int p = 0; p < tables.num_patches; ++p) {
            for (int x = 0; x < tables.patch_subbands[static_cast<std::size_t>(p)]; ++x) {
                int const source = tables.patch_start[static_cast<std::size_t>(p)] + x;
                int const k = target++;
                if (source < 0 || source >= k_low_bands || k >= k_bands)
                    continue;
                // 4.6.18.6.2: the predictor of the source band over the frame.
                // X_Low holds the bands below the crossover only (4.6.18.5):
                // this frame's columns cut at its kx, the eight before it at
                // the previous frame's.
                std::array<Complex, k_buffer> low = s.low[static_cast<std::size_t>(source)];
                if (source >= s.low_kx_prev)
                    std::fill(low.begin(), low.begin() + k_hf_gen, Complex(0, 0));
                if (source >= tables.kx)
                    std::fill(low.begin() + k_hf_gen, low.end(), Complex(0, 0));
                // In double: for a tonal band the determinant is a small
                // difference of large products, and float loses it.
                using Wide = std::complex<double>;
                Wide phi01(0, 0), phi02(0, 0), phi11(0, 0), phi12(0, 0), phi22(0, 0);
                // Over the frame's 38 columns, whatever its envelopes span.
                for (int n = k_hf_adj; n < k_buffer; ++n) {
                    Wide const x0(low[static_cast<std::size_t>(n)]);
                    Wide const x1(low[static_cast<std::size_t>(n - 1)]);
                    Wide const x2(low[static_cast<std::size_t>(n - 2)]);
                    phi01 += x0 * std::conj(x1);
                    phi02 += x0 * std::conj(x2);
                    phi11 += x1 * std::conj(x1);
                    phi12 += x1 * std::conj(x2);
                    phi22 += x2 * std::conj(x2);
                }
                double const d = phi22.real() * phi11.real() - std::norm(phi12) / (1 + 1e-6);
                Wide w1(0, 0), w0(0, 0);
                if (d != 0)
                    w1 = (phi01 * phi12 - phi02 * phi11.real()) / d;
                if (phi11.real() != 0)
                    w0 = -(phi01 + w1 * std::conj(phi12)) / phi11.real();
                Complex a0(static_cast<float>(w0.real()), static_cast<float>(w0.imag()));
                Complex a1(static_cast<float>(w1.real()), static_cast<float>(w1.imag()));
                if (std::abs(a0) >= 4 || std::abs(a1) >= 4) {
                    a0 = Complex(0, 0);
                    a1 = Complex(0, 0);
                }
                // The chirp of the noise band the target band is in.
                int band = 0;
                while (band + 1 < tables.n_q && tables.f_noise[static_cast<std::size_t>(band + 1)] <= k)
                    ++band;
                float const b = bw[static_cast<std::size_t>(band)];
                Complex const c0 = a0 * b;
                Complex const c1 = a1 * (b * b);
                auto& high = s.high[static_cast<std::size_t>(k)];
                for (int n = first; n < last && n < k_buffer; ++n)
                    high[static_cast<std::size_t>(n)] = low[static_cast<std::size_t>(n)] + c0 * low[static_cast<std::size_t>(n - 1)] + c1 * low[static_cast<std::size_t>(n - 2)];
            }
        }
        for (int k = target; k < k_bands; ++k)
            for (int n = first; n < last && n < k_buffer; ++n)
                s.high[static_cast<std::size_t>(k)][static_cast<std::size_t>(n)] = Complex(0, 0);
    }

    // 4.6.18.7: envelopes, noise and sinusoids put on the generated band.
    void adjust(ChannelState& s)
    {
        ChannelFrame const& f = s.frame;
        int const m = tables.m;
        int const kx = tables.kx;
        std::array<bool, 64> s_index_last {};
        for (int l = 0; l < f.num_env; ++l) {
            int const c0 = 2 * f.t_e[static_cast<std::size_t>(l)] + k_hf_adj;
            int const c1 = std::min(2 * f.t_e[static_cast<std::size_t>(l + 1)] + k_hf_adj, k_buffer);
            if (c1 <= c0)
                continue;
            bool const res = f.freq_res[static_cast<std::size_t>(l)];
            int const bands = res ? tables.n_high : tables.n_low;
            std::array<int, 65> const& table = res ? tables.f_high : tables.f_low;
            int const lq = (f.num_noise == 2 && f.t_e[static_cast<std::size_t>(l)] >= f.t_q[1]) ? 1 : 0;
            bool const delta = l == f.l_a || (l == 0 && s.delta_first);

            // 4.6.18.7.2: the mapping onto the QMF bands.
            std::array<float, 64> e_mapped {}, q_mapped {};
            std::array<bool, 64> s_index {}, s_mapped {};
            std::array<int, 64> band_of {};
            {
                int band = 0;
                int noise_band = 0;
                for (int i = 0; i < m; ++i) {
                    int const k = kx + i;
                    while (band + 1 < bands && table[static_cast<std::size_t>(band + 1)] <= k)
                        ++band;
                    while (noise_band + 1 < tables.n_q && tables.f_noise[static_cast<std::size_t>(noise_band + 1)] <= k)
                        ++noise_band;
                    band_of[static_cast<std::size_t>(i)] = band;
                    e_mapped[static_cast<std::size_t>(i)] = f.env[static_cast<std::size_t>(l)][static_cast<std::size_t>(band)];
                    q_mapped[static_cast<std::size_t>(i)] = f.noise[static_cast<std::size_t>(lq)][static_cast<std::size_t>(noise_band)];
                }
            }
            if (f.add_harmonic_flag) {
                for (int j = 0; j < tables.n_high; ++j) {
                    int const middle = (tables.f_high[static_cast<std::size_t>(j)] + tables.f_high[static_cast<std::size_t>(j + 1)]) / 2 - kx;
                    if (middle < 0 || middle >= m)
                        continue;
                    bool const wanted = f.add_harmonic[static_cast<std::size_t>(j)] && (l >= f.l_a || s.s_index_prev[static_cast<std::size_t>(middle)]);
                    s_index[static_cast<std::size_t>(middle)] = wanted;
                }
            }
            for (int i = 0; i < m; ++i) {
                if (!s_index[static_cast<std::size_t>(i)])
                    continue;
                for (int j = 0; j < m; ++j)
                    if (band_of[static_cast<std::size_t>(j)] == band_of[static_cast<std::size_t>(i)])
                        s_mapped[static_cast<std::size_t>(j)] = true;
            }
            s_index_last = s_index;

            // 4.6.18.7.3: the energy the generated band has.
            std::array<float, 64> e_curr {};
            float const columns = static_cast<float>(c1 - c0);
            if (header.interpol_freq) {
                for (int i = 0; i < m; ++i) {
                    float sum = 0;
                    auto const& high = s.high[static_cast<std::size_t>(kx + i)];
                    for (int c = c0; c < c1; ++c)
                        sum += std::norm(high[static_cast<std::size_t>(c)]);
                    e_curr[static_cast<std::size_t>(i)] = sum / columns;
                }
            } else {
                for (int band = 0; band < bands; ++band) {
                    int const from = std::max(table[static_cast<std::size_t>(band)], kx) - kx;
                    int const to = std::min(table[static_cast<std::size_t>(band + 1)], kx + m) - kx;
                    if (to <= from)
                        continue;
                    float sum = 0;
                    for (int i = from; i < to; ++i) {
                        auto const& high = s.high[static_cast<std::size_t>(kx + i)];
                        for (int c = c0; c < c1; ++c)
                            sum += std::norm(high[static_cast<std::size_t>(c)]);
                    }
                    float const mean = sum / (columns * static_cast<float>(to - from));
                    for (int i = from; i < to; ++i)
                        e_curr[static_cast<std::size_t>(i)] = mean;
                }
            }

            // 4.6.18.7.4 and 5: the gains, the noise and sinusoid levels,
            // limited by band and boosted back to the envelope's energy.
            std::array<float, 64> g {}, q_m {}, s_m {};
            for (int i = 0; i < m; ++i) {
                float const e = e_mapped[static_cast<std::size_t>(i)];
                float const q = q_mapped[static_cast<std::size_t>(i)];
                float const q_div = q / (1 + q);
                float const q_div2 = 1 / (1 + q);
                q_m[static_cast<std::size_t>(i)] = std::sqrt(e * q_div);
                s_m[static_cast<std::size_t>(i)] = s_index[static_cast<std::size_t>(i)] ? std::sqrt(e * q_div2) : 0.0f;
                float const denominator = 1 + e_curr[static_cast<std::size_t>(i)];
                if (s_mapped[static_cast<std::size_t>(i)])
                    g[static_cast<std::size_t>(i)] = std::sqrt(e * q_div / denominator);
                else if (delta)
                    g[static_cast<std::size_t>(i)] = std::sqrt(e / denominator);
                else
                    g[static_cast<std::size_t>(i)] = std::sqrt(e * q_div2 / denominator);
            }
            float const lim_gain = sbr_data::limiter_gains[header.limiter_gains];
            for (int b = 0; b < tables.n_l; ++b) {
                int const from = std::clamp(tables.f_lim[static_cast<std::size_t>(b)], 0, m);
                int const to = std::clamp(tables.f_lim[static_cast<std::size_t>(b + 1)], 0, m);
                if (to <= from)
                    continue;
                float sum_e = 0, sum_c = 0;
                for (int i = from; i < to; ++i) {
                    sum_e += e_mapped[static_cast<std::size_t>(i)];
                    sum_c += e_curr[static_cast<std::size_t>(i)];
                }
                float g_max = std::sqrt(sum_e / (1e-12f + sum_c) * lim_gain);
                g_max = std::min(g_max, 1e5f);
                for (int i = from; i < to; ++i) {
                    if (g[static_cast<std::size_t>(i)] > g_max) {
                        q_m[static_cast<std::size_t>(i)] *= g_max / g[static_cast<std::size_t>(i)];
                        g[static_cast<std::size_t>(i)] = g_max;
                    }
                }
                float sum_out = 1e-12f;
                for (int i = from; i < to; ++i) {
                    sum_out += e_curr[static_cast<std::size_t>(i)] * g[static_cast<std::size_t>(i)] * g[static_cast<std::size_t>(i)]
                        + s_m[static_cast<std::size_t>(i)] * s_m[static_cast<std::size_t>(i)];
                    if (!s_index[static_cast<std::size_t>(i)] && !delta)
                        sum_out += q_m[static_cast<std::size_t>(i)] * q_m[static_cast<std::size_t>(i)];
                }
                float const boost = std::min(std::sqrt(sum_e / sum_out), 1.584893192f);
                for (int i = from; i < to; ++i) {
                    g[static_cast<std::size_t>(i)] *= boost;
                    q_m[static_cast<std::size_t>(i)] *= boost;
                    s_m[static_cast<std::size_t>(i)] *= boost;
                }
            }

            // 4.6.18.7.6: assembled column by column, the gains smoothed
            // over the four columns before unless the envelope holds a
            // transient or smoothing is off.
            static constexpr float h_smooth[5] = { 0.33333333333333f, 0.30150283239582f, 0.21816949906249f, 0.11516383427084f, 0.03183050093751f };
            bool const smooth = !header.smoothing_mode && !delta;
            if (!s.hist_primed) {
                for (int i = 0; i < m; ++i) {
                    s.g_hist[static_cast<std::size_t>(i)].fill(g[static_cast<std::size_t>(i)]);
                    s.q_hist[static_cast<std::size_t>(i)].fill(q_m[static_cast<std::size_t>(i)]);
                }
                s.hist_primed = true;
            }
            for (int c = c0; c < c1; ++c) {
                static constexpr float phi_re[4] = { 1, 0, -1, 0 };
                static constexpr float phi_im[4] = { 0, 1, 0, -1 };
                for (int i = 0; i < m; ++i) {
                    float g_filt = g[static_cast<std::size_t>(i)];
                    float q_filt = q_m[static_cast<std::size_t>(i)];
                    if (smooth) {
                        auto const& gh = s.g_hist[static_cast<std::size_t>(i)];
                        auto const& qh = s.q_hist[static_cast<std::size_t>(i)];
                        g_filt = g_filt * h_smooth[0] + gh[0] * h_smooth[1] + gh[1] * h_smooth[2] + gh[2] * h_smooth[3] + gh[3] * h_smooth[4];
                        q_filt = q_filt * h_smooth[0] + qh[0] * h_smooth[1] + qh[1] * h_smooth[2] + qh[2] * h_smooth[3] + qh[3] * h_smooth[4];
                    }
                    int const k = kx + i;
                    Complex y = s.high[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)] * g_filt;
                    s.noise_index = (s.noise_index + 1) & 511;
                    if (s_index[static_cast<std::size_t>(i)]) {
                        float const sign = (k & 1) ? -1.0f : 1.0f;
                        y += Complex(s_m[static_cast<std::size_t>(i)] * phi_re[s.sine_index], s_m[static_cast<std::size_t>(i)] * phi_im[s.sine_index] * sign);
                    } else if (!delta) {
                        y += Complex(q_filt * sbr_data::random_phase[s.noise_index][0], q_filt * sbr_data::random_phase[s.noise_index][1]);
                    }
                    s.high[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)] = y;
                }
                // The sinusoids' phase turns a quarter each column (4.6.18.7.5).
                s.sine_index = (s.sine_index + 1) & 3;
                // The history moves on by a column.
                for (int i = 0; i < m; ++i) {
                    auto& gh = s.g_hist[static_cast<std::size_t>(i)];
                    auto& qh = s.q_hist[static_cast<std::size_t>(i)];
                    for (int j = 3; j > 0; --j) {
                        gh[static_cast<std::size_t>(j)] = gh[static_cast<std::size_t>(j - 1)];
                        qh[static_cast<std::size_t>(j)] = qh[static_cast<std::size_t>(j - 1)];
                    }
                    gh[0] = g[static_cast<std::size_t>(i)];
                    qh[0] = q_m[static_cast<std::size_t>(i)];
                }
            }
        }
        s.s_index_prev = s_index_last;
    }
};

SbrDecoder::SbrDecoder(int core_rate, bool channel_pair, bool downsampled)
    : m_impl(std::make_unique<Impl>(core_rate, channel_pair, downsampled))
{
}

SbrDecoder::~SbrDecoder() = default;

bool SbrDecoder::ready() const { return m_impl->have_header && m_impl->tables.valid; }

bool SbrDecoder::parametric_stereo() const { return m_impl->ps && m_impl->ps->active(); }

bool SbrDecoder::read(SbrBits& bits, std::size_t bit_count, bool crc)
{
    std::size_t const start = bits.position();
    if (crc)
        bits.u(10);
    bool ok = true;
    if (bits.flag())
        ok = m_impl->read_header(bits);
    if (ok && m_impl->have_header)
        ok = m_impl->read_data(bits);
    if (bits.position() > start + bit_count)
        ok = false;
    if (!ok) {
        for (ChannelState& s : m_impl->ch)
            s.frame.present = false;
    }
    return ok;
}

void SbrDecoder::process(float const* core_left, float const* core_right, float* out_left, float* out_right)
{
    Impl& in = *m_impl;
    if (in.ps && in.ps->active() && out_right) {
        in.process_channel(in.ch[0], core_left, out_left, &in.ch[1], out_right);
    } else {
        in.process_channel(in.ch[0], core_left, out_left);
    }
    if (in.pair) {
        in.process_channel(in.ch[1], core_right ? core_right : core_left, out_right);
    } else if (out_right && !(in.ps && in.ps->active())) {
        std::copy(out_left, out_left + (in.downsampled ? 1024 : 2048), out_right);
    }
    for (ChannelState& s : in.ch)
        s.frame.present = false;
}

void SbrDecoder::reset()
{
    for (ChannelState& s : m_impl->ch)
        s = ChannelState {};
    if (m_impl->ps)
        m_impl->ps->reset();
}

void sbr_qmf_round_trip(std::span<float const> in, std::vector<float>& out)
{
    auto state = std::make_unique<ChannelState>();
    out.clear();
    for (std::size_t frame = 0; frame + 1024 <= in.size(); frame += 1024) {
        for (int l = 0; l < k_columns; ++l) {
            analyze(*state, in.data() + frame + 32 * static_cast<std::size_t>(l), 0);
            std::array<Complex, 64> x {};
            for (int k = 0; k < k_low_bands; ++k)
                x[static_cast<std::size_t>(k)] = state->low[static_cast<std::size_t>(k)][0];
            std::size_t const at = out.size();
            out.resize(at + 64);
            synthesize(*state, x, out.data() + at);
        }
    }
}

}
