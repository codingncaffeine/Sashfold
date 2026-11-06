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

    Lab()
    {
        m_family = Family::Lab;
        m_components = 3;
    }
    void to_rgb(float const* in, float* rgb) const override
    {
        float const l = std::clamp(in[0], 0.0f, 100.0f);
        float const a = std::clamp(in[1], range[0], range[1]);
        float const b = std::clamp(in[2], range[2], range[3]);
        float const m = (l + 16) / 116;
        float const fl = m + a / 500;
        float const fn = m - b / 200;
        auto const g = [](float x) { return x >= 6.0f / 29 ? x * x * x : 108.0f / 841 * (x - 4.0f / 29); };
        float const x = white[0] * g(fl);
        float const y = white[1] * g(m);
        float const z = white[2] * g(fn);
        // XYZ (scaled to D65 by the white point's ratio, a von Kries-style
        // adaptation on the diagonal) to linear sRGB, then the sRGB curve.
        float const xs = x * 0.9505f / white[0];
        float const ys = y * 1.0f / white[1];
        float const zs = z * 1.089f / white[2];
        float const lin[3] = {
            3.2406f * xs - 1.5372f * ys - 0.4986f * zs,
            -0.9689f * xs + 1.8758f * ys + 0.0415f * zs,
            0.0557f * xs - 0.2040f * ys + 1.0570f * zs,
        };
        for (int i = 0; i < 3; ++i) {
            float const v = clamp01(lin[i]);
            rgb[i] = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
        }
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

class Indexed final : public ColorSpace {
public:
    std::shared_ptr<ColorSpace const> base;
    int hival = 0;
    std::vector<float> rgb_table; // (hival + 1) * 3, the lookup already converted

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

    void to_rgb(float const* in, float* rgb) const override
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
            alternate->to_rgb(out.data(), rgb);
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
    static std::shared_ptr<ColorSpace const> parse(Document const& doc, Object const& object_in, Dict const* resources,
        int depth)
    {
        if (depth > k_max_depth)
            return nullptr;
        Object const object = doc.resolve(object_in);
        if (object.is_name()) {
            std::string const& name = object.as_name();
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
        if (family == "CalGray")
            return ColorSpace::device_gray();
        if (family == "CalRGB")
            return ColorSpace::device_rgb();
        if (family == "ICCBased") {
            Object const stream = doc.resolve(array[1]);
            Dict const& dict = stream.as_dict();
            std::int64_t const n = doc.resolve(dict.get("N")).as_int(0);
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
                for (std::size_t i = 0; i < 3; ++i)
                    lab->white[i] = static_cast<float>(doc.resolve(white[i]).as_number(static_cast<double>(lab->white[i])));
                if (lab->white[0] <= 0 || lab->white[1] <= 0 || lab->white[2] <= 0)
                    lab->white = { 0.9505f, 1.0f, 1.089f };
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
            std::array<float, 32> comps {};
            for (int i = 0; i <= indexed->hival; ++i) {
                for (int j = 0; j < n; ++j) {
                    std::size_t const at = static_cast<std::size_t>(i) * static_cast<std::size_t>(n) + static_cast<std::size_t>(j);
                    float low = 0;
                    float high = 1;
                    indexed->base->default_decode(8, j, low, high);
                    float const v = at < table.size() ? static_cast<float>(table[at]) / 255.0f : 0;
                    comps[static_cast<std::size_t>(j)] = low + v * (high - low);
                }
                indexed->base->to_rgb(comps.data(), &indexed->rgb_table[static_cast<std::size_t>(i) * 3]);
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

}
