#include "media/Ps.h"

#include "media/Sbr.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <utility>

// Parametric stereo, written from ISO/IEC 14496-3 §8.6.4 and its baseline
// decoder (Annex 8.A): the bitstream (8.3, ps_data), the parameters'
// delta decoding and envelopes, the hybrid analysis of QMF bands 0 to 2,
// the decorrelator and the mixing. Times are in QMF columns, 32 a frame;
// a parameter "bin" is one of the 20 bands the parameters are sent for,
// a "group" one of the 22 runs of (hybrid or QMF) bands that share a bin.

namespace sashfold::media {

namespace {

using Complex = std::complex<float>;

constexpr int k_columns = 32;
constexpr int k_max_env = 5;
constexpr int k_max_bins = 34; // parameters an envelope can carry (the 34-band layout)
constexpr int k_hybrid = 10; // the sub-bands QMF bands 0 to 2 are split into
constexpr int k_hybrid_qmf = 3;
constexpr int k_groups = 22;
constexpr int k_sub_groups = 10; // the groups made of hybrid sub-bands
// QMF bands below 23 are decorrelated by the all-pass chain, those above
// by plain delays: 14 columns up to band 34, one column from band 35 on.
constexpr int k_allpass_end = 23;
constexpr int k_allpass_qmf = k_allpass_end - k_hybrid_qmf;
constexpr int k_allpass_bands = k_hybrid + k_allpass_qmf; // hybrid sub-bands, then QMF bands 3 to 22
constexpr int k_short_delay_start = 35;
constexpr int k_long_delay = 14;
constexpr int k_delay_bands = 64 - k_allpass_end;
constexpr int k_links = 3;
constexpr int k_max_link_delay = 5;

// The decorrelator's constants (8.6.4, decorrelation): the all-pass
// links' delays in columns, their decay, and the fractional delays (in QMF
// bands' share of pi) the links and the two-column delay before them turn
// each band by.
constexpr std::array<int, k_links> k_link_delay = { 3, 4, 5 };
constexpr std::array<float, k_links> k_link_decay = { 0.65143905753106f, 0.56471812200776f, 0.48954165955695f };
constexpr std::array<double, k_links> k_link_fraction = { 0.43, 0.75, 0.347 };
constexpr double k_fraction = 0.39;
constexpr float k_decay_slope = 0.05f;
constexpr int k_decay_cutoff = 3;
// The transient detector's: the peak's decay a column, the smoothing of
// the energies, and how far the peak may stand over the energy.
constexpr float k_peak_decay = 0.76592833836465f;
constexpr float k_smoothing = 0.25f;
constexpr float k_transient_impact = 1.5f;

// The 20-band layout. A group's first band: for the first ten a hybrid
// sub-band (each group is one), then QMF bands up to the next group's.
constexpr std::array<int, k_groups + 1> k_group_border = { 4, 5, 0, 1, 2, 3, 7, 6, 8, 9, 3, 4, 5, 6, 7, 8, 9, 11, 14, 18, 23, 35, 64 };
constexpr std::array<int, k_groups> k_group_bin = { 1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 };
// The hybrid sub-bands' center frequencies, in QMF bands: QMF band 0's
// eight complex sub-bands (two pairs of them summed, the band being
// symmetric about zero), then the two real halves of bands 1 and 2 — of
// band 1 the upper half first, a QMF band of odd index being mirrored.
constexpr std::array<double, k_hybrid> k_hybrid_center = { 0.125, 0.375, 0.625, 0.875, -0.375, -0.125, 1.75, 1.25, 2.25, 2.75 };

// The quantized parameters (8.6.4, dequantization): the inter-channel
// intensity differences in dB, coarse and fine, and the coherences.
constexpr std::array<double, 15> k_iid_coarse = { -25, -18, -14, -10, -7, -4, -2, 0, 2, 4, 7, 10, 14, 18, 25 };
constexpr std::array<double, 31> k_iid_fine
    = { -50, -45, -40, -35, -30, -25, -22, -19, -16, -13, -10, -8, -6, -4, -2, 0, 2, 4, 6, 8, 10, 13, 16, 19, 22, 25, 30, 35, 40, 45, 50 };
constexpr std::array<double, 8> k_icc = { 1, 0.937, 0.84118, 0.60092, 0.36764, 0, -0.589, -1 };

// The hybrid filters' prototypes (8.6.4, hybrid analysis), symmetric
// about their middle tap: the eight-band complex bank's and the two-band
// real one's.
constexpr std::array<double, 7> k_g8 = { 0.00746082949812, 0.02270420949825, 0.04546865930473, 0.07266113929591, 0.09885108575264, 0.11793710567217, 0.125 };
constexpr std::array<double, 7> k_g2 = { 0, 0.01899487526049, 0, -0.07293139167538, 0, 0.30596630545168, 0.5 };

struct Tables {
    std::array<float, 15> scale_coarse {};
    std::array<float, 31> scale_fine {};
    std::array<float, 8> alpha {};
    std::array<std::array<Complex, 13>, 8> g8 {}; // [q][tap]
    std::array<float, 13> g2 {};
    std::array<Complex, k_allpass_bands> phi {};
    std::array<std::array<Complex, k_allpass_bands>, k_links> q {};

    Tables()
    {
        // A channel's gain for an intensity difference c (in dB) between
        // it and the other: sqrt(2 / (1 + 10^(c/10))).
        for (std::size_t i = 0; i < k_iid_coarse.size(); ++i)
            scale_coarse[i] = static_cast<float>(std::sqrt(2 / (1 + std::pow(10.0, k_iid_coarse[i] / 10))));
        for (std::size_t i = 0; i < k_iid_fine.size(); ++i)
            scale_fine[i] = static_cast<float>(std::sqrt(2 / (1 + std::pow(10.0, k_iid_fine[i] / 10))));
        for (std::size_t i = 0; i < k_icc.size(); ++i)
            alpha[i] = static_cast<float>(0.5 * std::acos(k_icc[i]));
        double const pi = std::numbers::pi;
        for (int tap = 0; tap < 13; ++tap) {
            int const mirrored = tap <= 6 ? tap : 12 - tap;
            double const g = k_g8[static_cast<std::size_t>(mirrored)];
            for (int q8 = 0; q8 < 8; ++q8) {
                double const a = 2 * pi / 8 * (q8 + 0.5) * (tap - 6);
                g8[static_cast<std::size_t>(q8)][static_cast<std::size_t>(tap)] = Complex(static_cast<float>(g * std::cos(a)), static_cast<float>(g * std::sin(a)));
            }
            g2[static_cast<std::size_t>(tap)] = static_cast<float>(k_g2[static_cast<std::size_t>(mirrored)]);
        }
        // The fractional delays: exp(-i pi q f) at a band's center f.
        auto const turn = [&](double fraction, double center) {
            double const a = -pi * fraction * center;
            return Complex(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
        };
        for (int b = 0; b < k_allpass_bands; ++b) {
            double const center = b < k_hybrid ? k_hybrid_center[static_cast<std::size_t>(b)] : b - k_hybrid + k_hybrid_qmf + 0.5;
            phi[static_cast<std::size_t>(b)] = turn(k_fraction, center);
            for (int m = 0; m < k_links; ++m)
                q[static_cast<std::size_t>(m)][static_cast<std::size_t>(b)] = turn(k_link_fraction[static_cast<std::size_t>(m)], center);
        }
    }
};

Tables const& tables()
{
    static Tables const t;
    return t;
}

constexpr std::array<int, 3> k_bins_of_mode = { 10, 20, 34 };

// 8.6.4 (baseline): a 34-band envelope's parameters onto the 20 bands.
void map_34_to_20(std::array<int, k_max_bins>& v)
{
    v[0] = (2 * v[0] + v[1]) / 3;
    v[1] = (v[1] + 2 * v[2]) / 3;
    v[2] = (2 * v[3] + v[4]) / 3;
    v[3] = (v[4] + 2 * v[5]) / 3;
    v[4] = (v[6] + v[7]) / 2;
    v[5] = (v[8] + v[9]) / 2;
    v[6] = v[10];
    v[7] = v[11];
    v[8] = (v[12] + v[13]) / 2;
    v[9] = (v[14] + v[15]) / 2;
    v[10] = v[16];
    v[11] = v[17];
    v[12] = v[18];
    v[13] = v[19];
    v[14] = (v[20] + v[21]) / 2;
    v[15] = (v[22] + v[23]) / 2;
    v[16] = (v[24] + v[25]) / 2;
    v[17] = (v[26] + v[27]) / 2;
    v[18] = (v[28] + v[29] + v[30] + v[31]) / 4;
    v[19] = (v[32] + v[33]) / 2;
}

// An envelope's indices from their deltas: along frequency from zero, or
// along time from the envelope before; held to the quantizer's range. A
// coarse (10-bin) envelope is then spread over the 20 bins, two a bin.
void delta_decode(bool enabled, std::array<int, k_max_bins>& v, std::array<int, k_max_bins> const& previous, bool in_time, int count, int stride, int low, int high)
{
    if (enabled) {
        if (!in_time) {
            v[0] = std::clamp(v[0], low, high);
            for (int i = 1; i < count; ++i)
                v[static_cast<std::size_t>(i)] = std::clamp(v[static_cast<std::size_t>(i - 1)] + v[static_cast<std::size_t>(i)], low, high);
        } else {
            for (int i = 0; i < count; ++i)
                v[static_cast<std::size_t>(i)] = std::clamp(previous[static_cast<std::size_t>(i * stride)] + v[static_cast<std::size_t>(i)], low, high);
        }
    } else {
        for (int i = 0; i < count; ++i)
            v[static_cast<std::size_t>(i)] = 0;
    }
    if (stride == 2) {
        for (int i = count * stride - 1; i > 0; --i)
            v[static_cast<std::size_t>(i)] = v[static_cast<std::size_t>(i / stride)];
    }
}

} // namespace

PsMatrix ps_mixing_matrix(bool procedure_b, bool fine_iid, int iid_index, int icc_index)
{
    Tables const& t = tables();
    int const steps = fine_iid ? 15 : 7;
    float const* scale = fine_iid ? t.scale_fine.data() : t.scale_coarse.data();
    PsMatrix m;
    if (!procedure_b) {
        // Procedure A: the pair rotated apart by the coherence's angle,
        // about an axis the level difference sets.
        float const scale_r = scale[steps + iid_index];
        float const scale_l = scale[steps - iid_index];
        float const alpha = t.alpha[static_cast<std::size_t>(icc_index)];
        float const beta = alpha * (scale_r - scale_l) / std::numbers::sqrt2_v<float>;
        double const plus = static_cast<double>(beta + alpha);
        double const minus = static_cast<double>(beta - alpha);
        m.h11 = static_cast<float>(static_cast<double>(scale_l) * std::cos(plus));
        m.h12 = static_cast<float>(static_cast<double>(scale_r) * std::cos(minus));
        m.h21 = static_cast<float>(static_cast<double>(scale_l) * std::sin(plus));
        m.h22 = static_cast<float>(static_cast<double>(scale_r) * std::sin(minus));
        return m;
    }
    // Procedure B: the principal axis of the target covariance (angle a,
    // where tan 2a = 2 c rho / (c^2 - 1), c the left-to-right amplitude
    // ratio) carries the mono signal, the minor axis the decorrelated one,
    // each weighted by the square root of its eigenvalue (angle gamma) —
    // so the pair comes out with the sent level difference and coherence,
    // rho held above 0.05.
    double const db = fine_iid ? k_iid_fine[static_cast<std::size_t>(iid_index + 15)] : k_iid_coarse[static_cast<std::size_t>(iid_index + 7)];
    double const c = std::pow(10.0, db / 20);
    double const rho = std::max(k_icc[static_cast<std::size_t>(icc_index)], 0.05);
    double a = 0.5 * std::atan2(2 * c * rho, c * c - 1);
    if (a < 0)
        a += std::numbers::pi / 2;
    double const sum = c + 1 / c;
    double const mu = std::sqrt(1 + (4 * rho * rho - 4) / (sum * sum));
    double const gamma = std::atan(std::sqrt((1 - mu) / (1 + mu)));
    double const r2 = std::numbers::sqrt2;
    m.h11 = static_cast<float>(r2 * std::cos(a) * std::cos(gamma));
    m.h12 = static_cast<float>(r2 * std::sin(a) * std::cos(gamma));
    m.h21 = static_cast<float>(-r2 * std::sin(a) * std::sin(gamma));
    m.h22 = static_cast<float>(r2 * std::cos(a) * std::sin(gamma));
    return m;
}

struct PsDecoder::Impl {
    bool seen = false;
    // The header's fields, kept until a header changes them.
    bool enable_iid = false;
    bool enable_icc = false;
    bool enable_ext = false;
    bool fine_iid = false;
    int iid_mode = 0; // 0 to 2: 10, 20 or 34 bins
    int icc_mode = 0;
    bool mixing_b = false; // ICC modes 3 to 5: the rotation mixing (procedure B)
    // The frame's data.
    bool data_available = false;
    bool frame_class = false; // variable borders
    int num_env = 0;
    std::array<int, k_max_env + 2> border {}; // the envelopes' first columns, then the frame's end
    std::array<bool, k_max_env> iid_dt {}, icc_dt {};
    std::array<std::array<int, k_max_bins>, k_max_env + 1> iid {}, icc {};
    std::array<int, k_max_bins> iid_prev {}, icc_prev {};
    int next_env = 0;

    // The hybrid analysis: QMF bands 0 to 2's last 13 columns, newest last.
    std::array<std::array<Complex, 13>, k_hybrid_qmf> history {};

    // The decorrelator.
    std::array<std::array<Complex, 2>, k_allpass_bands> delay2 {};
    int delay2_index = 0;
    std::array<std::array<std::array<Complex, k_max_link_delay>, k_allpass_bands>, k_links> link {};
    std::array<int, k_links> link_index {};
    std::array<std::array<Complex, k_long_delay>, k_delay_bands> delay {};
    std::array<int, k_delay_bands> delay_index {};
    std::array<float, 20> peak_decay {}, smooth_energy {}, smooth_peak_diff {};
    int usb = 0;
    int last_usb = 0;
    int pending_usb = 0;

    // The mixing: each group's matrix where it stands, its step a column,
    // and where the envelope before left it.
    std::array<float, k_groups> h11 {}, h12 {}, h21 {}, h22 {};
    std::array<float, k_groups> d11 {}, d12 {}, d21 {}, d22 {};
    std::array<float, k_groups> h11_prev {}, h12_prev {}, h21_prev {}, h22_prev {};

    Impl()
    {
        h11_prev.fill(1);
        h12_prev.fill(1);
    }

    static int delay_length(int band) { return band < k_short_delay_start ? k_long_delay : 1; }

    // The frame's envelopes (8.6.4).
    void decode_frame()
    {
        if (!data_available)
            num_env = 0;
        int const steps = fine_iid ? 15 : 7;
        for (int e = 0; e < num_env; ++e) {
            std::array<int, k_max_bins> const& iid_before = e == 0 ? iid_prev : iid[static_cast<std::size_t>(e - 1)];
            std::array<int, k_max_bins> const& icc_before = e == 0 ? icc_prev : icc[static_cast<std::size_t>(e - 1)];
            delta_decode(enable_iid, iid[static_cast<std::size_t>(e)], iid_before, iid_dt[static_cast<std::size_t>(e)], k_bins_of_mode[static_cast<std::size_t>(iid_mode)],
                iid_mode != 0 ? 1 : 2, -steps, steps);
            delta_decode(enable_icc, icc[static_cast<std::size_t>(e)], icc_before, icc_dt[static_cast<std::size_t>(e)], k_bins_of_mode[static_cast<std::size_t>(icc_mode)],
                icc_mode != 0 ? 1 : 2, 0, 7);
        }
        // No data: one envelope holding the last frame's parameters.
        if (num_env == 0) {
            num_env = 1;
            if (enable_iid)
                iid[0] = iid_prev;
            else
                iid[0].fill(0);
            if (enable_icc)
                icc[0] = icc_prev;
            else
                icc[0].fill(0);
        }
        iid_prev = iid[static_cast<std::size_t>(num_env - 1)];
        icc_prev = icc[static_cast<std::size_t>(num_env - 1)];
        data_available = false;
        // The borders: spread evenly, or as sent — then the frame closed by
        // an envelope like its last when the last border falls short of
        // its end, and each border kept a column clear of its neighbours.
        if (!frame_class) {
            border[0] = 0;
            for (int e = 1; e < num_env; ++e)
                border[static_cast<std::size_t>(e)] = e * k_columns / num_env;
            border[static_cast<std::size_t>(num_env)] = k_columns;
        } else {
            border[0] = 0;
            if (border[static_cast<std::size_t>(num_env)] < k_columns) {
                ++num_env;
                border[static_cast<std::size_t>(num_env)] = k_columns;
                iid[static_cast<std::size_t>(num_env - 1)] = iid[static_cast<std::size_t>(num_env - 2)];
                icc[static_cast<std::size_t>(num_env - 1)] = icc[static_cast<std::size_t>(num_env - 2)];
            }
            for (int e = 1; e < num_env; ++e) {
                int const most = k_columns - num_env + e;
                if (border[static_cast<std::size_t>(e)] > most)
                    border[static_cast<std::size_t>(e)] = most;
                else
                    border[static_cast<std::size_t>(e)] = std::max(border[static_cast<std::size_t>(e)], border[static_cast<std::size_t>(e - 1)] + 1);
            }
        }
        // The baseline decoder works in the 20 bands.
        for (int e = 0; e < num_env; ++e) {
            if (iid_mode == 2)
                map_34_to_20(iid[static_cast<std::size_t>(e)]);
            if (icc_mode == 2)
                map_34_to_20(icc[static_cast<std::size_t>(e)]);
        }
    }

    // 8.6.4, stereo processing: an envelope's mixing matrices, from its
    // parameters; the columns it spans step from the last envelope's to them.
    void begin_envelope(int e)
    {
        if (e == 0) {
            last_usb = usb;
            usb = pending_usb;
            // Bands the SBR range has just grown into start from silence.
            if (usb > last_usb && last_usb != 0) {
                for (int band = last_usb; band < usb; ++band) {
                    if (band < k_allpass_end) {
                        int const b = band - k_hybrid_qmf + k_hybrid;
                        for (int m = 0; m < k_links; ++m)
                            link[static_cast<std::size_t>(m)][static_cast<std::size_t>(b)].fill(Complex(0, 0));
                        delay2[static_cast<std::size_t>(b)].fill(Complex(0, 0));
                    } else {
                        delay[static_cast<std::size_t>(band - k_allpass_end)].fill(Complex(0, 0));
                    }
                }
            }
        }
        int const length = border[static_cast<std::size_t>(e + 1)] - border[static_cast<std::size_t>(e)];
        float const step = 1.0f / static_cast<float>(std::max(length, 1));
        for (int g = 0; g < k_groups; ++g) {
            std::size_t const bin = static_cast<std::size_t>(k_group_bin[static_cast<std::size_t>(g)]);
            PsMatrix const m = ps_mixing_matrix(mixing_b, fine_iid, iid[static_cast<std::size_t>(e)][bin], icc[static_cast<std::size_t>(e)][bin]);
            float const n11 = m.h11, n12 = m.h12, n21 = m.h21, n22 = m.h22;
            std::size_t const gi = static_cast<std::size_t>(g);
            d11[gi] = (n11 - h11_prev[gi]) * step;
            d12[gi] = (n12 - h12_prev[gi]) * step;
            d21[gi] = (n21 - h21_prev[gi]) * step;
            d22[gi] = (n22 - h22_prev[gi]) * step;
            h11[gi] = h11_prev[gi];
            h12[gi] = h12_prev[gi];
            h21[gi] = h21_prev[gi];
            h22[gi] = h22_prev[gi];
            h11_prev[gi] = n11;
            h12_prev[gi] = n12;
            h21_prev[gi] = n21;
            h22_prev[gi] = n22;
        }
    }

    // 8.6.4, hybrid analysis: one column of QMF bands 0 to 2 into the ten
    // hybrid sub-bands.
    void hybrid_analysis(std::array<Complex, 3> const& in, std::array<Complex, k_hybrid>& out)
    {
        Tables const& t = tables();
        for (std::size_t b = 0; b < k_hybrid_qmf; ++b) {
            std::copy(history[b].begin() + 1, history[b].end(), history[b].begin());
            history[b][12] = in[b];
        }
        // Band 0 through the eight complex filters, the sub-bands symmetric
        // about zero summed in pairs.
        std::array<Complex, 8> y {};
        for (std::size_t q8 = 0; q8 < 8; ++q8) {
            Complex sum(0, 0);
            for (std::size_t tap = 0; tap < 13; ++tap)
                sum += t.g8[q8][tap] * history[0][12 - tap];
            y[q8] = sum;
        }
        out[0] = y[0];
        out[1] = y[1];
        out[2] = y[2] + y[5];
        out[3] = y[3] + y[4];
        out[4] = y[6];
        out[5] = y[7];
        // Bands 1 and 2 halved by the real filter and its mirror.
        for (std::size_t b = 1; b < 3; ++b) {
            Complex const middle = t.g2[6] * history[b][6];
            Complex odd(0, 0);
            for (std::size_t tap = 1; tap < 13; tap += 2)
                odd += t.g2[tap] * history[b][12 - tap];
            out[4 + 2 * b] = middle + odd;
            out[5 + 2 * b] = middle - odd;
        }
    }

    void apply(int column, std::array<Complex, 64>& left, std::array<Complex, 3> const& ahead, std::array<Complex, 64>& right)
    {
        if (next_env < num_env && column == border[static_cast<std::size_t>(next_env)])
            begin_envelope(next_env++);
        Tables const& t = tables();
        std::array<Complex, k_hybrid> hl {}, hr {};
        hybrid_analysis(ahead, hl);
        right.fill(Complex(0, 0));
        auto const top = [&](int g) { return g < k_sub_groups ? k_group_border[static_cast<std::size_t>(g)] + 1 : std::min(usb, k_group_border[static_cast<std::size_t>(g + 1)]); };

        // 8.6.4, decorrelation: the transient detector — each bin's energy
        // against its decaying peak; where the peak stands well over it,
        // the decorrelated signal is ducked.
        std::array<float, 20> power {};
        for (int g = 0; g < k_groups; ++g) {
            std::size_t const bin = static_cast<std::size_t>(k_group_bin[static_cast<std::size_t>(g)]);
            for (int k = k_group_border[static_cast<std::size_t>(g)]; k < top(g); ++k) {
                Complex const v = g < k_sub_groups ? hl[static_cast<std::size_t>(k)] : left[static_cast<std::size_t>(k)];
                power[bin] += v.real() * v.real() + v.imag() * v.imag();
            }
        }
        std::array<float, 20> ratio {};
        for (std::size_t bin = 0; bin < 20; ++bin) {
            peak_decay[bin] *= k_peak_decay;
            if (peak_decay[bin] < power[bin])
                peak_decay[bin] = power[bin];
            float peak_diff = smooth_peak_diff[bin] + (peak_decay[bin] - power[bin] - smooth_peak_diff[bin]) * k_smoothing;
            smooth_peak_diff[bin] = peak_diff;
            float const energy = smooth_energy[bin] + (power[bin] - smooth_energy[bin]) * k_smoothing;
            smooth_energy[bin] = energy;
            peak_diff *= k_transient_impact;
            ratio[bin] = peak_diff <= energy ? 1.0f : energy / peak_diff;
        }

        // The decorrelated signal — the low bands through a
        // fractional delay and three all-pass links, the high ones delayed.
        for (int g = 0; g < k_groups; ++g) {
            std::size_t const bin = static_cast<std::size_t>(k_group_bin[static_cast<std::size_t>(g)]);
            bool const sub = g < k_sub_groups;
            for (int k = k_group_border[static_cast<std::size_t>(g)]; k < top(g); ++k) {
                Complex const input = sub ? hl[static_cast<std::size_t>(k)] : left[static_cast<std::size_t>(k)];
                Complex out;
                if (!sub && k >= k_allpass_end) {
                    std::size_t const d = static_cast<std::size_t>(k - k_allpass_end);
                    int& at = delay_index[d];
                    out = delay[d][static_cast<std::size_t>(at)];
                    delay[d][static_cast<std::size_t>(at)] = input;
                    if (++at >= delay_length(k))
                        at = 0;
                } else {
                    std::size_t const b = static_cast<std::size_t>(sub ? k : k - k_hybrid_qmf + k_hybrid);
                    float const decay = (sub || k <= k_decay_cutoff) ? 1.0f : (1.0f + k_decay_cutoff * k_decay_slope) - k_decay_slope * static_cast<float>(k);
                    Complex v = delay2[b][static_cast<std::size_t>(delay2_index)] * t.phi[b];
                    delay2[b][static_cast<std::size_t>(delay2_index)] = input;
                    for (std::size_t m = 0; m < k_links; ++m) {
                        Complex& stored = link[m][b][static_cast<std::size_t>(link_index[m])];
                        float const a = decay * k_link_decay[m];
                        Complex const w = stored * t.q[m][b] - a * v;
                        stored = v + a * w;
                        v = w;
                    }
                    out = v;
                }
                (sub ? hr[static_cast<std::size_t>(k)] : right[static_cast<std::size_t>(k)]) = ratio[bin] * out;
            }
        }
        delay2_index = delay2_index + 1 >= 2 ? 0 : delay2_index + 1;
        for (std::size_t m = 0; m < k_links; ++m) {
            if (++link_index[m] >= k_link_delay[m])
                link_index[m] = 0;
        }

        // 8.6.4, stereo processing: the mixing, the matrices a step further
        // each column.
        for (int g = 0; g < k_groups; ++g) {
            std::size_t const gi = static_cast<std::size_t>(g);
            h11[gi] += d11[gi];
            h12[gi] += d12[gi];
            h21[gi] += d21[gi];
            h22[gi] += d22[gi];
            bool const sub = g < k_sub_groups;
            for (int k = k_group_border[gi]; k < top(g); ++k) {
                Complex& l = sub ? hl[static_cast<std::size_t>(k)] : left[static_cast<std::size_t>(k)];
                Complex& r = sub ? hr[static_cast<std::size_t>(k)] : right[static_cast<std::size_t>(k)];
                Complex const new_l = h11[gi] * l + h21[gi] * r;
                Complex const new_r = h12[gi] * l + h22[gi] * r;
                l = new_l;
                r = new_r;
            }
        }

        // The hybrid synthesis: the sub-bands summed back into their QMF bands.
        left[0] = hl[0] + hl[1] + hl[2] + hl[3] + hl[4] + hl[5];
        left[1] = hl[6] + hl[7];
        left[2] = hl[8] + hl[9];
        right[0] = hr[0] + hr[1] + hr[2] + hr[3] + hr[4] + hr[5];
        right[1] = hr[6] + hr[7];
        right[2] = hr[8] + hr[9];
    }
};

PsDecoder::PsDecoder()
    : m_impl(std::make_unique<Impl>())
{
}

PsDecoder::~PsDecoder() = default;

bool PsDecoder::active() const { return m_impl->seen; }

std::size_t PsDecoder::read(SbrBits& bits, std::size_t bits_left)
{
    Impl& p = *m_impl;
    std::size_t const start = bits.position();
    auto const used = [&]() { return bits.position() - start; };
    p.seen = true;
    if (bits.flag()) { // enable_ps_header
        p.enable_iid = bits.flag();
        if (p.enable_iid) {
            int const mode = static_cast<int>(bits.u(3));
            p.fine_iid = mode > 2;
            p.iid_mode = mode > 2 ? mode - 3 : mode;
        }
        p.enable_icc = bits.flag();
        if (p.enable_icc) {
            int const mode = static_cast<int>(bits.u(3));
            // 3 to 5: the same layouts, mixed by procedure B.
            p.mixing_b = mode > 2;
            p.icc_mode = mode > 2 ? mode - 3 : mode;
        }
        p.enable_ext = bits.flag();
    }
    p.frame_class = bits.flag();
    if (!p.frame_class) {
        static constexpr int fixed[4] = { 0, 1, 2, 4 };
        p.num_env = fixed[bits.u(2)];
    } else {
        p.num_env = static_cast<int>(bits.u(2)) + 1;
        for (int e = 1; e <= p.num_env; ++e)
            p.border[static_cast<std::size_t>(e)] = static_cast<int>(bits.u(5)) + 1;
    }
    // A layout the standard reserves: the rest of the extension passed over.
    if (p.iid_mode > 2 || p.icc_mode > 2) {
        p.data_available = false;
        if (used() < bits_left)
            bits.skip(bits_left - used());
        return bits_left;
    }
    bool ok = true;
    if (p.enable_iid) {
        for (int e = 0; e < p.num_env && ok; ++e) {
            bool const dt = bits.flag();
            sbr_data::HuffmanBook const& book = sbr_data::ps_books[(p.fine_iid ? 2 : 0) + (dt ? 0 : 1)];
            for (int b = 0; b < k_bins_of_mode[static_cast<std::size_t>(p.iid_mode)] && ok; ++b) {
                std::optional<int> const delta = sbr_read_code(bits, book);
                ok = delta.has_value();
                p.iid[static_cast<std::size_t>(e)][static_cast<std::size_t>(b)] = delta.value_or(0);
            }
            p.iid_dt[static_cast<std::size_t>(e)] = dt;
        }
    }
    if (p.enable_icc) {
        for (int e = 0; e < p.num_env && ok; ++e) {
            bool const dt = bits.flag();
            sbr_data::HuffmanBook const& book = sbr_data::ps_books[dt ? 4 : 5];
            for (int b = 0; b < k_bins_of_mode[static_cast<std::size_t>(p.icc_mode)] && ok; ++b) {
                std::optional<int> const delta = sbr_read_code(bits, book);
                ok = delta.has_value();
                p.icc[static_cast<std::size_t>(e)][static_cast<std::size_t>(b)] = delta.value_or(0);
            }
            p.icc_dt[static_cast<std::size_t>(e)] = dt;
        }
    }
    // ps_extension: the phase parameters, which the baseline passes over.
    if (ok && p.enable_ext) {
        std::size_t count = bits.u(4);
        if (count == 15)
            count += bits.u(8);
        bits.skip(count * 8);
    }
    p.data_available = ok && !bits.failed() && used() <= bits_left;
    return std::min(used(), bits_left);
}

void PsDecoder::begin_frame(int usb)
{
    Impl& p = *m_impl;
    p.decode_frame();
    p.pending_usb = usb;
    p.next_env = 0;
}

void PsDecoder::apply(int column, std::array<Complex, 64>& left, std::array<Complex, 3> const& ahead, std::array<Complex, 64>& right)
{
    m_impl->apply(column, left, ahead, right);
}

void PsDecoder::reset()
{
    // The filters' and parameters' history goes; what the stream has said
    // (that it carries parametric stereo, and the last header) stays.
    Impl& p = *m_impl;
    auto fresh = std::make_unique<Impl>();
    fresh->seen = p.seen;
    fresh->enable_iid = p.enable_iid;
    fresh->enable_icc = p.enable_icc;
    fresh->enable_ext = p.enable_ext;
    fresh->fine_iid = p.fine_iid;
    fresh->iid_mode = p.iid_mode;
    fresh->icc_mode = p.icc_mode;
    fresh->mixing_b = p.mixing_b;
    m_impl = std::move(fresh);
}

}
