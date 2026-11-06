#pragma once

// PDF's color spaces (ISO 32000-2 §8.6): the device spaces, CalGray and
// CalRGB, Lab, ICCBased (by its component count and alternate), Indexed,
// Separation, DeviceN and Pattern. Every space turns its components into
// sRGB for the screen. CMYK goes through the corners of the CMYK cube as
// printed on press and seen in sRGB, interpolated between them, so a
// process color looks as it does in other viewers rather than as the
// arithmetic complement.
//
// ⚠ ICC profiles are not applied: an ICCBased space is read as the device
// space with its number of components (or its /Alternate). Below what
// Chrome does (it runs the profile); owed in the plan.

#include "pdf/Function.h"
#include "pdf/Object.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sashfold::pdf {

class Document;

class ColorSpace {
public:
    enum class Family : std::uint8_t { Gray, RGB, CMYK, Lab, Indexed, Separation, DeviceN, Pattern };

    virtual ~ColorSpace() = default;
    ColorSpace() = default;
    ColorSpace(ColorSpace const&) = delete;
    ColorSpace& operator=(ColorSpace const&) = delete;

    Family family() const { return m_family; }
    int components() const { return m_components; }
    // sRGB in [0, 1] from components() values.
    virtual void to_rgb(float const* in, float* rgb) const = 0;
    // The color a space starts with when it is set (§8.6.8 table): black
    // for the device spaces, tint 1 for Separation/DeviceN, index 0.
    virtual void initial_color(float* out) const;
    // An image sample's default /Decode range for component i.
    virtual void default_decode(int bits_per_component, int index, float& low, float& high) const;
    // A Separation /None (or DeviceN of only None): paints nothing.
    bool paints_nothing() const { return m_paints_nothing; }

    // Pattern spaces: the space an uncolored pattern's color is in (may be null).
    std::shared_ptr<ColorSpace const> const& pattern_base() const { return m_base; }

    // From a name or an array; a name not of the device spaces is looked
    // up in the resources' /ColorSpace. Null when it cannot be read.
    static std::shared_ptr<ColorSpace const> parse(Document const& doc, Object const& object, Dict const* resources,
        int depth = 0);
    static std::shared_ptr<ColorSpace const> device_gray();
    static std::shared_ptr<ColorSpace const> device_rgb();
    static std::shared_ptr<ColorSpace const> device_cmyk();

protected:
    Family m_family = Family::Gray;
    int m_components = 1;
    bool m_paints_nothing = false;
    std::shared_ptr<ColorSpace const> m_base;

    friend class ColorSpaceParser;
};

// CMYK in [0, 1] to sRGB in [0, 1], the conversion every CMYK color takes.
void cmyk_to_rgb(float c, float m, float y, float k, float* rgb);

}
