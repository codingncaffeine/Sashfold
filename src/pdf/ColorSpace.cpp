#include "pdf/ColorSpace.h"

#include "pdf/Document.h"

#include <algorithm>
#include <cmath>

namespace sashfold::pdf {

namespace {

constexpr int k_max_depth = 8;

float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

class Gray final : public ColorSpace {
public:
    Gray()
    {
        m_family = Family::Gray;
        m_components = 1;
    }
    void to_rgb(float const* in, float* rgb) const override { rgb[0] = rgb[1] = rgb[2] = clamp01(in[0]); }
};

class Rgb final : public ColorSpace {
public:
    Rgb()
    {
        m_family = Family::RGB;
        m_components = 3;
    }
    void to_rgb(float const* in, float* rgb) const override
    {
        rgb[0] = clamp01(in[0]);
        rgb[1] = clamp01(in[1]);
        rgb[2] = clamp01(in[2]);
    }
};

class Cmyk final : public ColorSpace {
public:
    Cmyk()
    {
        m_family = Family::CMYK;
        m_components = 4;
    }
    void to_rgb(float const* in, float* rgb) const override { cmyk_to_rgb(in[0], in[1], in[2], in[3], rgb); }
    void initial_color(float* out) const override
    {
        out[0] = out[1] = out[2] = 0;
        out[3] = 1;
    }
};

class Lab final : public ColorSpace {
public:
    std::array<float, 3> white { 0.9505f, 1.0f, 1.089f };
    std::array<float, 4> range { -100, 100, -100, 100 };
    icc::Mat3 to_pcs = icc::bradford({ 0.9505, 1.0, 1.089 }, icc::k_d50);

    Lab()
    {
        m_family = Family::Lab;
        m_components = 3;
    }
    void set_white(std::array<float, 3> const& w)
    {
        white = w;
        to_pcs = icc::bradford({ static_cast<double>(w[0]), static_cast<double>(w[1]), static_cast<double>(w[2]) }, icc::k_d50);
    }
    void to_rgb(float const* in, float* rgb) const override
    {
        // §8.6.5.4: CIELAB relative to the space's white, which is adapted
        // to the D50 PCS by Bradford (as a profile's white is), then sRGB.
        double const l = static_cast<double>(std::clamp(in[0], 0.0f, 100.0f));
        double const a = static_cast<double>(std::clamp(in[1], range[0], range[1]));
        double const b = static_cast<double>(std::clamp(in[2], range[2], range[3]));
        icc::Vec3 const xyz = icc::lab_to_xyz({ l, a, b }, { static_cast<double>(white[0]), static_cast<double>(white[1]), static_cast<double>(white[2]) });
        icc::Vec3 const linear = icc::pcs_to_linear_srgb(icc::transform_by(to_pcs, xyz));
        for (std::size_t i = 0; i < 3; ++i)
            rgb[i] = static_cast<float>(icc::srgb_encode(linear[i]));
    }
    void initial_color(float* out) const override
    {
        out[0] = 0;
        out[1] = std::clamp(0.0f, range[0], range[1]);
        out[2] = std::clamp(0.0f, range[2], range[3]);
    }
    void default_decode(int, int index, float& low, float& high) const override
    {
        if (index == 0) {
            low = 0;
            high = 100;
        } else {
            low = range[static_cast<std::size_t>(index - 1) * 2];
            high = range[static_cast<std::size_t>(index - 1) * 2 + 1];
        }
    }
};

// ICCBased (§8.6.5.5): the profile by the intent, components kept to /Range.
class IccBased final : public ColorSpace {
public:
    std::shared_ptr<icc::Profile const> profile;
    std::array<float, 32> range_low {};
    std::array<float, 32> range_high {};

    IccBased(int n, bool lab)
    {
        m_family = lab ? Family::Lab : n == 1 ? Family::Gray : n == 3 ? Family::RGB : Family::CMYK;
        m_components = n;
        for (std::size_t i = 0; i < 32; ++i)
            range_high[i] = 1;
    }
    void to_rgb(float const* in, float* rgb) const override { to_rgb_by(in, rgb, RenderingIntent::RelativeColorimetric); }
    void to_rgb_by(float const* in, float* rgb, RenderingIntent intent) const override
    {
        std::array<float, 32> kept {};
        keep(in, kept.data());
        if (auto const t = profile->transform(intent)) {
            t->to_srgb(kept.data(), rgb);
            return;
        }
        rgb[0] = rgb[1] = rgb[2] = 0;
    }
    void to_rgb8(float const* in, std::size_t count, std::uint8_t* rgb, RenderingIntent intent) const override
    {
        auto const t = profile->transform(intent);
        if (!t) {
            ColorSpace::to_rgb8(in, count, rgb, intent);
            return;
        }
        // Kept to the range in chunks, then the whole run through the transform.
        std::size_t const n = static_cast<std::size_t>(m_components);
        std::array<float, 1024> kept;
        std::size_t const per = kept.size() / n;
        for (std::size_t start = 0; start < count; start += per) {
            std::size_t const run = std::min(per, count - start);
            for (std::size_t i = 0; i < run; ++i)
                keep(in + (start + i) * n, kept.data() + i * n);
            t->to_srgb8(kept.data(), run, rgb + start * 3);
        }
    }
    void initial_color(float* out) const override
    {
        // Every component 0, kept within the range (§8.6.5.5).
        for (std::size_t i = 0; i < static_cast<std::size_t>(m_components); ++i)
            out[i] = std::clamp(0.0f, range_low[i], range_high[i]);
    }
    void default_decode(int, int index, float& low, float& high) const override
    {
        // Table 90: an ICCBased image's default /Decode is its /Range.
        low = range_low[static_cast<std::size_t>(index)];
        high = range_high[static_cast<std::size_t>(index)];
    }

private:
    void keep(float const* in, float* out) const
    {
        for (std::size_t i = 0; i < static_cast<std::size_t>(m_components); ++i) {
            float const v = std::isfinite(in[i]) ? in[i] : 0.0f;
            out[i] = std::clamp(v, range_low[i], range_high[i]);
        }
    }
};

// CalGray and CalRGB (§8.6.5.2, §8.6.5.3): each a gamma per component and a
// matrix to XYZ under the space's white, which is adapted to D50.
class Calibrated final : public ColorSpace {
public:
    std::shared_ptr<icc::Transform const> transform;

    explicit Calibrated(int n)
    {
        m_family = n == 1 ? Family::Gray : Family::RGB;
        m_components = n;
    }
    void to_rgb(float const* in, float* rgb) const override { transform->to_srgb(in, rgb); }
    void to_rgb8(float const* in, std::size_t count, std::uint8_t* rgb, RenderingIntent) const override
    {
        transform->to_srgb8(in, count, rgb);
    }
};

class Indexed final : public ColorSpace {
public:
    std::shared_ptr<ColorSpace const> base;
    int hival = 0;
    std::vector<float> rgb_table; // (hival + 1) * 3, the lookup already converted by the default intent
    std::vector<float> base_values; // (hival + 1) * the base's components, for other intents

    Indexed()
    {
        m_family = Family::Indexed;
        m_components = 1;
    }
    void to_rgb(float const* in, float* rgb) const override
    {
        int const index = std::clamp(static_cast<int>(std::lround(in[0])), 0, hival);
        rgb[0] = rgb_table[static_cast<std::size_t>(index) * 3];
        rgb[1] = rgb_table[static_cast<std::size_t>(index) * 3 + 1];
        rgb[2] = rgb_table[static_cast<std::size_t>(index) * 3 + 2];
    }
    void to_rgb_by(float const* in, float* rgb, RenderingIntent intent) const override
    {
        if (intent == RenderingIntent::RelativeColorimetric) {
            to_rgb(in, rgb);
            return;
        }
        int const index = std::clamp(static_cast<int>(std::lround(in[0])), 0, hival);
        base->to_rgb_by(base_values.data() + static_cast<std::size_t>(index) * static_cast<std::size_t>(base->components()), rgb, intent);
    }
    void default_decode(int bits, int, float& low, float& high) const override
    {
        low = 0;
        high = static_cast<float>((1 << bits) - 1);
    }
    void initial_color(float* out) const override { out[0] = 0; }
};

// Separation and DeviceN: the tint through its transform into the alternate.
class Tinted final : public ColorSpace {
public:
    std::shared_ptr<ColorSpace const> alternate;
    std::shared_ptr<Function const> transform;
    bool all = false; // Separation /All: every colorant, so the tint is a gray's opposite

    void to_rgb(float const* in, float* rgb) const override { to_rgb_by(in, rgb, RenderingIntent::RelativeColorimetric); }
    void to_rgb_by(float const* in, float* rgb, RenderingIntent intent) const override
    {
        if (all) {
            float const v = 1 - clamp01(in[0]);
            rgb[0] = rgb[1] = rgb[2] = v;
            return;
        }
        std::array<float, 32> out {};
        if (transform) {
            transform->evaluate(in, out.data());
        }
        if (alternate && transform && transform->outputs() >= alternate->components()) {
            alternate->to_rgb_by(out.data(), rgb, intent);
            return;
        }
        // No usable transform: show the tint as a gray, as viewers do.
        float const v = 1 - clamp01(in[0]);
        rgb[0] = rgb[1] = rgb[2] = v;
    }
    void initial_color(float* out) const override
    {
        for (int i = 0; i < m_components; ++i)
            out[i] = 1;
    }
};

class PatternSpace final : public ColorSpace {
public:
    PatternSpace()
    {
        m_family = Family::Pattern;
        m_components = 0;
    }
    void to_rgb(float const*, float* rgb) const override { rgb[0] = rgb[1] = rgb[2] = 0; }
    void initial_color(float*) const override { }
};

} // namespace

RenderingIntent rendering_intent(std::string const& name)
{
    if (name == "Perceptual")
        return RenderingIntent::Perceptual;
    if (name == "Saturation")
        return RenderingIntent::Saturation;
    if (name == "AbsoluteColorimetric")
        return RenderingIntent::AbsoluteColorimetric;
    return RenderingIntent::RelativeColorimetric;
}

void ColorSpace::to_rgb_by(float const* in, float* rgb, RenderingIntent) const
{
    to_rgb(in, rgb);
}

void ColorSpace::to_rgb8(float const* in, std::size_t count, std::uint8_t* rgb, RenderingIntent intent) const
{
    // One color at a time, the last one kept: pictures repeat their colors.
    std::size_t const n = static_cast<std::size_t>(m_components);
    std::array<float, 32> last {};
    std::array<std::uint8_t, 3> last_rgb {};
    bool have_last = false;
    for (std::size_t i = 0; i < count; ++i) {
        float const* px = in + i * n;
        if (!have_last || !std::equal(px, px + n, last.begin())) {
            float out[3];
            to_rgb_by(px, out, intent);
            for (std::size_t k = 0; k < 3; ++k)
                last_rgb[k] = static_cast<std::uint8_t>(std::lround(clamp01(out[k]) * 255));
            std::copy(px, px + n, last.begin());
            have_last = true;
        }
        std::copy(last_rgb.begin(), last_rgb.end(), rgb + i * 3);
    }
}

void ColorSpace::initial_color(float* out) const
{
    for (int i = 0; i < m_components; ++i)
        out[i] = 0;
}

void ColorSpace::default_decode(int, int, float& low, float& high) const
{
    low = 0;
    high = 1;
}

std::shared_ptr<ColorSpace const> ColorSpace::device_gray()
{
    static auto const space = std::make_shared<Gray const>();
    return space;
}

std::shared_ptr<ColorSpace const> ColorSpace::device_rgb()
{
    static auto const space = std::make_shared<Rgb const>();
    return space;
}

std::shared_ptr<ColorSpace const> ColorSpace::device_cmyk()
{
    static auto const space = std::make_shared<Cmyk const>();
    return space;
}

void cmyk_to_rgb(float c, float m, float y, float k, float* rgb)
{
    // The sixteen corners of the CMYK cube as each prints on a press and
    // reads back in sRGB (inks over white paper), interpolated in all four
    // directions at once. The order is c m y k, each bit 1 for full ink.
    static constexpr float k_corners[16][3] = {
        { 1.0000f, 1.0000f, 1.0000f }, // paper
        { 0.1373f, 0.1216f, 0.1255f }, // k
        { 1.0000f, 0.9490f, 0.0000f }, // y
        { 0.1098f, 0.1020f, 0.0000f }, // y k
        { 0.9255f, 0.0000f, 0.5490f }, // m
        { 0.1412f, 0.0000f, 0.0000f }, // m k
        { 0.9294f, 0.1098f, 0.1412f }, // m y
        { 0.1333f, 0.0000f, 0.0000f }, // m y k
        { 0.0000f, 0.6784f, 0.9373f }, // c
        { 0.0000f, 0.0588f, 0.1412f }, // c k
        { 0.0000f, 0.6510f, 0.3137f }, // c y
        { 0.0000f, 0.0745f, 0.0000f }, // c y k
        { 0.1804f, 0.1922f, 0.5725f }, // c m
        { 0.0000f, 0.0000f, 0.0078f }, // c m k
        { 0.2118f, 0.2119f, 0.2235f }, // c m y
        { 0.0000f, 0.0000f, 0.0000f }, // c m y k
    };
    float const amounts[4] = { clamp01(c), clamp01(m), clamp01(y), clamp01(k) };
    float r = 0;
    float g = 0;
    float b = 0;
    for (int corner = 0; corner < 16; ++corner) {
        float weight = 1;
        for (int ink = 0; ink < 4; ++ink) {
            bool const full = (corner >> (3 - ink)) & 1;
            weight *= full ? amounts[ink] : 1 - amounts[ink];
        }
        if (weight == 0)
            continue;
        r += weight * k_corners[corner][0];
        g += weight * k_corners[corner][1];
        b += weight * k_corners[corner][2];
    }
    rgb[0] = clamp01(r);
    rgb[1] = clamp01(g);
    rgb[2] = clamp01(b);
}

class ColorSpaceParser {
public:
    // CalGray or CalRGB from its dictionary; null without a usable white point.
    static std::shared_ptr<ColorSpace const> calibrated(Document const& doc, Dict const& dict, int n)
    {
        auto const numbers = [&](char const* key, std::size_t count, double fallback) {
            std::array<double, 9> out {};
            out.fill(fallback);
            Object const value = doc.resolve(dict.get(key));
            if (count == 1 && value.is_number()) {
                out[0] = value.as_number(fallback);
            } else {
                Array const& list = value.as_array();
                if (list.size() == count) {
                    for (std::size_t i = 0; i < count; ++i)
                        out[i] = doc.resolve(list[i]).as_number(fallback);
                }
            }
            for (double& v : out)
                v = std::isfinite(v) ? v : fallback;
            return out;
        };
        auto const w = numbers("WhitePoint", 3, -1);
        icc::Vec3 const white { w[0], w[1], w[2] };
        if (!(white[0] > 0 && white[1] > 0 && white[2] > 0))
            return nullptr;
        auto const b = numbers("BlackPoint", 3, 0);
        icc::Vec3 black { std::max(0.0, b[0]), std::max(0.0, b[1]), std::max(0.0, b[2]) };
        auto const gamma = numbers("Gamma", static_cast<std::size_t>(n), 1);
        std::vector<icc::Curve> curves;
        for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i)
            curves.push_back(icc::Curve::gamma(gamma[i] > 0 ? gamma[i] : 1));
        // XYZ under the space's white: CalGray's is the white times A^G;
        // CalRGB's /Matrix lists the columns X Y Z of A, then B, then C.
        icc::Mat3 m {};
        if (n == 1) {
            m = { white[0], 0, 0, white[1], 0, 0, white[2], 0, 0 };
        } else {
            auto const matrix = numbers("Matrix", 9, 0);
            bool const given = doc.resolve(dict.get("Matrix")).as_array().size() == 9;
            for (std::size_t r = 0; r < 3; ++r) {
                for (std::size_t c = 0; c < 3; ++c)
                    m[r * 3 + c] = given ? matrix[c * 3 + r] : (r == c ? 1 : 0);
            }
        }
        icc::Mat3 const adapt = icc::bradford(white, icc::k_d50);
        auto space = std::make_shared<Calibrated>(n);
        space->transform = icc::Transform::shaper_matrix(std::move(curves), icc::multiply(adapt, m), icc::transform_by(adapt, black));
        if (!space->transform)
            return nullptr;
        return space;
    }

    static std::shared_ptr<ColorSpace const> parse(Document const& doc, Object const& object_in, Dict const* resources,
        int depth)
    {
        if (depth > k_max_depth)
            return nullptr;
        Object const object = doc.resolve(object_in);
        if (object.is_name()) {
            std::string const& name = object.as_name();
            // A bare CalGray or CalRGB has no white point: the device space.
            if (name == "DeviceGray" || name == "G" || name == "CalGray")
                return ColorSpace::device_gray();
            if (name == "DeviceRGB" || name == "RGB" || name == "CalRGB")
                return ColorSpace::device_rgb();
            if (name == "DeviceCMYK" || name == "CMYK")
                return ColorSpace::device_cmyk();
            if (name == "Pattern")
                return std::make_shared<PatternSpace const>();
            if (resources) {
                Dict const& spaces = doc.resolve(resources->get("ColorSpace")).as_dict();
                if (spaces.has(name)) {
                    // Default spaces may name themselves; never loop on a name.
                    return parse(doc, spaces.get(name), nullptr, depth + 1);
                }
            }
            return nullptr;
        }
        Array const& array = object.as_array();
        if (array.empty())
            return nullptr;
        std::string const family = doc.resolve(array[0]).as_name();
        if (array.size() == 1)
            return parse(doc, array[0], resources, depth + 1);
        if (family == "CalGray" || family == "CalRGB") {
            int const n = family == "CalGray" ? 1 : 3;
            if (auto space = calibrated(doc, doc.resolve(array[1]).as_dict(), n))
                return space;
            return n == 1 ? ColorSpace::device_gray() : ColorSpace::device_rgb();
        }
        if (family == "ICCBased") {
            Object const stream = doc.resolve(array[1]);
            Dict const& dict = stream.as_dict();
            std::int64_t const n = doc.resolve(dict.get("N")).as_int(0);
            std::shared_ptr<icc::Profile const> const profile = doc.icc_profile(array[1]);
            if (profile && profile->channels() == n && profile->transform(RenderingIntent::RelativeColorimetric)) {
                bool const lab = profile->data_space() == icc::DataSpace::Lab;
                auto space = std::make_shared<IccBased>(static_cast<int>(n), lab);
                space->profile = profile;
                if (lab) {
                    // A Lab profile's components are L*, a*, b* (§8.6.5.5 asks
                    // /Range to say so; without one, their whole range).
                    space->range_low = { 0, -128, -128 };
                    space->range_high = { 100, 127, 127 };
                }
                Array const& range = doc.resolve(dict.get("Range")).as_array();
                if (static_cast<std::int64_t>(range.size()) == 2 * n) {
                    for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
                        double const low = doc.resolve(range[2 * i]).as_number(0);
                        double const high = doc.resolve(range[2 * i + 1]).as_number(1);
                        if (std::isfinite(low) && std::isfinite(high) && low <= high) {
                            space->range_low[i] = static_cast<float>(low);
                            space->range_high[i] = static_cast<float>(high);
                        }
                    }
                }
                return space;
            }
            // A profile that cannot be used: the alternate, else the device space of N.
            if (dict.has("Alternate")) {
                auto alternate = parse(doc, dict.get("Alternate"), resources, depth + 1);
                if (alternate && alternate->components() == n)
                    return alternate;
            }
            if (n == 1)
                return ColorSpace::device_gray();
            if (n == 3)
                return ColorSpace::device_rgb();
            if (n == 4)
                return ColorSpace::device_cmyk();
            return nullptr;
        }
        if (family == "Lab") {
            auto lab = std::make_shared<Lab>();
            Dict const& dict = doc.resolve(array[1]).as_dict();
            Array const& white = doc.resolve(dict.get("WhitePoint")).as_array();
            if (white.size() == 3) {
                std::array<float, 3> w {};
                for (std::size_t i = 0; i < 3; ++i)
                    w[i] = static_cast<float>(doc.resolve(white[i]).as_number(static_cast<double>(lab->white[i])));
                if (w[0] > 0 && w[1] > 0 && w[2] > 0 && std::isfinite(w[0]) && std::isfinite(w[1]) && std::isfinite(w[2]))
                    lab->set_white(w);
            }
            Array const& range = doc.resolve(dict.get("Range")).as_array();
            if (range.size() == 4) {
                for (std::size_t i = 0; i < 4; ++i)
                    lab->range[i] = static_cast<float>(doc.resolve(range[i]).as_number(static_cast<double>(lab->range[i])));
            }
            return lab;
        }
        if (family == "Indexed" || family == "I") {
            if (array.size() < 4)
                return nullptr;
            auto indexed = std::make_shared<Indexed>();
            indexed->base = parse(doc, array[1], resources, depth + 1);
            if (!indexed->base || indexed->base->family() == ColorSpace::Family::Pattern
                || indexed->base->family() == ColorSpace::Family::Indexed)
                return nullptr;
            indexed->hival = std::clamp(static_cast<int>(doc.resolve(array[2]).as_int(0)), 0, 255);
            Object const lookup = doc.resolve(array[3]);
            std::vector<std::uint8_t> table;
            if (Stream const* stream = lookup.as_stream())
                table = doc.decode(*stream);
            else
                table.assign(lookup.as_string().begin(), lookup.as_string().end());
            int const n = indexed->base->components();
            indexed->rgb_table.resize(static_cast<std::size_t>(indexed->hival + 1) * 3);
            indexed->base_values.resize(static_cast<std::size_t>(indexed->hival + 1) * static_cast<std::size_t>(n));
            for (int i = 0; i <= indexed->hival; ++i) {
                float* const comps = indexed->base_values.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(n);
                for (int j = 0; j < n; ++j) {
                    std::size_t const at = static_cast<std::size_t>(i) * static_cast<std::size_t>(n) + static_cast<std::size_t>(j);
                    float low = 0;
                    float high = 1;
                    indexed->base->default_decode(8, j, low, high);
                    float const v = at < table.size() ? static_cast<float>(table[at]) / 255.0f : 0;
                    comps[j] = low + v * (high - low);
                }
                indexed->base->to_rgb(comps, &indexed->rgb_table[static_cast<std::size_t>(i) * 3]);
            }
            return indexed;
        }
        if (family == "Separation" || family == "DeviceN") {
            auto tinted = std::make_shared<Tinted>();
            tinted->m_family = family == "Separation" ? ColorSpace::Family::Separation : ColorSpace::Family::DeviceN;
            if (array.size() < 4)
                return nullptr;
            Object const names = doc.resolve(array[1]);
            if (family == "Separation") {
                tinted->m_components = 1;
                std::string const& colorant = names.as_name();
                tinted->all = colorant == "All";
                tinted->m_paints_nothing = colorant == "None";
            } else {
                Array const& list = names.as_array();
                if (list.empty() || list.size() > 32)
                    return nullptr;
                tinted->m_components = static_cast<int>(list.size());
                bool all_none = true;
                for (Object const& colorant : list)
                    all_none = all_none && doc.resolve(colorant).as_name() == "None";
                tinted->m_paints_nothing = all_none;
            }
            tinted->alternate = parse(doc, array[2], resources, depth + 1);
            tinted->transform = Function::parse(doc, array[3]);
            if (tinted->transform && tinted->transform->inputs() != tinted->m_components)
                tinted->transform = nullptr;
            return tinted;
        }
        if (family == "Pattern") {
            auto pattern = std::make_shared<PatternSpace>();
            pattern->m_base = parse(doc, array[1], resources, depth + 1);
            return pattern;
        }
        if (family == "DeviceGray" || family == "DeviceRGB" || family == "DeviceCMYK")
            return parse(doc, array[0], resources, depth + 1);
        return nullptr;
    }
};

std::shared_ptr<ColorSpace const> ColorSpace::parse(Document const& doc, Object const& object, Dict const* resources, int depth)
{
    return ColorSpaceParser::parse(doc, object, resources, depth);
}

std::shared_ptr<icc::Profile const> Document::icc_profile(Object const& stream_object) const
{
    // A file may name thousands of profiles: the first ones are kept.
    constexpr std::size_t k_kept = 64;
    auto const parse = [&] {
        Stream const* const stream = resolve(stream_object).as_stream();
        if (!stream)
            return std::shared_ptr<icc::Profile const> {};
        std::vector<std::uint8_t> const bytes = decode(*stream);
        return icc::Profile::parse(bytes);
    };
    if (!stream_object.is_ref())
        return parse();
    Ref const ref = stream_object.as_ref();
    {
        std::lock_guard const lock(m_icc_mutex);
        auto const it = m_icc_profiles.find(ref);
        if (it != m_icc_profiles.end())
            return it->second;
    }
    // Parsed outside the lock; the first one stored stands.
    std::shared_ptr<icc::Profile const> parsed = parse();
    std::lock_guard const lock(m_icc_mutex);
    if (m_icc_profiles.size() >= k_kept)
        return parsed;
    return m_icc_profiles.emplace(ref, std::move(parsed)).first->second;
}

}
