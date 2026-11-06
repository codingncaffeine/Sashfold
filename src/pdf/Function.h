#pragma once

// PDF functions (ISO 32000-2 §7.10): sampled (type 0), exponential
// interpolation (type 2), stitching (type 3) and PostScript calculator
// (type 4) — what tint transforms, shadings, transfer functions and soft
// mask backdrops are written in. A function is parsed once and evaluated
// from any thread; malformed ones parse to nothing and their callers fall
// back as viewers do.

#include "pdf/Object.h"

#include <array>
#include <memory>
#include <vector>

namespace sashfold::pdf {

class Document;

class Function {
public:
    virtual ~Function() = default;
    Function() = default;
    Function(Function const&) = delete;
    Function& operator=(Function const&) = delete;

    // Writes outputs() values from the inputs; inputs beyond the domain are clipped to it.
    virtual void evaluate(float const* in, float* out) const = 0;
    int inputs() const { return static_cast<int>(m_domain.size() / 2); }
    int outputs() const { return m_outputs; }

    // From a function object (a dictionary or a stream), or an array of
    // one-output functions taken together; null when it cannot be read.
    static std::shared_ptr<Function const> parse(Document const& doc, Object const& object, int depth = 0);

protected:
    float clip_input(float value, int index) const;
    float clip_output(float value, int index) const;

    std::vector<float> m_domain; // pairs
    std::vector<float> m_range; // pairs; may be empty for types 2 and 3
    int m_outputs = 0;

    friend class FunctionParser;
};

// A one-input function sampled ahead at many points, for the inner loops
// of shadings: evaluate(t) by linear interpolation between samples.
class SampledFunction1D {
public:
    SampledFunction1D() = default;
    SampledFunction1D(Function const& function, float t0, float t1, int samples = 1024);
    // Into `out` (outputs() values) for t, clipped to [t0, t1].
    void evaluate(float t, float* out) const;
    int outputs() const { return m_outputs; }
    bool valid() const { return m_outputs > 0; }

private:
    float m_t0 = 0;
    float m_t1 = 1;
    int m_samples = 0;
    int m_outputs = 0;
    std::vector<float> m_values;
};

}
