#include "media/Resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sashfold::media {

namespace {

// The filter: a sinc of `taps` zero crossings each side, under a Kaiser
// window (beta 9: about 90 dB of stop-band), sampled `oversample` times a
// crossing. One table for every rate: the cutoff scales its reading.
constexpr int taps = 24;
constexpr int oversample = 128;

std::vector<float> const& sinc_table()
{
    static std::vector<float> const table = [] {
        std::vector<float> t(static_cast<std::size_t>(taps) * oversample + 2);
        double const beta = 9.0;
        auto const bessel_i0 = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 60; ++k) {
                term *= (x / (2 * k)) * (x / (2 * k));
                sum += term;
                if (term < sum * 1e-18)
                    break;
            }
            return sum;
        };
        double const norm = bessel_i0(beta);
        for (std::size_t i = 0; i < t.size(); ++i) {
            double const x = static_cast<double>(i) / oversample; // in zero crossings
            double const r = x / taps;
            double const window = r < 1 ? bessel_i0(beta * std::sqrt(1 - r * r)) / norm : 0;
            double const sinc = x == 0 ? 1 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            t[i] = static_cast<float>(sinc * window);
        }
        return t;
    }();
    return table;
}

} // namespace

Resampler::Resampler(int channels, int rate_in, int rate_out)
    : m_channels(std::max(1, channels))
    , m_rate_in(std::max(1, rate_in))
    , m_rate_out(std::max(1, rate_out))
{
    // Going down, the band is cut at the output's Nyquist rate.
    m_cutoff = m_rate_out < m_rate_in ? static_cast<double>(m_rate_out) / m_rate_in : 1.0;
    reset();
}

void Resampler::reset()
{
    // Silence before the first sample, a filter's reach of it; the first
    // output sits at the first input sample.
    m_history.assign(static_cast<std::size_t>(taps * 2 + 2) * static_cast<std::size_t>(m_channels), 0.0f);
    m_phase = 0;
}

std::size_t Resampler::resample(std::span<float const> in, std::vector<float>& out)
{
    std::size_t const channels = static_cast<std::size_t>(m_channels);
    std::size_t const in_frames = in.size() / channels;
    if (identity()) {
        out.insert(out.end(), in.begin(), in.begin() + static_cast<std::ptrdiff_t>(in_frames * channels));
        return in_frames;
    }
    // The signal: the history (the frames before `in`) then `in`. The
    // phase is the next output's position in input frames from `in`'s
    // start; an output needs the signal within `reach` either side.
    std::vector<float> signal;
    signal.reserve(m_history.size() + in.size());
    signal.insert(signal.end(), m_history.begin(), m_history.end());
    signal.insert(signal.end(), in.begin(), in.begin() + static_cast<std::ptrdiff_t>(in_frames * channels));
    long const history_frames = static_cast<long>(m_history.size() / channels);
    long const total_frames = history_frames + static_cast<long>(in_frames);
    double const step = static_cast<double>(m_rate_in) / m_rate_out;
    double const reach = taps / m_cutoff;
    std::vector<float> const& table = sinc_table();
    std::size_t made = 0;
    while (m_phase + reach < static_cast<double>(in_frames)) {
        double const center = m_phase + static_cast<double>(history_frames);
        long const first = std::max(0L, static_cast<long>(std::ceil(center - reach)));
        long const last = std::min(total_frames - 1, static_cast<long>(std::floor(center + reach)));
        for (std::size_t c = 0; c < channels; ++c) {
            double sum = 0;
            double weight = 0;
            for (long i = first; i <= last; ++i) {
                double const position = std::fabs(static_cast<double>(i) - center) * m_cutoff * oversample;
                auto const index = static_cast<std::size_t>(position);
                if (index + 1 >= table.size())
                    continue;
                double const fraction = position - static_cast<double>(index);
                double const w = static_cast<double>(table[index]) + (static_cast<double>(table[index + 1]) - static_cast<double>(table[index])) * fraction;
                sum += w * static_cast<double>(signal[static_cast<std::size_t>(i) * channels + c]);
                weight += w;
            }
            // The weights' sum keeps the gain at one whatever the phase.
            out.push_back(static_cast<float>(weight > 1e-9 ? sum / weight : 0.0));
        }
        ++made;
        m_phase += step;
    }
    // Kept for the next call: from a reach before the next output back.
    long const keep_from = std::max(0L, static_cast<long>(std::floor(m_phase + static_cast<double>(history_frames) - reach)) - 1);
    m_history.assign(signal.begin() + keep_from * static_cast<long>(channels), signal.end());
    m_phase -= static_cast<double>(in_frames); // relative to the next input's start
    return made;
}

}
