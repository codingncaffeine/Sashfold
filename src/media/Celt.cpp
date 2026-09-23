#include "media/Celt.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <span>

// The decoder follows RFC 6716 §4.3 and its normative reference, with the
// corrections of RFC 8251 (the cap on band energy, and the folding of the
// second band of a hybrid frame). The tables below are the specification's
// own for the one mode Opus uses: 48 kHz, 20 ms frames, 21 bands.

namespace sashfold::media {

namespace {

constexpr int band_count = 21;
constexpr int max_lm = 3;
constexpr int short_mdct_size = 120;
constexpr int overlap = 120;
constexpr int decode_buffer_size = 2048; // the post-filter reaches up to 1,026 samples back
constexpr int bit_resolution = 3; // allocation is counted in eighths of a bit
constexpr int max_fine_bits = 8;
constexpr int fine_offset = 21;
constexpr int qtheta_offset = 4;
constexpr int qtheta_offset_two_phase = 16;
constexpr int log_max_pseudo = 6;
constexpr int alloc_steps = 6;
constexpr int spread_none = 0;
constexpr int spread_normal = 2;
constexpr int spread_aggressive = 3;
constexpr int combfilter_min_period = 15;
constexpr float preemphasis = 0.85000610f;

// The band edges in units of 200 Hz at 2.5 ms (eband5ms): band i covers
// bins [e[i], e[i+1]) times the frame's multiple M.
constexpr std::array<int, band_count + 1> band_edges = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 34, 40, 48, 60,
    78, 100 };

int band_width(int band) { return band_edges[static_cast<std::size_t>(band + 1)] - band_edges[static_cast<std::size_t>(band)]; }

// The allocation table, in 1/32 bit a sample, eleven quality steps of 21
// bands (§4.3.3, Table 57).
constexpr int alloc_vector_count = 11;
constexpr unsigned char band_allocation[alloc_vector_count * band_count] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //
    90, 80, 75, 69, 63, 56, 49, 40, 34, 29, 20, 18, 10, 0, 0, 0, 0, 0, 0, 0, 0, //
    110, 100, 90, 84, 78, 71, 65, 58, 51, 45, 39, 32, 26, 20, 12, 0, 0, 0, 0, 0, 0, //
    118, 110, 103, 93, 86, 80, 75, 70, 65, 59, 53, 47, 40, 31, 23, 15, 4, 0, 0, 0, 0, //
    126, 119, 112, 104, 95, 89, 83, 78, 72, 66, 60, 54, 47, 39, 32, 25, 17, 12, 1, 0, 0, //
    134, 127, 120, 114, 103, 97, 91, 85, 78, 72, 66, 60, 54, 47, 41, 35, 29, 23, 16, 10, 1, //
    144, 137, 130, 124, 113, 107, 101, 95, 88, 82, 76, 70, 64, 57, 51, 45, 39, 33, 26, 15, 1, //
    152, 145, 138, 132, 123, 117, 111, 105, 98, 92, 86, 80, 74, 67, 61, 55, 49, 43, 36, 20, 1, //
    162, 155, 148, 142, 133, 127, 121, 115, 108, 102, 96, 90, 84, 77, 71, 65, 59, 53, 46, 30, 1, //
    172, 165, 158, 152, 143, 137, 131, 125, 118, 112, 106, 100, 94, 87, 81, 75, 69, 63, 56, 45, 20, //
    200, 200, 200, 200, 200, 200, 200, 200, 198, 193, 188, 183, 178, 173, 168, 163, 158, 153, 148, 129, 104, //
};

// log2 of each band's width in eighths of a bit.
constexpr std::array<int, band_count> log_n = { 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 16, 16, 16, 21, 21, 24, 29, 34, 36 };

// The pulse cache (§4.3.4.3): for each frame size and band, the bits K
// pulses cost, indexed through cache_index; and each band's cap.
constexpr short cache_index[105] = {
    -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 41, 41, 41, 82, 82, 123, 164, 200, 222, 0, 0, 0, 0, 0, 0, 0, 0, 41, 41, 41, 41, 123,
    123, 123, 164, 164, 240, 266, 283, 295, 41, 41, 41, 41, 41, 41, 41, 41, 123, 123, 123, 123, 240, 240, 240, 266, 266, 305, 318,
    328, 336, 123, 123, 123, 123, 123, 123, 123, 123, 240, 240, 240, 240, 305, 305, 305, 318, 318, 343, 351, 358, 364, 240, 240,
    240, 240, 240, 240, 240, 240, 305, 305, 305, 305, 343, 343, 343, 351, 351, 370, 376, 382, 387,
};
constexpr unsigned char cache_bits[392] = {
    40, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 40,
    15, 23, 28, 31, 34, 36, 38, 39, 41, 42, 43, 44, 45, 46, 47, 47, 49, 50, 51, 52, 53, 54, 55, 55, 57, 58, 59, 60, 61, 62, 63, 63,
    65, 66, 67, 68, 69, 70, 71, 71, 40, 20, 33, 41, 48, 53, 57, 61, 64, 66, 69, 71, 73, 75, 76, 78, 80, 82, 85, 87, 89, 91, 92, 94,
    96, 98, 101, 103, 105, 107, 108, 110, 112, 114, 117, 119, 121, 123, 124, 126, 128, 40, 23, 39, 51, 60, 67, 73, 79, 83, 87, 91,
    94, 97, 100, 102, 105, 107, 111, 115, 118, 121, 124, 126, 129, 131, 135, 139, 142, 145, 148, 150, 153, 155, 159, 163, 166,
    169, 172, 174, 177, 179, 35, 28, 49, 65, 78, 89, 99, 107, 114, 120, 126, 132, 136, 141, 145, 149, 153, 159, 165, 171, 176,
    180, 185, 189, 192, 199, 205, 211, 216, 220, 225, 229, 232, 239, 245, 251, 21, 33, 58, 79, 97, 112, 125, 137, 148, 157, 166,
    174, 182, 189, 195, 201, 207, 217, 227, 235, 243, 251, 17, 35, 63, 86, 106, 123, 139, 152, 165, 177, 187, 197, 206, 214, 222,
    230, 237, 250, 25, 31, 55, 75, 91, 105, 117, 128, 138, 146, 154, 161, 168, 174, 180, 185, 190, 200, 208, 215, 222, 229, 235,
    240, 245, 255, 16, 36, 65, 89, 110, 128, 144, 159, 173, 185, 196, 207, 217, 226, 234, 242, 250, 11, 41, 74, 103, 128, 151,
    172, 191, 209, 225, 241, 255, 9, 43, 79, 110, 138, 163, 186, 207, 227, 246, 12, 39, 71, 99, 123, 144, 164, 182, 198, 214,
    228, 241, 253, 9, 44, 81, 113, 142, 168, 192, 214, 235, 255, 7, 49, 90, 127, 160, 191, 220, 247, 6, 51, 95, 134, 170, 203,
    234, 7, 47, 87, 123, 155, 184, 212, 237, 6, 52, 97, 137, 174, 208, 240, 5, 57, 106, 151, 192, 231, 5, 59, 111, 158, 202, 243,
    5, 55, 103, 147, 187, 224, 5, 60, 113, 161, 206, 248, 4, 65, 122, 175, 224, 4, 67, 127, 182, 234,
};
constexpr unsigned char cache_caps[168] = {
    224, 224, 224, 224, 224, 224, 224, 224, 160, 160, 160, 160, 185, 185, 185, 178, 178, 168, 134, 61, 37, 224, 224, 224, 224,
    224, 224, 224, 224, 240, 240, 240, 240, 207, 207, 207, 198, 198, 183, 144, 66, 40, 160, 160, 160, 160, 160, 160, 160, 160,
    185, 185, 185, 185, 193, 193, 193, 183, 183, 172, 138, 64, 38, 240, 240, 240, 240, 240, 240, 240, 240, 207, 207, 207, 207,
    204, 204, 204, 193, 193, 180, 143, 66, 40, 185, 185, 185, 185, 185, 185, 185, 185, 193, 193, 193, 193, 193, 193, 193, 183,
    183, 172, 138, 65, 39, 207, 207, 207, 207, 207, 207, 207, 207, 204, 204, 204, 204, 201, 201, 201, 188, 188, 176, 141, 66,
    40, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 194, 194, 194, 184, 184, 173, 139, 65, 39, 204, 204, 204,
    204, 204, 204, 204, 204, 201, 201, 201, 201, 198, 198, 198, 187, 187, 175, 140, 66, 40,
};

// The Laplace model of the coarse energy (§4.3.2.1): for each frame size,
// inter and intra, the probability of zero and the decay, per band.
constexpr unsigned char energy_model[4][2][42] = {
    { { 72, 127, 65, 129, 66, 128, 65, 128, 64, 128, 62, 128, 64, 128, 64, 128, 92, 78, 92, 79, 92, 78, 90, 79, 116, 41, 115, 40,
          114, 40, 132, 26, 132, 26, 145, 17, 161, 12, 176, 10, 177, 11 },
        { 24, 179, 48, 138, 54, 135, 54, 132, 53, 134, 56, 133, 55, 132, 55, 132, 61, 114, 70, 96, 74, 88, 75, 88, 87, 74, 89, 66,
            91, 67, 100, 59, 108, 50, 120, 40, 122, 37, 97, 43, 78, 50 } },
    { { 83, 78, 84, 81, 88, 75, 86, 74, 87, 71, 90, 73, 93, 74, 93, 74, 109, 40, 114, 36, 117, 34, 117, 34, 143, 17, 145, 18, 146,
          19, 162, 12, 165, 10, 178, 7, 189, 6, 190, 8, 177, 9 },
        { 23, 178, 54, 115, 63, 102, 66, 98, 69, 99, 74, 89, 71, 91, 73, 91, 78, 89, 86, 80, 92, 66, 93, 64, 102, 59, 103, 60,
            104, 60, 117, 52, 123, 44, 138, 35, 133, 31, 97, 38, 77, 45 } },
    { { 61, 90, 93, 60, 105, 42, 107, 41, 110, 45, 116, 38, 113, 38, 112, 38, 124, 26, 132, 27, 136, 19, 140, 20, 155, 14, 159,
          16, 158, 18, 170, 13, 177, 10, 187, 8, 192, 6, 175, 9, 159, 10 },
        { 21, 178, 59, 110, 71, 86, 75, 85, 84, 83, 91, 66, 88, 73, 87, 72, 92, 75, 98, 72, 105, 58, 107, 54, 115, 52, 114, 55,
            112, 56, 129, 51, 132, 40, 150, 33, 140, 29, 98, 35, 77, 42 } },
    { { 42, 121, 96, 66, 108, 43, 111, 40, 117, 44, 123, 32, 120, 36, 119, 33, 127, 33, 134, 34, 139, 21, 147, 23, 152, 20, 158,
          25, 154, 26, 166, 21, 173, 16, 184, 13, 184, 10, 150, 13, 139, 15 },
        { 22, 178, 63, 114, 74, 82, 84, 83, 92, 82, 103, 62, 96, 72, 96, 67, 101, 73, 107, 72, 113, 55, 118, 52, 125, 52, 118, 52,
            117, 55, 135, 49, 137, 39, 157, 32, 145, 29, 97, 33, 77, 40 } },
};

// The mean energy of each band, in base-2 log units, that the coded
// energies are relative to.
constexpr float energy_means[25] = { 6.437500f, 6.250000f, 5.750000f, 5.312500f, 5.062500f, 4.812500f, 4.500000f, 4.375000f,
    4.875000f, 4.687500f, 4.562500f, 4.437500f, 4.875000f, 4.625000f, 4.312500f, 4.500000f, 4.375000f, 4.625000f, 4.750000f,
    4.437500f, 3.750000f, 3.750000f, 3.750000f, 3.750000f, 3.750000f };
constexpr float prediction_coefficient[4] = { 29440 / 32768.f, 26112 / 32768.f, 21248 / 32768.f, 16384 / 32768.f };
constexpr float beta_coefficient[4] = { 30147 / 32768.f, 22282 / 32768.f, 12124 / 32768.f, 6554 / 32768.f };
constexpr float beta_intra = 4915 / 32768.f;

constexpr unsigned char small_energy_icdf[3] = { 2, 1, 0 };
constexpr unsigned char trim_icdf[11] = { 126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0 };
constexpr unsigned char spread_icdf[4] = { 25, 23, 2, 0 };
constexpr unsigned char tapset_icdf[3] = { 2, 1, 0 };
constexpr signed char tf_select_table[4][8] = {
    { 0, -1, 0, -1, 0, -1, 0, -1 },
    { 0, -1, 0, -2, 1, 0, 1, -1 },
    { 0, -2, 0, -3, 2, 0, 1, -1 },
    { 0, -2, 0, -3, 3, 0, 1, -1 },
};
constexpr unsigned char log2_frac_table[24] = { 0, 8, 13, 16, 19, 21, 23, 24, 26, 27, 28, 29, 30, 31, 32, 32, 33, 34, 34, 35,
    36, 36, 37, 37 };

// The overlap window (§4.3.7): the power-complementary sine of a sine.
std::array<float, overlap> const& window()
{
    static std::array<float, overlap> const table = [] {
        std::array<float, overlap> w {};
        double const pi = 3.141592653589793;
        for (int i = 0; i < overlap; ++i) {
            double const s = std::sin(0.5 * pi * (i + 0.5) / overlap);
            w[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(0.5 * pi * s * s));
        }
        return w;
    }();
    return table;
}

std::uint32_t lcg_rand(std::uint32_t seed)
{
    return 1664525u * seed + 1013904223u;
}

// FRAC_MUL16: a product of two 16-bit values in Q15, rounded, exactly as
// the specification's integer paths compute it.
int frac_mul16(int a, int b)
{
    return (16384 + static_cast<std::int32_t>(static_cast<std::int16_t>(a)) * static_cast<std::int16_t>(b)) >> 15;
}

// A cosine every platform computes to the same bits, since it decides how
// bits are split between the halves of a band (§4.3.4.3).
int bitexact_cos(int x)
{
    std::int32_t const tmp = (4096 + static_cast<std::int32_t>(x) * x) >> 13;
    int x2 = static_cast<std::int16_t>(tmp);
    x2 = (32767 - x2) + frac_mul16(x2, (-7651 + frac_mul16(x2, (8277 + frac_mul16(-626, x2)))));
    return 1 + x2;
}

int bitexact_log2tan(int isin, int icos)
{
    int const lc = ilog(static_cast<std::uint32_t>(icos));
    int const ls = ilog(static_cast<std::uint32_t>(isin));
    icos <<= 15 - lc;
    isin <<= 15 - ls;
    return (ls - lc) * (1 << 11) + frac_mul16(isin, frac_mul16(isin, -2597) + 7932) - frac_mul16(icos, frac_mul16(icos, -2597) + 7932);
}

unsigned isqrt32(std::uint32_t value)
{
    unsigned g = 0;
    int bshift = (ilog(value) - 1) >> 1;
    unsigned b = 1u << bshift;
    do {
        std::uint32_t const t = ((static_cast<std::uint32_t>(g) << 1) + b) << bshift;
        if (t <= value) {
            g += b;
            value -= t;
        }
        b >>= 1;
        bshift--;
    } while (bshift >= 0);
    return g;
}

// --- The pulse cache -------------------------------------------------------------------------

unsigned char const* cache_for(int band, int lm)
{
    return cache_bits + cache_index[(lm + 1) * band_count + band];
}

int get_pulses(int i)
{
    return i < 8 ? i : (8 + (i & 7)) << ((i >> 3) - 1);
}

int bits_to_pulses(int band, int lm, int bits)
{
    unsigned char const* cache = cache_for(band, lm);
    int lo = 0;
    int hi = cache[0];
    bits--;
    for (int i = 0; i < log_max_pseudo; ++i) {
        int const mid = (lo + hi + 1) >> 1;
        if (cache[mid] >= bits)
            hi = mid;
        else
            lo = mid;
    }
    if (bits - (lo == 0 ? -1 : cache[lo]) <= cache[hi] - bits)
        return lo;
    return hi;
}

int pulses_to_bits(int band, int lm, int pulses)
{
    unsigned char const* cache = cache_for(band, lm);
    return pulses == 0 ? 0 : cache[pulses] + 1;
}

// --- The pulse vector (§4.3.4.2): an index into the vectors of K unit
// pulses in N dimensions, and back to the vector ---------------------------------------

void u_next(std::uint32_t* u, unsigned len, std::uint32_t u0)
{
    unsigned j = 1;
    do {
        std::uint32_t const u1 = u[j] + u[j - 1] + u0;
        u[j - 1] = u0;
        u0 = u1;
    } while (++j < len);
    u[j - 1] = u0;
}

void u_previous(std::uint32_t* u, unsigned n, std::uint32_t u0)
{
    unsigned j = 1;
    do {
        std::uint32_t const u1 = u[j] - u[j - 1] - u0;
        u[j - 1] = u0;
        u0 = u1;
    } while (++j < n);
    u[j - 1] = u0;
}

// U(n, 0..k+1), and V(n, k) = U(n, k) + U(n, k+1): the count of vectors.
std::uint32_t u_row(unsigned n, unsigned k, std::uint32_t* u)
{
    unsigned const len = k + 2;
    u[0] = 0;
    u[1] = 1;
    for (unsigned i = 2; i < len; ++i)
        u[i] = (i << 1) - 1;
    for (unsigned i = 2; i < n; ++i)
        u_next(u + 1, k + 1, 1);
    return u[k] + u[k + 1];
}

void decode_pulses(int* y, int n, int k, RangeDecoder& dec)
{
    std::vector<std::uint32_t> u(static_cast<std::size_t>(k) + 2);
    std::uint32_t index = dec.uint(u_row(static_cast<unsigned>(n), static_cast<unsigned>(k), u.data()));
    int j = 0;
    do {
        std::uint32_t p = u[static_cast<std::size_t>(k) + 1];
        int const s = -static_cast<int>(index >= p);
        index -= p & static_cast<std::uint32_t>(s);
        int yj = k;
        p = u[static_cast<std::size_t>(k)];
        while (p > index)
            p = u[static_cast<std::size_t>(--k)];
        index -= p;
        yj -= k;
        y[j] = (yj + s) ^ s;
        u_previous(u.data(), static_cast<unsigned>(k) + 2, 0);
    } while (++j < n);
}

// --- Rotation, normalisation and the other shapes of a band ----------------------------------

void rotate_once(float* x, int len, int stride, float c, float s)
{
    float* p = x;
    for (int i = 0; i < len - stride; ++i) {
        float const x1 = p[0];
        float const x2 = p[stride];
        p[stride] = c * x2 + s * x1;
        *p++ = c * x1 - s * x2;
    }
    p = &x[len - 2 * stride - 1];
    for (int i = len - 2 * stride - 1; i >= 0; --i) {
        float const x1 = p[0];
        float const x2 = p[stride];
        p[stride] = c * x2 + s * x1;
        *p-- = c * x1 - s * x2;
    }
}

// The spreading rotation (§4.3.4.3): pulses a decoder sees as too
// concentrated are spread over their neighbours, by an angle that grows as
// the pulses thin out. The decoder undoes the encoder's rotation.
void undo_spreading(float* x, int len, int stride, int k, int spread)
{
    static constexpr int spread_factor[3] = { 15, 10, 5 };
    if (2 * k >= len || spread == spread_none)
        return;
    int const factor = spread_factor[spread - 1];
    float const gain = static_cast<float>(len) / static_cast<float>(len + factor * k);
    float const theta = 0.5f * (gain * gain);
    float const pi = 3.141592653f;
    float const c = static_cast<float>(std::cos(static_cast<double>((0.5f * pi) * theta)));
    float const s = static_cast<float>(std::cos(static_cast<double>((0.5f * pi) * (1.0f - theta))));
    int stride2 = 0;
    if (len >= 8 * stride) {
        stride2 = 1;
        while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len)
            stride2++;
    }
    len /= stride;
    for (int i = 0; i < stride; ++i) {
        if (stride2)
            rotate_once(x + i * len, len, stride2, s, c);
        rotate_once(x + i * len, len, 1, c, s);
    }
}

void renormalise(float* x, int n, float gain)
{
    float energy = 1e-15f;
    for (int i = 0; i < n; ++i)
        energy += x[i] * x[i];
    float const g = (1.0f / std::sqrt(energy)) * gain;
    for (int i = 0; i < n; ++i)
        x[i] *= g;
}

unsigned collapse_mask(int const* y, int n, int b)
{
    if (b <= 1)
        return 1;
    int const n0 = n / b;
    unsigned mask = 0;
    for (int i = 0; i < b; ++i) {
        for (int j = 0; j < n0; ++j)
            mask |= static_cast<unsigned>(y[i * n0 + j] != 0) << i;
    }
    return mask;
}

// A band's shape from its K pulses: the vector read, made unit length and
// scaled, and the spreading undone.
unsigned unquantize_pulses(float* x, int n, int k, int spread, int b, RangeDecoder& dec, float gain)
{
    std::vector<int> y(static_cast<std::size_t>(n));
    decode_pulses(y.data(), n, k, dec);
    float energy = 0;
    for (int i = 0; i < n; ++i)
        energy += static_cast<float>(y[static_cast<std::size_t>(i)]) * static_cast<float>(y[static_cast<std::size_t>(i)]);
    float const g = (1.0f / std::sqrt(energy)) * gain;
    for (int i = 0; i < n; ++i)
        x[i] = g * static_cast<float>(y[static_cast<std::size_t>(i)]);
    undo_spreading(x, n, b, k, spread);
    return collapse_mask(y.data(), n, b);
}

constexpr int ordery_table[] = {
    1, 0, //
    3, 0, 2, 1, //
    7, 0, 4, 3, 6, 1, 5, 2, //
    15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5,
};

void deinterleave_hadamard(float* x, int n0, int stride, bool hadamard)
{
    int const n = n0 * stride;
    std::vector<float> tmp(static_cast<std::size_t>(n));
    if (hadamard) {
        int const* ordery = ordery_table + stride - 2;
        for (int i = 0; i < stride; ++i)
            for (int j = 0; j < n0; ++j)
                tmp[static_cast<std::size_t>(ordery[i] * n0 + j)] = x[j * stride + i];
    } else {
        for (int i = 0; i < stride; ++i)
            for (int j = 0; j < n0; ++j)
                tmp[static_cast<std::size_t>(i * n0 + j)] = x[j * stride + i];
    }
    std::copy(tmp.begin(), tmp.end(), x);
}

void interleave_hadamard(float* x, int n0, int stride, bool hadamard)
{
    int const n = n0 * stride;
    std::vector<float> tmp(static_cast<std::size_t>(n));
    if (hadamard) {
        int const* ordery = ordery_table + stride - 2;
        for (int i = 0; i < stride; ++i)
            for (int j = 0; j < n0; ++j)
                tmp[static_cast<std::size_t>(j * stride + i)] = x[ordery[i] * n0 + j];
    } else {
        for (int i = 0; i < stride; ++i)
            for (int j = 0; j < n0; ++j)
                tmp[static_cast<std::size_t>(j * stride + i)] = x[i * n0 + j];
    }
    std::copy(tmp.begin(), tmp.end(), x);
}

void haar1(float* x, int n0, int stride)
{
    n0 >>= 1;
    for (int i = 0; i < stride; ++i)
        for (int j = 0; j < n0; ++j) {
            float const t1 = 0.70710678f * x[stride * 2 * j + i];
            float const t2 = 0.70710678f * x[stride * (2 * j + 1) + i];
            x[stride * 2 * j + i] = t1 + t2;
            x[stride * (2 * j + 1) + i] = t1 - t2;
        }
}

void stereo_merge(float* x, float* y, float mid, int n)
{
    float xp = 0;
    float side = 0;
    for (int j = 0; j < n; ++j) {
        xp += x[j] * y[j];
        side += y[j] * y[j];
    }
    xp = mid * xp;
    float const mid2 = mid;
    float const el = mid2 * mid2 + side - 2 * xp;
    float const er = mid2 * mid2 + side + 2 * xp;
    if (er < 6e-4f || el < 6e-4f) {
        std::copy(x, x + n, y);
        return;
    }
    float const lgain = 1.0f / std::sqrt(el);
    float const rgain = 1.0f / std::sqrt(er);
    for (int j = 0; j < n; ++j) {
        float const l = mid * x[j];
        float const r = y[j];
        x[j] = lgain * (l - r);
        y[j] = rgain * (l + r);
    }
}

int compute_qn(int n, int b, int offset, int pulse_cap, bool stereo)
{
    static constexpr std::int16_t exp2_table8[8] = { 16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048 };
    int n2 = 2 * n - 1;
    if (stereo && n == 2)
        n2--;
    int qb = std::min(b - pulse_cap - (4 << bit_resolution), (b + n2 * offset) / n2);
    qb = std::min(8 << bit_resolution, qb);
    if (qb < (1 << bit_resolution >> 1))
        return 1;
    int qn = exp2_table8[qb & 0x7] >> (14 - (qb >> bit_resolution));
    qn = (qn + 1) >> 1 << 1;
    return qn;
}

// --- The bands (§4.3.4): each band's shape, recursively split in two by an
// angle until it is small enough to code as one vector of pulses ---------

struct BandContext {
    RangeDecoder& dec;
    int band = 0;
    int spread = 0;
    int intensity = 0;
    int tf_change = 0;
    std::int32_t remaining_bits = 0;
    std::uint32_t* seed = nullptr;
    float* lowband_scratch = nullptr;
};

unsigned decode_band(BandContext& ctx, float* x, float* y, int n, int b, int blocks, float* lowband, int lm, float* lowband_out,
    int level, float gain, float* scratch, unsigned fill)
{
    int const n0 = n;
    int n_b = n;
    int b0 = blocks;
    int time_divide = 0;
    int recombine = 0;
    bool inv = false;
    float mid = 0;
    float side = 0;
    bool const long_blocks = b0 == 1;
    unsigned cm = 0;

    n_b /= blocks;
    int n_b0 = n_b;
    bool const stereo = y != nullptr;
    bool split = stereo;

    // A band of one coefficient is only a sign.
    if (n == 1) {
        float* target = x;
        for (int c = 0; c < 1 + (stereo ? 1 : 0); ++c) {
            int sign = 0;
            if (ctx.remaining_bits >= 1 << bit_resolution) {
                sign = static_cast<int>(ctx.dec.bits(1));
                ctx.remaining_bits -= 1 << bit_resolution;
                b -= 1 << bit_resolution;
            }
            target[0] = sign ? -1.0f : 1.0f;
            target = y;
        }
        if (lowband_out)
            lowband_out[0] = x[0];
        return 1;
    }

    if (!stereo && level == 0) {
        int tf_change = ctx.tf_change;
        if (tf_change > 0)
            recombine = tf_change;
        // Band recombining to increase frequency resolution, on a copy of
        // the band being folded from.
        if (lowband && (recombine || ((n_b & 1) == 0 && tf_change < 0) || b0 > 1)) {
            std::copy(lowband, lowband + n, scratch);
            lowband = scratch;
        }
        static constexpr unsigned char bit_interleave_table[16] = { 0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3 };
        for (int k = 0; k < recombine; ++k) {
            if (lowband)
                haar1(lowband, n >> k, 1 << k);
            fill = bit_interleave_table[fill & 0xF] | static_cast<unsigned>(bit_interleave_table[fill >> 4]) << 2;
        }
        blocks >>= recombine;
        n_b <<= recombine;
        // Increasing the time resolution.
        while ((n_b & 1) == 0 && tf_change < 0) {
            if (lowband)
                haar1(lowband, n_b, blocks);
            fill |= fill << blocks;
            blocks <<= 1;
            n_b >>= 1;
            time_divide++;
            tf_change++;
        }
        b0 = blocks;
        n_b0 = n_b;
        // Samples in time order rather than frequency order.
        if (b0 > 1 && lowband)
            deinterleave_hadamard(lowband, n_b >> recombine, b0 << recombine, long_blocks);
    }

    // With more than one and a half bits beyond what the band's largest
    // vector would take, it is split in two.
    if (!stereo && lm != -1 && n > 2 && b > cache_for(ctx.band, lm)[cache_for(ctx.band, lm)[0]] + 12) {
        n >>= 1;
        y = x + n;
        split = true;
        lm -= 1;
        if (blocks == 1)
            fill = (fill & 1) | (fill << 1);
        blocks = (blocks + 1) >> 1;
    }

    if (split) {
        // The angle between the halves, at a resolution the bits allow.
        int const pulse_cap = log_n[static_cast<std::size_t>(ctx.band)] + lm * (1 << bit_resolution);
        int const offset = (pulse_cap >> 1) - (stereo && n == 2 ? qtheta_offset_two_phase : qtheta_offset);
        int qn = compute_qn(n, b, offset, pulse_cap, stereo);
        if (stereo && ctx.band >= ctx.intensity)
            qn = 1;
        int itheta = 0;
        auto const tell = static_cast<std::int32_t>(ctx.dec.tell_frac());
        if (qn != 1) {
            if (stereo && n > 2) {
                // A step: probability 3 up to half the angles, then 1.
                int const p0 = 3;
                int const x0 = qn / 2;
                unsigned const ft = static_cast<unsigned>(p0 * (x0 + 1) + x0);
                int const fs = static_cast<int>(ctx.dec.decode(ft));
                int xv = fs < (x0 + 1) * p0 ? fs / p0 : x0 + 1 + (fs - (x0 + 1) * p0);
                ctx.dec.update(static_cast<unsigned>(xv <= x0 ? p0 * xv : (xv - 1 - x0) + (x0 + 1) * p0),
                    static_cast<unsigned>(xv <= x0 ? p0 * (xv + 1) : (xv - x0) + (x0 + 1) * p0), ft);
                itheta = xv;
            } else if (b0 > 1 || stereo) {
                itheta = static_cast<int>(ctx.dec.uint(static_cast<std::uint32_t>(qn + 1)));
            } else {
                // Triangular.
                int const ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
                int const fm = static_cast<int>(ctx.dec.decode(static_cast<unsigned>(ft)));
                int fl;
                int fs;
                if (fm < ((qn >> 1) * ((qn >> 1) + 1) >> 1)) {
                    itheta = static_cast<int>((isqrt32(8 * static_cast<std::uint32_t>(fm) + 1) - 1) >> 1);
                    fs = itheta + 1;
                    fl = itheta * (itheta + 1) >> 1;
                } else {
                    itheta = static_cast<int>((2 * (qn + 1) - static_cast<int>(isqrt32(8 * static_cast<std::uint32_t>(ft - fm - 1) + 1))) >> 1);
                    fs = qn + 1 - itheta;
                    fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
                }
                ctx.dec.update(static_cast<unsigned>(fl), static_cast<unsigned>(fl + fs), static_cast<unsigned>(ft));
            }
            itheta = static_cast<int>(static_cast<std::int32_t>(itheta) * 16384 / qn);
        } else if (stereo) {
            inv = b > 2 << bit_resolution && ctx.remaining_bits > 2 << bit_resolution ? ctx.dec.bit_logp(2) : false;
            itheta = 0;
        }
        int const qalloc = static_cast<int>(static_cast<std::int32_t>(ctx.dec.tell_frac()) - tell);
        b -= qalloc;

        unsigned const orig_fill = fill;
        int imid;
        int iside;
        int delta;
        if (itheta == 0) {
            imid = 32767;
            iside = 0;
            fill &= (1u << blocks) - 1;
            delta = -16384;
        } else if (itheta == 16384) {
            imid = 0;
            iside = 32767;
            fill &= ((1u << blocks) - 1) << blocks;
            delta = 16384;
        } else {
            imid = bitexact_cos(itheta);
            iside = bitexact_cos(16384 - itheta);
            delta = frac_mul16((n - 1) << 7, bitexact_log2tan(iside, imid));
        }
        mid = (1.f / 32768) * static_cast<float>(imid);
        side = (1.f / 32768) * static_cast<float>(iside);

        if (n == 2 && stereo) {
            // Two coefficients a side, orthogonal: the side is one sign.
            int mbits = b;
            int sbits = 0;
            if (itheta != 0 && itheta != 16384)
                sbits = 1 << bit_resolution;
            mbits -= sbits;
            bool const c = itheta > 8192;
            ctx.remaining_bits -= qalloc + sbits;
            float* x2 = c ? y : x;
            float* y2 = c ? x : y;
            int sign = 0;
            if (sbits)
                sign = static_cast<int>(ctx.dec.bits(1));
            sign = 1 - 2 * sign;
            cm = decode_band(ctx, x2, nullptr, n, mbits, blocks, lowband, lm, lowband_out, level, gain, scratch, orig_fill);
            y2[0] = static_cast<float>(-sign) * x2[1];
            y2[1] = static_cast<float>(sign) * x2[0];
            x[0] = mid * x[0];
            x[1] = mid * x[1];
            y[0] = side * y[0];
            y[1] = side * y[1];
            float tmp = x[0];
            x[0] = tmp - y[0];
            y[0] = tmp + y[0];
            tmp = x[1];
            x[1] = tmp - y[1];
            y[1] = tmp + y[1];
        } else {
            // Low-energy short blocks get more than their share.
            if (b0 > 1 && !stereo && (itheta & 0x3fff)) {
                if (itheta > 8192)
                    delta -= delta >> (4 - lm);
                else
                    delta = std::min(0, delta + (n << bit_resolution >> (5 - lm)));
            }
            int mbits = std::max(0, std::min(b, (b - delta) / 2));
            int sbits = b - mbits;
            ctx.remaining_bits -= qalloc;
            float* next_lowband2 = lowband && !stereo ? lowband + n : nullptr;
            float* next_lowband_out1 = stereo ? lowband_out : nullptr;
            int const next_level = stereo ? 0 : level + 1;
            std::int32_t rebalance = ctx.remaining_bits;
            unsigned const side_shift = static_cast<unsigned>((b0 >> 1) & (stereo ? 0 : -1));
            if (mbits >= sbits) {
                // The mid of a stereo split stays unit length, for folding.
                cm = decode_band(ctx, x, nullptr, n, mbits, blocks, lowband, lm, next_lowband_out1, next_level,
                    stereo ? 1.0f : gain * mid, scratch, fill);
                rebalance = mbits - (rebalance - ctx.remaining_bits);
                if (rebalance > 3 << bit_resolution && itheta != 0)
                    sbits += rebalance - (3 << bit_resolution);
                cm |= decode_band(ctx, y, nullptr, n, sbits, blocks, next_lowband2, lm, nullptr, next_level, gain * side, nullptr,
                          fill >> blocks)
                    << side_shift;
            } else {
                cm = decode_band(ctx, y, nullptr, n, sbits, blocks, next_lowband2, lm, nullptr, next_level, gain * side, nullptr,
                         fill >> blocks)
                    << side_shift;
                rebalance = sbits - (rebalance - ctx.remaining_bits);
                if (rebalance > 3 << bit_resolution && itheta != 16384)
                    mbits += rebalance - (3 << bit_resolution);
                cm |= decode_band(ctx, x, nullptr, n, mbits, blocks, lowband, lm, next_lowband_out1, next_level,
                    stereo ? 1.0f : gain * mid, scratch, fill);
            }
        }
    } else {
        // Not split: one vector of pulses, as many as the bits buy.
        int q = bits_to_pulses(ctx.band, lm, b);
        int curr_bits = pulses_to_bits(ctx.band, lm, q);
        ctx.remaining_bits -= curr_bits;
        while (ctx.remaining_bits < 0 && q > 0) {
            ctx.remaining_bits += curr_bits;
            q--;
            curr_bits = pulses_to_bits(ctx.band, lm, q);
            ctx.remaining_bits -= curr_bits;
        }
        if (q != 0) {
            cm = unquantize_pulses(x, n, get_pulses(q), ctx.spread, blocks, ctx.dec, gain);
        } else {
            // No pulses: the band is filled anyway, from what was coded
            // below it, or with noise when there is nothing to fold.
            unsigned const cm_mask = static_cast<unsigned>((1ul << blocks) - 1);
            fill &= cm_mask;
            if (!fill) {
                std::fill(x, x + n, 0.0f);
            } else {
                if (lowband == nullptr) {
                    for (int j = 0; j < n; ++j) {
                        *ctx.seed = lcg_rand(*ctx.seed);
                        x[j] = static_cast<float>(static_cast<std::int32_t>(*ctx.seed) >> 20);
                    }
                    cm = cm_mask;
                } else {
                    for (int j = 0; j < n; ++j) {
                        *ctx.seed = lcg_rand(*ctx.seed);
                        float const tmp = (*ctx.seed & 0x8000) ? 1.0f / 256 : -1.0f / 256;
                        x[j] = lowband[j] + tmp;
                    }
                    cm = fill;
                }
                renormalise(x, n, gain);
            }
        }
    }

    // Put back what was taken apart on the way in.
    if (stereo) {
        if (n != 2)
            stereo_merge(x, y, mid, n);
        if (inv) {
            for (int j = 0; j < n; ++j)
                y[j] = -y[j];
        }
    } else if (level == 0) {
        if (b0 > 1)
            interleave_hadamard(x, n_b >> recombine, b0 << recombine, long_blocks);
        n_b = n_b0;
        blocks = b0;
        for (int k = 0; k < time_divide; ++k) {
            blocks >>= 1;
            n_b <<= 1;
            cm |= cm >> blocks;
            haar1(x, n_b, blocks);
        }
        static constexpr unsigned char bit_deinterleave_table[16] = { 0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F, 0xC0, 0xC3,
            0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF };
        for (int k = 0; k < recombine; ++k) {
            cm = bit_deinterleave_table[cm];
            haar1(x, n0 >> k, 1 << k);
        }
        blocks <<= recombine;
        // Scaled for folding into the bands above.
        if (lowband_out) {
            float const scale = std::sqrt(static_cast<float>(n0));
            for (int j = 0; j < n0; ++j)
                lowband_out[j] = scale * x[j];
        }
        cm &= (1u << blocks) - 1;
    }
    return cm;
}

void decode_all_bands(int start, int end, float* x_all, float* y_all, unsigned char* collapse_masks, int const* pulses,
    int short_blocks, int spread, int dual_stereo, int intensity, int const* tf_res, std::int32_t total_bits, std::int32_t balance,
    RangeDecoder& dec, int lm, int coded_bands, std::uint32_t* seed)
{
    int const m = 1 << lm;
    int const blocks = short_blocks ? m : 1;
    int const channels = y_all != nullptr ? 2 : 1;
    std::vector<float> norm_storage(static_cast<std::size_t>(channels * m * band_edges[band_count]));
    std::vector<float> lowband_scratch(static_cast<std::size_t>(m * band_width(band_count - 1)));
    float* norm = norm_storage.data();
    float* norm2 = norm + m * band_edges[band_count];
    int lowband_offset = 0;
    bool update_lowband = true;
    BandContext ctx { dec };
    ctx.spread = spread;
    ctx.intensity = intensity;
    ctx.seed = seed;
    ctx.lowband_scratch = lowband_scratch.data();

    for (int i = start; i < end; ++i) {
        float* x = x_all + m * band_edges[static_cast<std::size_t>(i)];
        float* y = y_all ? y_all + m * band_edges[static_cast<std::size_t>(i)] : nullptr;
        int const n = m * band_edges[static_cast<std::size_t>(i) + 1] - m * band_edges[static_cast<std::size_t>(i)];
        auto const tell = static_cast<std::int32_t>(dec.tell_frac());
        if (i != start)
            balance -= tell;
        ctx.remaining_bits = total_bits - tell - 1;
        int b = 0;
        if (i <= coded_bands - 1) {
            std::int32_t const curr_balance = balance / std::min(3, coded_bands - i);
            b = std::max(0, static_cast<int>(std::min<std::int32_t>(16383, std::min(ctx.remaining_bits + 1, pulses[i] + curr_balance))));
        }
        if ((m * band_edges[static_cast<std::size_t>(i)] - n >= m * band_edges[static_cast<std::size_t>(start)] || i == start + 1)
            && (update_lowband || lowband_offset == 0))
            lowband_offset = i;
        // RFC 8251 §9: the second band of a hybrid frame folds from a copy
        // of the first that is long enough (nothing is copied for CELT alone).
        if (i == start + 1) {
            int const n1 = m * band_width(start);
            int const n2 = m * band_width(start + 1);
            int const offset = m * band_edges[static_cast<std::size_t>(start)];
            if (n2 > n1) {
                std::memmove(&norm[offset + n1], &norm[offset + 2 * n1 - n2], static_cast<std::size_t>(n2 - n1) * sizeof(float));
                if (channels == 2)
                    std::memmove(&norm2[offset + n1], &norm2[offset + 2 * n1 - n2], static_cast<std::size_t>(n2 - n1) * sizeof(float));
            }
        }
        ctx.tf_change = tf_res[i];
        ctx.band = i;

        int effective_lowband = -1;
        unsigned x_cm;
        unsigned y_cm;
        if (lowband_offset != 0 && (spread != spread_aggressive || blocks > 1 || ctx.tf_change < 0)) {
            effective_lowband = std::max(m * band_edges[static_cast<std::size_t>(start)], m * band_edges[static_cast<std::size_t>(lowband_offset)] - n);
            int fold_start = lowband_offset;
            while (m * band_edges[static_cast<std::size_t>(--fold_start)] > effective_lowband) { }
            int fold_end = lowband_offset - 1;
            while (++fold_end < i && m * band_edges[static_cast<std::size_t>(fold_end)] < effective_lowband + n) { }
            x_cm = y_cm = 0;
            int fold_i = fold_start;
            do {
                x_cm |= collapse_masks[fold_i * channels + 0];
                y_cm |= collapse_masks[fold_i * channels + channels - 1];
            } while (++fold_i < fold_end);
        } else {
            x_cm = y_cm = (1u << blocks) - 1;
        }

        if (dual_stereo && i == intensity) {
            // Dual stereo gives way to intensity stereo from here up.
            dual_stereo = 0;
            for (int j = m * band_edges[static_cast<std::size_t>(start)]; j < m * band_edges[static_cast<std::size_t>(i)]; ++j)
                norm[j] = 0.5f * (norm[j] + norm2[j]);
        }
        if (dual_stereo) {
            x_cm = decode_band(ctx, x, nullptr, n, b / 2, blocks, effective_lowband != -1 ? norm + effective_lowband : nullptr, lm,
                norm + m * band_edges[static_cast<std::size_t>(i)], 0, 1.0f, ctx.lowband_scratch, x_cm);
            y_cm = decode_band(ctx, y, nullptr, n, b / 2, blocks, effective_lowband != -1 ? norm2 + effective_lowband : nullptr, lm,
                norm2 + m * band_edges[static_cast<std::size_t>(i)], 0, 1.0f, ctx.lowband_scratch, y_cm);
        } else {
            x_cm = decode_band(ctx, x, y, n, b, blocks, effective_lowband != -1 ? norm + effective_lowband : nullptr, lm,
                norm + m * band_edges[static_cast<std::size_t>(i)], 0, 1.0f, ctx.lowband_scratch, x_cm | y_cm);
            y_cm = x_cm;
        }
        collapse_masks[i * channels + 0] = static_cast<unsigned char>(x_cm);
        collapse_masks[i * channels + channels - 1] = static_cast<unsigned char>(y_cm);
        balance += pulses[i] + tell;
        // The folding point moves up only while bands have a bit a sample.
        update_lowband = b > (n << bit_resolution);
    }
}

// --- The allocation (§4.3.3) ----------------------------------------------------------------------

int interpolate_allocation(int start, int end, int skip_start, int const* bits1, int const* bits2, int const* thresh, int const* cap,
    std::int32_t total, std::int32_t* balance_out, int skip_rsv, int* intensity, int intensity_rsv, int* dual_stereo,
    int dual_stereo_rsv, int* bits, int* ebits, int* fine_priority, int channels, int lm, RangeDecoder& dec)
{
    int const alloc_floor = channels << bit_resolution;
    int const stereo = channels > 1 ? 1 : 0;
    int const log_m = lm << bit_resolution;
    int lo = 0;
    int hi = 1 << alloc_steps;
    for (int i = 0; i < alloc_steps; ++i) {
        int const mid = (lo + hi) >> 1;
        std::int32_t psum = 0;
        bool done = false;
        for (int j = end; j-- > start;) {
            int const tmp = bits1[j] + static_cast<int>(mid * static_cast<std::int32_t>(bits2[j]) >> alloc_steps);
            if (tmp >= thresh[j] || done) {
                done = true;
                psum += std::min(tmp, cap[j]);
            } else if (tmp >= alloc_floor) {
                psum += alloc_floor;
            }
        }
        if (psum > total)
            hi = mid;
        else
            lo = mid;
    }
    std::int32_t psum = 0;
    bool done = false;
    for (int j = end; j-- > start;) {
        int tmp = bits1[j] + (lo * bits2[j] >> alloc_steps);
        if (tmp < thresh[j] && !done)
            tmp = tmp >= alloc_floor ? alloc_floor : 0;
        else
            done = true;
        tmp = std::min(tmp, cap[j]);
        bits[j] = tmp;
        psum += tmp;
    }

    // Which bands to skip, from the top down.
    int coded_bands = end;
    for (;; coded_bands--) {
        int const j = coded_bands - 1;
        if (j <= skip_start) {
            total += skip_rsv;
            break;
        }
        std::int32_t left = total - psum;
        std::int32_t const percoeff = left / (band_edges[static_cast<std::size_t>(coded_bands)] - band_edges[static_cast<std::size_t>(start)]);
        left -= (band_edges[static_cast<std::size_t>(coded_bands)] - band_edges[static_cast<std::size_t>(start)]) * percoeff;
        int const rem = std::max(static_cast<int>(left) - (band_edges[static_cast<std::size_t>(j)] - band_edges[static_cast<std::size_t>(start)]), 0);
        int const width = band_edges[static_cast<std::size_t>(coded_bands)] - band_edges[static_cast<std::size_t>(j)];
        int band_bits = static_cast<int>(bits[j] + percoeff * width + rem);
        if (band_bits >= std::max(thresh[j], alloc_floor + (1 << bit_resolution))) {
            if (dec.bit_logp(1))
                break;
            psum += 1 << bit_resolution;
            band_bits -= 1 << bit_resolution;
        }
        psum -= bits[j] + intensity_rsv;
        if (intensity_rsv > 0)
            intensity_rsv = log2_frac_table[j - start];
        psum += intensity_rsv;
        if (band_bits >= alloc_floor) {
            psum += alloc_floor;
            bits[j] = alloc_floor;
        } else {
            bits[j] = 0;
        }
    }

    if (intensity_rsv > 0)
        *intensity = start + static_cast<int>(dec.uint(static_cast<std::uint32_t>(coded_bands + 1 - start)));
    else
        *intensity = 0;
    if (*intensity <= start) {
        total += dual_stereo_rsv;
        dual_stereo_rsv = 0;
    }
    *dual_stereo = dual_stereo_rsv > 0 ? static_cast<int>(dec.bit_logp(1)) : 0;

    // What is left is shared by width, then the remainder a bin at a time.
    std::int32_t left = total - psum;
    std::int32_t const percoeff = left / (band_edges[static_cast<std::size_t>(coded_bands)] - band_edges[static_cast<std::size_t>(start)]);
    left -= (band_edges[static_cast<std::size_t>(coded_bands)] - band_edges[static_cast<std::size_t>(start)]) * percoeff;
    for (int j = start; j < coded_bands; ++j)
        bits[j] += static_cast<int>(percoeff) * band_width(j);
    for (int j = start; j < coded_bands; ++j) {
        int const tmp = static_cast<int>(std::min<std::int32_t>(left, band_width(j)));
        bits[j] += tmp;
        left -= tmp;
    }

    std::int32_t balance = 0;
    int j = start;
    for (; j < coded_bands; ++j) {
        int const n0 = band_width(j);
        int const n = n0 << lm;
        bits[j] += balance;
        int excess;
        if (n > 1) {
            excess = std::max(bits[j] - cap[j], 0);
            bits[j] -= excess;
            // The extra degree of freedom of a stereo band.
            int const den = channels * n + ((channels == 2 && n > 2 && !*dual_stereo && j < *intensity) ? 1 : 0);
            int const nc_log_n = den * (log_n[static_cast<std::size_t>(j)] + log_m);
            int offset = (nc_log_n >> 1) - den * fine_offset;
            if (n == 2)
                offset += den << bit_resolution >> 2;
            if (bits[j] + offset < den * 2 << bit_resolution)
                offset += nc_log_n >> 2;
            else if (bits[j] + offset < den * 3 << bit_resolution)
                offset += nc_log_n >> 3;
            ebits[j] = std::max(0, (bits[j] + offset + (den << (bit_resolution - 1))) / (den << bit_resolution));
            if (channels * ebits[j] > (bits[j] >> bit_resolution))
                ebits[j] = bits[j] >> stereo >> bit_resolution;
            ebits[j] = std::min(ebits[j], max_fine_bits);
            fine_priority[j] = ebits[j] * (den << bit_resolution) >= bits[j] + offset;
            bits[j] -= channels * ebits[j] << bit_resolution;
        } else {
            // One coefficient: all but its sign goes to fine energy.
            excess = std::max(0, bits[j] - (channels << bit_resolution));
            bits[j] -= excess;
            ebits[j] = 0;
            fine_priority[j] = 1;
        }
        if (excess > 0) {
            int const extra_fine = std::min(excess >> (stereo + bit_resolution), max_fine_bits - ebits[j]);
            ebits[j] += extra_fine;
            int const extra_bits = extra_fine * channels << bit_resolution;
            fine_priority[j] = extra_bits >= excess - balance;
            excess -= extra_bits;
        }
        balance = excess;
    }
    *balance_out = balance;
    // Skipped bands spend all they have on fine energy.
    for (; j < end; ++j) {
        ebits[j] = bits[j] >> stereo >> bit_resolution;
        bits[j] = 0;
        fine_priority[j] = ebits[j] < 1;
    }
    return coded_bands;
}

int compute_allocation(int start, int end, int const* offsets, int const* cap, int alloc_trim, int* intensity, int* dual_stereo,
    std::int32_t total, std::int32_t* balance, int* pulses, int* ebits, int* fine_priority, int channels, int lm, RangeDecoder& dec)
{
    total = std::max<std::int32_t>(total, 0);
    int skip_start = start;
    int const skip_rsv = total >= 1 << bit_resolution ? 1 << bit_resolution : 0;
    total -= skip_rsv;
    int intensity_rsv = 0;
    int dual_stereo_rsv = 0;
    if (channels == 2) {
        intensity_rsv = log2_frac_table[end - start];
        if (intensity_rsv > total) {
            intensity_rsv = 0;
        } else {
            total -= intensity_rsv;
            dual_stereo_rsv = total >= 1 << bit_resolution ? 1 << bit_resolution : 0;
            total -= dual_stereo_rsv;
        }
    }
    std::array<int, band_count> bits1 {};
    std::array<int, band_count> bits2 {};
    std::array<int, band_count> thresh {};
    std::array<int, band_count> trim_offset {};
    for (int j = start; j < end; ++j) {
        int const width = band_width(j);
        // Below this, no bits go to the band's vector at all.
        thresh[static_cast<std::size_t>(j)] = std::max(channels << bit_resolution, (3 * width << lm << bit_resolution) >> 4);
        // The tilt of the allocation.
        trim_offset[static_cast<std::size_t>(j)] = channels * width * (alloc_trim - 5 - lm) * (end - j - 1) * (1 << (lm + bit_resolution)) >> 6;
        if (width << lm == 1)
            trim_offset[static_cast<std::size_t>(j)] -= channels << bit_resolution;
    }
    int lo = 1;
    int hi = alloc_vector_count - 1;
    do {
        bool done = false;
        int psum = 0;
        int const mid = (lo + hi) >> 1;
        for (int j = end; j-- > start;) {
            int const n = band_width(j);
            int bitsj = channels * n * band_allocation[mid * band_count + j] << lm >> 2;
            if (bitsj > 0)
                bitsj = std::max(0, bitsj + trim_offset[static_cast<std::size_t>(j)]);
            bitsj += offsets[j];
            if (bitsj >= thresh[static_cast<std::size_t>(j)] || done) {
                done = true;
                psum += std::min(bitsj, cap[j]);
            } else if (bitsj >= channels << bit_resolution) {
                psum += channels << bit_resolution;
            }
        }
        if (psum > total)
            hi = mid - 1;
        else
            lo = mid + 1;
    } while (lo <= hi);
    hi = lo--;
    for (int j = start; j < end; ++j) {
        int const n = band_width(j);
        int bits1j = channels * n * band_allocation[lo * band_count + j] << lm >> 2;
        int bits2j = hi >= alloc_vector_count ? cap[j] : channels * n * band_allocation[hi * band_count + j] << lm >> 2;
        if (bits1j > 0)
            bits1j = std::max(0, bits1j + trim_offset[static_cast<std::size_t>(j)]);
        if (bits2j > 0)
            bits2j = std::max(0, bits2j + trim_offset[static_cast<std::size_t>(j)]);
        if (lo > 0)
            bits1j += offsets[j];
        bits2j += offsets[j];
        if (offsets[j] > 0)
            skip_start = j;
        bits2j = std::max(0, bits2j - bits1j);
        bits1[static_cast<std::size_t>(j)] = bits1j;
        bits2[static_cast<std::size_t>(j)] = bits2j;
    }
    return interpolate_allocation(start, end, skip_start, bits1.data(), bits2.data(), thresh.data(), cap, total, balance, skip_rsv,
        intensity, intensity_rsv, dual_stereo, dual_stereo_rsv, pulses, ebits, fine_priority, channels, lm, dec);
}

// --- Energy (§4.3.2) ------------------------------------------------------------------------------------

int laplace_decode(RangeDecoder& dec, unsigned fs, int decay)
{
    constexpr unsigned minp = 1;
    constexpr unsigned nmin = 16;
    int value = 0;
    unsigned const fm = dec.decode_bin(15);
    unsigned fl = 0;
    if (fm >= fs) {
        value++;
        fl = fs;
        fs = ((32768 - minp * (2 * nmin) - fs) * static_cast<unsigned>(16384 - decay) >> 15) + minp;
        while (fs > minp && fm >= fl + 2 * fs) {
            fs *= 2;
            fl += fs;
            fs = ((fs - 2 * minp) * static_cast<unsigned>(decay)) >> 15;
            fs += minp;
            value++;
        }
        if (fs <= minp) {
            unsigned const di = (fm - fl) >> 1;
            value += static_cast<int>(di);
            fl += 2 * di * minp;
        }
        if (fm < fl + fs)
            value = -value;
        else
            fl += fs;
    }
    dec.update(fl, std::min(fl + fs, 32768u), 32768);
    return value;
}

void decode_coarse_energy(int start, int end, float* old_energy, bool intra, RangeDecoder& dec, int channels, int lm)
{
    unsigned char const* model = energy_model[lm][intra ? 1 : 0];
    float prev[2] = { 0, 0 };
    float const coef = intra ? 0 : prediction_coefficient[lm];
    float const beta = intra ? beta_intra : beta_coefficient[lm];
    std::int32_t const budget = static_cast<std::int32_t>(dec.storage()) * 8;
    for (int i = start; i < end; ++i) {
        for (int c = 0; c < channels; ++c) {
            int qi;
            std::int32_t const tell = dec.tell();
            if (budget - tell >= 15) {
                int const pi = 2 * std::min(i, 20);
                qi = laplace_decode(dec, static_cast<unsigned>(model[pi]) << 7, model[pi + 1] << 6);
            } else if (budget - tell >= 2) {
                qi = dec.icdf(small_energy_icdf, 2);
                qi = (qi >> 1) ^ -(qi & 1);
            } else if (budget - tell >= 1) {
                qi = -static_cast<int>(dec.bit_logp(1));
            } else {
                qi = -1;
            }
            float const q = static_cast<float>(qi);
            float& e = old_energy[i + c * band_count];
            e = std::max(-9.0f, e);
            float const tmp = coef * e + prev[c] + q;
            e = tmp;
            prev[c] = prev[c] + q - beta * q;
        }
    }
}

void decode_fine_energy(int start, int end, float* old_energy, int const* fine_quant, RangeDecoder& dec, int channels)
{
    for (int i = start; i < end; ++i) {
        if (fine_quant[i] <= 0)
            continue;
        for (int c = 0; c < channels; ++c) {
            auto const q2 = static_cast<float>(dec.bits(static_cast<unsigned>(fine_quant[i])));
            float const offset = (q2 + .5f) * static_cast<float>(1 << (14 - fine_quant[i])) * (1.f / 16384) - .5f;
            old_energy[i + c * band_count] += offset;
        }
    }
}

void finish_energy(int start, int end, float* old_energy, int const* fine_quant, int const* fine_priority, int bits_left,
    RangeDecoder& dec, int channels)
{
    for (int priority = 0; priority < 2; ++priority) {
        for (int i = start; i < end && bits_left >= channels; ++i) {
            if (fine_quant[i] >= max_fine_bits || fine_priority[i] != priority)
                continue;
            for (int c = 0; c < channels; ++c) {
                auto const q2 = static_cast<float>(dec.bits(1));
                float const offset = (q2 - .5f) * static_cast<float>(1 << (14 - fine_quant[i] - 1)) * (1.f / 16384);
                old_energy[i + c * band_count] += offset;
                bits_left--;
            }
        }
    }
}

void decode_tf(int start, int end, bool transient, int* tf_res, int lm, RangeDecoder& dec)
{
    std::uint32_t budget = dec.storage() * 8;
    auto tell = static_cast<std::uint32_t>(dec.tell());
    int logp = transient ? 2 : 4;
    bool const tf_select_rsv = lm > 0 && tell + static_cast<std::uint32_t>(logp) + 1 <= budget;
    budget -= tf_select_rsv ? 1 : 0;
    int tf_changed = 0;
    int curr = 0;
    for (int i = start; i < end; ++i) {
        if (tell + static_cast<std::uint32_t>(logp) <= budget) {
            curr ^= static_cast<int>(dec.bit_logp(static_cast<unsigned>(logp)));
            tell = static_cast<std::uint32_t>(dec.tell());
            tf_changed |= curr;
        }
        tf_res[i] = curr;
        logp = transient ? 4 : 5;
    }
    int tf_select = 0;
    int const t = transient ? 4 : 0;
    if (tf_select_rsv && tf_select_table[lm][t + 0 + tf_changed] != tf_select_table[lm][t + 2 + tf_changed])
        tf_select = static_cast<int>(dec.bit_logp(1));
    for (int i = start; i < end; ++i)
        tf_res[i] = tf_select_table[lm][t + 2 * tf_select + tf_res[i]];
}

// --- Time domain ------------------------------------------------------------------------------------------

// An inverse FFT of the sizes the MDCT needs (480, 240, 120, 60 points):
// mixed radix, recursive, unscaled.
class InverseFft {
public:
    explicit InverseFft(int n)
        : m_n(n)
        , m_twiddles(static_cast<std::size_t>(n))
    {
        double const pi = 3.141592653589793;
        for (int k = 0; k < n; ++k)
            m_twiddles[static_cast<std::size_t>(k)] = std::complex<float>(static_cast<float>(std::cos(2 * pi * k / n)), static_cast<float>(std::sin(2 * pi * k / n)));
        int remaining = n;
        for (int p : { 4, 2, 3, 5 }) {
            while (remaining % p == 0) {
                remaining /= p;
                m_factors.push_back(p);
                m_factors.push_back(remaining);
            }
        }
    }

    void run(std::complex<float> const* in, std::complex<float>* out) const { work(out, in, 1, m_factors.data()); }

private:
    void work(std::complex<float>* out, std::complex<float> const* in, int fstride, int const* factors) const
    {
        int const p = factors[0];
        int const m = factors[1];
        std::complex<float>* const begin = out;
        std::complex<float>* const end = out + p * m;
        if (m == 1) {
            do {
                *out = *in;
                in += fstride;
            } while (++out != end);
        } else {
            do {
                work(out, in, fstride * p, factors + 2);
                in += fstride;
            } while ((out += m) != end);
        }
        butterfly(begin, fstride, m, p);
    }

    void butterfly(std::complex<float>* out, int fstride, int m, int p) const
    {
        std::complex<float> scratch[5];
        for (int u = 0; u < m; ++u) {
            int k = u;
            for (int q = 0; q < p; ++q, k += m)
                scratch[q] = out[k];
            k = u;
            for (int q1 = 0; q1 < p; ++q1, k += m) {
                int twiddle = 0;
                std::complex<float> sum = scratch[0];
                for (int q = 1; q < p; ++q) {
                    twiddle += fstride * k;
                    if (twiddle >= m_n)
                        twiddle -= m_n;
                    sum += scratch[q] * m_twiddles[static_cast<std::size_t>(twiddle)];
                }
                out[k] = sum;
            }
        }
    }

    int m_n;
    std::vector<std::complex<float>> m_twiddles;
    std::vector<int> m_factors;
};

InverseFft const& fft_for(int shift)
{
    static InverseFft const ffts[4] = { InverseFft(480), InverseFft(240), InverseFft(120), InverseFft(60) };
    return ffts[shift];
}

float const* mdct_trig()
{
    // cos(2 pi i / 1920) for i up to a quarter: the largest transform's
    // twiddles, which the smaller ones read with a stride.
    static std::array<float, 481> const table = [] {
        std::array<float, 481> t {};
        double const pi = 3.141592653589793;
        for (int i = 0; i <= 480; ++i)
            t[static_cast<std::size_t>(i)] = static_cast<float>(std::cos(2 * pi * i / 1920));
        return t;
    }();
    return table.data();
}

// The inverse MDCT of one block (§4.3.7): N/2 coefficients read with a
// stride become N windowed samples, the first and last `overlap` of them
// added to what overlaps (out is the start of this block's output, which
// reaches (N/2 - overlap)/2 before it).
void inverse_mdct(float const* in, int stride, float* x, int out_at, int shift)
{
    int const n = 1920 >> shift;
    int const n2 = n >> 1;
    int const n4 = n >> 2;
    float const* t = mdct_trig();
    float const sine = 2 * 3.141592653f * (.125f) / static_cast<float>(n);
    std::vector<std::complex<float>> f2(static_cast<std::size_t>(n4));
    std::vector<std::complex<float>> f(static_cast<std::size_t>(n4));
    {
        float const* xp1 = in;
        float const* xp2 = in + stride * (n2 - 1);
        for (int i = 0; i < n4; ++i) {
            float const yr = -*xp2 * t[i << shift] + *xp1 * t[(n4 - i) << shift];
            float const yi = -*xp2 * t[(n4 - i) << shift] - *xp1 * t[i << shift];
            f2[static_cast<std::size_t>(i)] = { yr - yi * sine, yi + yr * sine };
            xp1 += 2 * stride;
            xp2 -= 2 * stride;
        }
    }
    fft_for(shift).run(f2.data(), f.data());
    std::vector<float> rotated(static_cast<std::size_t>(n2));
    for (int i = 0; i < n4; ++i) {
        float const re = f[static_cast<std::size_t>(i)].real();
        float const im = f[static_cast<std::size_t>(i)].imag();
        float const yr = re * t[i << shift] - im * t[(n4 - i) << shift];
        float const yi = im * t[i << shift] + re * t[(n4 - i) << shift];
        rotated[static_cast<std::size_t>(2 * i)] = yr - yi * sine;
        rotated[static_cast<std::size_t>(2 * i + 1)] = yi + yr * sine;
    }
    // De-shuffled for the middle of the window.
    std::vector<float> shuffled(static_cast<std::size_t>(n2));
    for (int i = 0; i < n4; ++i) {
        shuffled[static_cast<std::size_t>(2 * i)] = -rotated[static_cast<std::size_t>(2 * i)];
        shuffled[static_cast<std::size_t>(2 * i + 1)] = rotated[static_cast<std::size_t>(n2 - 1 - 2 * i)];
    }
    auto const& w = window();
    int const out = out_at - ((n2 - overlap) >> 1);
    // Mirrored on both sides for the aliasing to cancel.
    {
        int fp1 = n4 - 1;
        int xp1 = out + n2 - 1;
        int yp1 = out + n4 - overlap / 2;
        int wp1 = 0;
        int wp2 = overlap - 1;
        int i = 0;
        for (; i < n4 - overlap / 2; ++i)
            x[xp1--] = shuffled[static_cast<std::size_t>(fp1--)];
        for (; i < n4; ++i) {
            float const x1 = shuffled[static_cast<std::size_t>(fp1--)];
            x[yp1++] += -w[static_cast<std::size_t>(wp1)] * x1;
            x[xp1--] += w[static_cast<std::size_t>(wp2)] * x1;
            wp1++;
            wp2--;
        }
    }
    {
        int fp2 = n4;
        int xp2 = out + n2;
        int yp2 = out + n - 1 - (n4 - overlap / 2);
        int wp1 = 0;
        int wp2 = overlap - 1;
        int i = 0;
        for (; i < n4 - overlap / 2; ++i)
            x[xp2++] = shuffled[static_cast<std::size_t>(fp2++)];
        for (; i < n4; ++i) {
            float const x2 = shuffled[static_cast<std::size_t>(fp2++)];
            x[yp2--] = w[static_cast<std::size_t>(wp1)] * x2;
            x[xp2++] = w[static_cast<std::size_t>(wp2)] * x2;
            wp1++;
            wp2--;
        }
    }
}

// The pitch post-filter (§4.3.7.1): a comb filter run in place over the
// output, cross-faded from the last frame's pitch and gain to this one's
// over the overlap.
void comb_filter(float* y, int t0, int t1, int n, float g0, float g1, int tapset0, int tapset1)
{
    static constexpr float gains[3][3] = { { 0.3066406250f, 0.2170410156f, 0.1296386719f }, { 0.4638671875f, 0.2680664062f, 0.f },
        { 0.7998046875f, 0.1000976562f, 0.f } };
    if (g0 == 0 && g1 == 0)
        return;
    float const g00 = g0 * gains[tapset0][0];
    float const g01 = g0 * gains[tapset0][1];
    float const g02 = g0 * gains[tapset0][2];
    float const g10 = g1 * gains[tapset1][0];
    float const g11 = g1 * gains[tapset1][1];
    float const g12 = g1 * gains[tapset1][2];
    auto const& w = window();
    int i = 0;
    for (; i < overlap && i < n; ++i) {
        float const f = w[static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)];
        y[i] = y[i] + ((1.0f - f) * g00) * y[i - t0] + ((1.0f - f) * g01) * y[i - t0 - 1] + ((1.0f - f) * g01) * y[i - t0 + 1]
            + ((1.0f - f) * g02) * y[i - t0 - 2] + ((1.0f - f) * g02) * y[i - t0 + 2] + (f * g10) * y[i - t1]
            + (f * g11) * y[i - t1 - 1] + (f * g11) * y[i - t1 + 1] + (f * g12) * y[i - t1 - 2] + (f * g12) * y[i - t1 + 2];
    }
    for (; i < n; ++i)
        y[i] = y[i] + g10 * y[i - t1] + g11 * y[i - t1 - 1] + g11 * y[i - t1 + 1] + g12 * y[i - t1 - 2] + g12 * y[i - t1 + 2];
}

}

CeltDecoder::CeltDecoder(int channels)
    : m_channels(channels)
    , m_decode_memory(static_cast<std::size_t>(channels * (decode_buffer_size + overlap)))
{
    reset();
}

void CeltDecoder::reset()
{
    std::fill(m_decode_memory.begin(), m_decode_memory.end(), 0.0f);
    m_old_band_energy.fill(0);
    m_old_log_energy.fill(-28.0f);
    m_old_log_energy2.fill(-28.0f);
    m_background_log_energy.fill(0);
    m_preemphasis_memory.fill(0);
    m_rng = 0;
    m_postfilter_period = m_postfilter_period_old = 0;
    m_postfilter_gain = m_postfilter_gain_old = 0;
    m_postfilter_tapset = m_postfilter_tapset_old = 0;
    m_loss_count = 0;
}

bool CeltDecoder::decode(RangeDecoder& dec, int bytes, float* pcm, int frame_size, int coded_channels, int start, int end)
{
    int lm = 0;
    while (lm <= max_lm && short_mdct_size << lm != frame_size)
        lm++;
    if (lm > max_lm || bytes < 0 || bytes > 1275 || coded_channels < 1 || coded_channels > 2)
        return false;
    int const m = 1 << lm;
    int const n = m * short_mdct_size;
    int const channels = coded_channels;
    int const eff_end = std::min(end, band_count);
    if (bytes <= 1) {
        conceal(pcm, frame_size);
        return true;
    }

    std::vector<float> x(static_cast<std::size_t>(channels * n), 0.0f);
    if (channels == 1) {
        for (int i = 0; i < band_count; ++i)
            m_old_band_energy[static_cast<std::size_t>(i)] = std::max(m_old_band_energy[static_cast<std::size_t>(i)], m_old_band_energy[static_cast<std::size_t>(band_count + i)]);
    }

    std::int32_t total_bits = bytes * 8;
    std::int32_t tell = dec.tell();
    bool silence;
    if (tell >= total_bits)
        silence = true;
    else if (tell == 1)
        silence = dec.bit_logp(15);
    else
        silence = false;
    if (silence) {
        // Everything after the flag is silence: count it all as read.
        tell = bytes * 8;
        dec.skip_to(tell);
    }

    float postfilter_gain = 0;
    int postfilter_pitch = 0;
    int postfilter_tapset = 0;
    if (start == 0 && tell + 16 <= total_bits) {
        if (dec.bit_logp(1)) {
            int const octave = static_cast<int>(dec.uint(6));
            postfilter_pitch = (16 << octave) + static_cast<int>(dec.bits(static_cast<unsigned>(4 + octave))) - 1;
            int const qg = static_cast<int>(dec.bits(3));
            if (dec.tell() + 2 <= total_bits)
                postfilter_tapset = dec.icdf(tapset_icdf, 2);
            postfilter_gain = .09375f * static_cast<float>(qg + 1);
        }
        tell = dec.tell();
    }

    bool transient = false;
    if (lm > 0 && tell + 3 <= total_bits) {
        transient = dec.bit_logp(3);
        tell = dec.tell();
    }
    int const short_blocks = transient ? m : 0;

    bool const intra = tell + 3 <= total_bits ? dec.bit_logp(3) : false;
    decode_coarse_energy(start, end, m_old_band_energy.data(), intra, dec, channels, lm);

    std::array<int, band_count> tf_res {};
    decode_tf(start, end, transient, tf_res.data(), lm, dec);

    tell = dec.tell();
    int spread = spread_normal;
    if (tell + 4 <= total_bits)
        spread = dec.icdf(spread_icdf, 5);

    std::array<int, band_count> cap {};
    for (int i = 0; i < band_count; ++i)
        cap[static_cast<std::size_t>(i)] = (cache_caps[band_count * (2 * lm + channels - 1) + i] + 64) * channels * (band_width(i) << lm) >> 2;

    // Bits a band asks for beyond its share (dynamic allocation).
    std::array<int, band_count> offsets {};
    int dynalloc_logp = 6;
    total_bits <<= bit_resolution;
    tell = static_cast<std::int32_t>(dec.tell_frac());
    for (int i = start; i < end; ++i) {
        int const width = channels * band_width(i) << lm;
        int const quanta = std::min(width << bit_resolution, std::max(6 << bit_resolution, width));
        int loop_logp = dynalloc_logp;
        int boost = 0;
        while (tell + (loop_logp << bit_resolution) < total_bits && boost < cap[static_cast<std::size_t>(i)]) {
            bool const flag = dec.bit_logp(static_cast<unsigned>(loop_logp));
            tell = static_cast<std::int32_t>(dec.tell_frac());
            if (!flag)
                break;
            boost += quanta;
            total_bits -= quanta;
            loop_logp = 1;
        }
        offsets[static_cast<std::size_t>(i)] = boost;
        if (boost > 0)
            dynalloc_logp = std::max(2, dynalloc_logp - 1);
    }

    int const alloc_trim = tell + (6 << bit_resolution) <= total_bits ? dec.icdf(trim_icdf, 7) : 5;

    std::int32_t bits = ((bytes * 8) << bit_resolution) - static_cast<std::int32_t>(dec.tell_frac()) - 1;
    int const anti_collapse_rsv = transient && lm >= 2 && bits >= ((lm + 2) << bit_resolution) ? (1 << bit_resolution) : 0;
    bits -= anti_collapse_rsv;
    std::array<int, band_count> pulses {};
    std::array<int, band_count> fine_quant {};
    std::array<int, band_count> fine_priority {};
    int intensity = 0;
    int dual_stereo = 0;
    std::int32_t balance = 0;
    int const coded_bands = compute_allocation(start, end, offsets.data(), cap.data(), alloc_trim, &intensity, &dual_stereo, bits,
        &balance, pulses.data(), fine_quant.data(), fine_priority.data(), channels, lm, dec);

    decode_fine_energy(start, end, m_old_band_energy.data(), fine_quant.data(), dec, channels);

    std::array<unsigned char, 2 * band_count> collapse_masks {};
    decode_all_bands(start, end, x.data(), channels == 2 ? x.data() + n : nullptr, collapse_masks.data(), pulses.data(),
        short_blocks, spread, dual_stereo, intensity, tf_res.data(), bytes * (8 << bit_resolution) - anti_collapse_rsv, balance, dec, lm,
        coded_bands, &m_rng);

    bool anti_collapse_on = false;
    if (anti_collapse_rsv > 0)
        anti_collapse_on = dec.bits(1) != 0;

    finish_energy(start, end, m_old_band_energy.data(), fine_quant.data(), fine_priority.data(), bytes * 8 - dec.tell(), dec, channels);

    if (anti_collapse_on) {
        // Short blocks that a transient left empty get noise at the level
        // of the band's recent energy, so the band does not collapse.
        std::uint32_t seed = m_rng;
        for (int i = start; i < end; ++i) {
            int const n0 = band_width(i);
            int const depth = (1 + pulses[static_cast<std::size_t>(i)]) / (n0 << lm);
            float const thresh = .5f * static_cast<float>(std::exp(0.6931471805599453094 * static_cast<double>(-.125f * static_cast<float>(depth))));
            float const sqrt_1 = 1.0f / std::sqrt(static_cast<float>(n0 << lm));
            for (int c = 0; c < channels; ++c) {
                float prev1 = m_old_log_energy[static_cast<std::size_t>(c * band_count + i)];
                float prev2 = m_old_log_energy2[static_cast<std::size_t>(c * band_count + i)];
                if (channels == 1) {
                    prev1 = std::max(prev1, m_old_log_energy[static_cast<std::size_t>(band_count + i)]);
                    prev2 = std::max(prev2, m_old_log_energy2[static_cast<std::size_t>(band_count + i)]);
                }
                float ediff = m_old_band_energy[static_cast<std::size_t>(c * band_count + i)] - std::min(prev1, prev2);
                ediff = std::max(0.0f, ediff);
                float r = 2.f * static_cast<float>(std::exp(0.6931471805599453094 * static_cast<double>(-ediff)));
                if (lm == 3)
                    r *= 1.41421356f;
                r = std::min(thresh, r);
                r = r * sqrt_1;
                float* band = x.data() + c * n + (band_edges[static_cast<std::size_t>(i)] << lm);
                bool renormalize = false;
                for (int k = 0; k < 1 << lm; ++k) {
                    if (!(collapse_masks[static_cast<std::size_t>(i * channels + c)] & 1 << k)) {
                        for (int j = 0; j < n0; ++j) {
                            seed = lcg_rand(seed);
                            band[(j << lm) + k] = (seed & 0x8000) ? r : -r;
                        }
                        renormalize = true;
                    }
                }
                if (renormalize)
                    renormalise(band, n0 << lm, 1.0f);
            }
        }
    }

    // The energies to linear amplitudes (RFC 8251 §8 caps them first).
    std::array<float, 2 * band_count> band_energy {};
    for (int c = 0; c < channels; ++c) {
        for (int i = start; i < end; ++i) {
            float const lg = std::min(32.0f, m_old_band_energy[static_cast<std::size_t>(i + c * band_count)] + energy_means[i]);
            band_energy[static_cast<std::size_t>(i + c * band_count)] = static_cast<float>(std::exp(0.6931471805599453094 * static_cast<double>(lg)));
        }
    }
    if (silence) {
        band_energy.fill(0);
        for (int i = 0; i < channels * band_count; ++i)
            m_old_band_energy[static_cast<std::size_t>(i)] = -28.0f;
    }

    // Each band's shape times its amplitude: the spectrum.
    std::vector<float> freq(static_cast<std::size_t>(std::max(m_channels, channels) * n), 0.0f);
    for (int c = 0; c < channels; ++c) {
        float* f = freq.data() + c * n;
        float const* shape = x.data() + c * n;
        for (int i = 0; i < eff_end; ++i) {
            float const g = band_energy[static_cast<std::size_t>(i + c * band_count)];
            for (int j = m * band_edges[static_cast<std::size_t>(i)]; j < m * band_edges[static_cast<std::size_t>(i) + 1]; ++j)
                f[j] = shape[j] * g;
        }
        for (int j = 0; j < m * band_edges[static_cast<std::size_t>(start)]; ++j)
            f[j] = 0;
        for (int j = m * band_edges[static_cast<std::size_t>(eff_end)]; j < n; ++j)
            f[j] = 0;
    }

    synthesize(freq.data(), pcm, n, lm, short_blocks, channels, postfilter_pitch, postfilter_gain, postfilter_tapset);

    if (channels == 1) {
        for (int i = 0; i < band_count; ++i)
            m_old_band_energy[static_cast<std::size_t>(band_count + i)] = m_old_band_energy[static_cast<std::size_t>(i)];
    }
    if (!transient) {
        m_old_log_energy2 = m_old_log_energy;
        m_old_log_energy = m_old_band_energy;
        for (int i = 0; i < 2 * band_count; ++i)
            m_background_log_energy[static_cast<std::size_t>(i)] = std::min(m_background_log_energy[static_cast<std::size_t>(i)] + static_cast<float>(m) * 0.001f, m_old_band_energy[static_cast<std::size_t>(i)]);
    } else {
        for (int i = 0; i < 2 * band_count; ++i)
            m_old_log_energy[static_cast<std::size_t>(i)] = std::min(m_old_log_energy[static_cast<std::size_t>(i)], m_old_band_energy[static_cast<std::size_t>(i)]);
    }
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < start; ++i) {
            m_old_band_energy[static_cast<std::size_t>(c * band_count + i)] = 0;
            m_old_log_energy[static_cast<std::size_t>(c * band_count + i)] = m_old_log_energy2[static_cast<std::size_t>(c * band_count + i)] = -28.0f;
        }
        for (int i = end; i < band_count; ++i) {
            m_old_band_energy[static_cast<std::size_t>(c * band_count + i)] = 0;
            m_old_log_energy[static_cast<std::size_t>(c * band_count + i)] = m_old_log_energy2[static_cast<std::size_t>(c * band_count + i)] = -28.0f;
        }
    }
    m_rng = dec.range();
    m_loss_count = 0;
    return dec.tell() <= 8 * bytes;
}

void CeltDecoder::synthesize(float* freq, float* pcm, int n, int lm, int short_blocks, int coded_channels, int postfilter_pitch,
    float postfilter_gain, int postfilter_tapset)
{
    int const channels = m_channels;
    int const stride = decode_buffer_size + overlap;
    // The history moves down a frame; this frame's output goes at its end.
    for (int c = 0; c < channels; ++c) {
        float* memory = m_decode_memory.data() + c * stride;
        std::memmove(memory, memory + n, static_cast<std::size_t>(decode_buffer_size - n) * sizeof(float));
    }
    if (channels == 2 && coded_channels == 1)
        std::copy(freq, freq + n, freq + n);
    if (channels == 1 && coded_channels == 2) {
        for (int i = 0; i < n; ++i)
            freq[i] = .5f * (freq[i] + freq[n + i]);
    }

    int const n2 = short_blocks ? short_mdct_size : n;
    int const blocks = short_blocks ? short_blocks : 1;
    int const shift = short_blocks ? max_lm : max_lm - lm;
    std::vector<float> x(static_cast<std::size_t>(n + overlap));
    for (int c = 0; c < channels; ++c) {
        float* memory = m_decode_memory.data() + c * stride;
        float* out = memory + decode_buffer_size - n;
        float* overlap_memory = memory + decode_buffer_size;
        std::fill(x.begin(), x.end(), 0.0f);
        for (int b = 0; b < blocks; ++b)
            inverse_mdct(freq + c * n + b, blocks, x.data(), n2 * b, shift);
        for (int j = 0; j < overlap; ++j)
            out[j] = x[static_cast<std::size_t>(j)] + overlap_memory[j];
        for (int j = overlap; j < n; ++j)
            out[j] = x[static_cast<std::size_t>(j)];
        for (int j = 0; j < overlap; ++j)
            overlap_memory[j] = x[static_cast<std::size_t>(n + j)];
    }

    for (int c = 0; c < channels; ++c) {
        float* out = m_decode_memory.data() + c * stride + decode_buffer_size - n;
        m_postfilter_period = std::max(m_postfilter_period, combfilter_min_period);
        m_postfilter_period_old = std::max(m_postfilter_period_old, combfilter_min_period);
        comb_filter(out, m_postfilter_period_old, m_postfilter_period, short_mdct_size, m_postfilter_gain_old, m_postfilter_gain,
            m_postfilter_tapset_old, m_postfilter_tapset);
        if (lm != 0)
            comb_filter(out + short_mdct_size, m_postfilter_period, postfilter_pitch, n - short_mdct_size, m_postfilter_gain,
                postfilter_gain, m_postfilter_tapset, postfilter_tapset);
    }
    m_postfilter_period_old = m_postfilter_period;
    m_postfilter_gain_old = m_postfilter_gain;
    m_postfilter_tapset_old = m_postfilter_tapset;
    m_postfilter_period = postfilter_pitch;
    m_postfilter_gain = postfilter_gain;
    m_postfilter_tapset = postfilter_tapset;
    if (lm != 0) {
        m_postfilter_period_old = m_postfilter_period;
        m_postfilter_gain_old = m_postfilter_gain;
        m_postfilter_tapset_old = m_postfilter_tapset;
    }

    // De-emphasis (§4.3.7.2), and into the interleaved output.
    for (int c = 0; c < channels; ++c) {
        float const* in = m_decode_memory.data() + c * stride + decode_buffer_size - n;
        float memory = m_preemphasis_memory[static_cast<std::size_t>(c)];
        for (int j = 0; j < n; ++j) {
            float const tmp = in[j] + 1e-30f + memory;
            memory = preemphasis * tmp;
            pcm[j * channels + c] = tmp * (1.0f / 32768.0f);
        }
        m_preemphasis_memory[static_cast<std::size_t>(c)] = memory;
    }
}

void CeltDecoder::conceal(float* pcm, int frame_size)
{
    // What the last frame's transform leaves in the overlap, with nothing
    // new added: a fade to silence rather than a click.
    int lm = 0;
    while (lm <= max_lm && short_mdct_size << lm != frame_size)
        lm++;
    if (lm > max_lm) {
        std::fill(pcm, pcm + frame_size * m_channels, 0.0f);
        return;
    }
    int const n = short_mdct_size << lm;
    std::vector<float> freq(static_cast<std::size_t>(m_channels * n), 0.0f);
    synthesize(freq.data(), pcm, n, lm, 0, m_channels, m_postfilter_period, m_postfilter_gain, m_postfilter_tapset);
    m_loss_count++;
}

}
