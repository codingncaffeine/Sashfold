#pragma once

// Sound from one sampling rate to another, as a player must when the
// stream's rate is not the speakers' (AAC at 44.1 kHz through a device at
// 48 kHz): a windowed-sinc interpolation, the sinc oversampled into a table
// and read between its points, which keeps the band below the lower
// Nyquist rate clean to better than 90 dB — what every shipping player's
// resampler does, at the quality a listener cannot tell from the source.
// Interleaved channels; the output's frame count follows the input's by
// the ratio, with the filter's delay carried from call to call.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sashfold::media {

class Resampler {
public:
    Resampler(int channels, int rate_in, int rate_out);

    int channels() const { return m_channels; }
    bool identity() const { return m_rate_in == m_rate_out; }

    // Appends the output frames the input frames give to `out`; returns how many.
    std::size_t resample(std::span<float const> in, std::vector<float>& out);
    // Forgets the history, as at a seek.
    void reset();
    // The output frames `frames` input frames become, on the long run.
    double ratio() const { return static_cast<double>(m_rate_out) / m_rate_in; }

private:
    int m_channels;
    int m_rate_in;
    int m_rate_out;
    std::vector<float> m_history; // the last frames seen, half a filter's worth and more
    double m_phase = 0; // the next output's position within the input, in input frames
    double m_cutoff = 1; // of the lower Nyquist rate
};

}
