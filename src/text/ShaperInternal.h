#pragma once

// What the script shapers share with the pipeline in Shaper.cpp: the
// shaping plan (the features a font answers for a script, language and
// direction, compiled to masks and lookups in stages), the plan builder a
// shaper adds its features to, and the hooks a shaper fills in. Not for
// use outside src/text.

#include "text/OpenTypeLayout.h"
#include "text/Shaper.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace sashfold::text {

class TrueTypeFont;

namespace shaping {

using otl::Buffer;
using otl::GlyphInfo;
using otl::GlyphPosition;

// The face as the shaper reads it.
struct ShapeFont {
    TrueTypeFont const& font;
    otl::LayoutTables const& tables;
    std::uint32_t nominal_glyph(char32_t) const;
    bool has_glyph(char32_t c) const { return nominal_glyph(c) != 0; }
    std::int32_t advance(std::uint32_t glyph) const;
};

struct Plan;
using PauseFunction = void (*)(Plan const&, ShapeFont const&, Buffer&);

namespace feature_flag {
inline constexpr std::uint32_t global = 0x01;
inline constexpr std::uint32_t has_fallback = 0x02; // kept even when the font lacks it: the shaper does it
inline constexpr std::uint32_t manual_zwnj = 0x04; // ZWNJ is not stepped over in its context matching
inline constexpr std::uint32_t manual_zwj = 0x08; // nor ZWJ in its input matching
inline constexpr std::uint32_t global_search = 0x10; // found among all the font's features, not only the language's
inline constexpr std::uint32_t random = 0x20;
inline constexpr std::uint32_t per_syllable = 0x40;
}

// Feature values a mask can hold at most: the 'rand' feature asks for this
// many, and an alternate lookup reading it picks at random.
inline constexpr std::uint32_t max_feature_value = 255;

class PlanBuilder {
public:
    struct Request {
        std::uint32_t tag = 0;
        std::uint32_t flags = 0;
        std::uint32_t max_value = 1;
        std::uint32_t default_value = 0;
        std::array<unsigned, 2> stage { 0, 0 };
        std::size_t seq = 0;
    };
    struct Pause {
        unsigned stage = 0;
        PauseFunction function = nullptr;
    };

    ScriptCode script = scripts::common;
    TextDirection direction = TextDirection::LeftToRight;
    std::array<std::uint32_t, 2> chosen_script { 0, 0 };
    std::array<bool, 2> found_script { false, false };

    void add_feature(std::uint32_t tag, std::uint32_t flags = 0, std::uint32_t value = 1);
    void enable_feature(std::uint32_t tag, std::uint32_t flags = 0, std::uint32_t value = 1)
    {
        add_feature(tag, flags | feature_flag::global, value);
    }
    void disable_feature(std::uint32_t tag) { add_feature(tag, feature_flag::global, 0); }
    void add_gsub_pause(PauseFunction function) { add_pause(0, function); }
    void add_gpos_pause(PauseFunction function) { add_pause(1, function); }

    std::vector<Request> requests;
    std::array<std::vector<Pause>, 2> pauses;
    std::array<unsigned, 2> current_stage { 0, 0 };

private:
    void add_pause(int table, PauseFunction function)
    {
        pauses[static_cast<std::size_t>(table)].push_back(Pause { current_stage[static_cast<std::size_t>(table)], function });
        ++current_stage[static_cast<std::size_t>(table)];
    }
};

enum class ZeroMarks : std::uint8_t {
    None,
    ByGdefEarly,
    ByGdefLate,
};

enum class Normalization : std::uint8_t {
    None,
    Decomposed,
    ComposedDiacritics,
    ComposedDiacriticsNoShortCircuit,
};

// What a script family's shaper adds to the pipeline.
struct ScriptShaper {
    void (*collect_features)(PlanBuilder&) = nullptr;
    void (*override_features)(PlanBuilder&) = nullptr;
    void (*init_plan)(Plan&) = nullptr;
    void (*preprocess_text)(Plan const&, ShapeFont const&, Buffer&) = nullptr;
    void (*postprocess_glyphs)(Plan const&, ShapeFont const&, Buffer&) = nullptr;
    Normalization normalization = Normalization::ComposedDiacritics;
    bool (*decompose)(Plan const&, char32_t ab, char32_t& a, char32_t& b) = nullptr;
    bool (*compose)(Plan const&, char32_t a, char32_t b, char32_t& ab) = nullptr;
    void (*setup_masks)(Plan const&, ShapeFont const&, Buffer&, std::u32string_view before, std::u32string_view after)
        = nullptr;
    void (*reorder_marks)(Plan const&, Buffer&, std::size_t start, std::size_t end) = nullptr;
    std::uint32_t gpos_tag = 0; // when set, GPOS applies only when the font has this script there
    ZeroMarks zero_marks = ZeroMarks::ByGdefLate;
    bool fallback_position = true;
};

extern ScriptShaper const default_shaper;
extern ScriptShaper const arabic_shaper;
extern ScriptShaper const hebrew_shaper;
extern ScriptShaper const thai_shaper;
extern ScriptShaper const hangul_shaper;

struct FeatureMap {
    std::uint32_t tag = 0;
    std::array<int, 2> index { -1, -1 };
    std::array<unsigned, 2> stage { 0, 0 };
    unsigned shift = 0;
    std::uint32_t mask = 0;
    std::uint32_t one_mask = 0; // the mask with value 1
    bool auto_zwnj = true;
    bool auto_zwj = true;
    bool random = false;
    bool per_syllable = false;
    bool needs_fallback = false; // asked for, not in the font: the shaper's fallback does it
};

struct LookupMap {
    std::uint16_t index = 0;
    std::uint32_t mask = 0;
    bool auto_zwnj = true;
    bool auto_zwj = true;
    bool random = false;
    bool per_syllable = false;
};

struct StageMap {
    std::size_t last_lookup = 0; // one past this stage's last lookup
    PauseFunction pause = nullptr;
};

struct Plan {
    ScriptShaper const* shaper = &default_shaper;
    ScriptCode script = scripts::common;
    TextDirection direction = TextDirection::LeftToRight;
    std::array<std::uint32_t, 2> chosen_script { 0, 0 };
    std::array<bool, 2> found_script { false, false };
    std::uint32_t global_mask = 1;
    std::vector<FeatureMap> features; // sorted by tag
    std::array<std::vector<LookupMap>, 2> lookups;
    std::array<std::vector<StageMap>, 2> stages;

    FeatureMap const* feature(std::uint32_t tag) const;
    std::uint32_t mask(std::uint32_t tag, unsigned* shift = nullptr) const
    {
        FeatureMap const* f = feature(tag);
        if (shift)
            *shift = f ? f->shift : 0;
        return f ? f->mask : 0;
    }
    std::uint32_t one_mask(std::uint32_t tag) const
    {
        FeatureMap const* f = feature(tag);
        return f ? f->one_mask : 0;
    }
    bool needs_fallback(std::uint32_t tag) const
    {
        FeatureMap const* f = feature(tag);
        return f && f->needs_fallback;
    }
    bool has_feature_in_font(int table, std::uint32_t tag) const
    {
        FeatureMap const* f = feature(tag);
        return f && f->index[static_cast<std::size_t>(table)] >= 0;
    }

    std::uint32_t frac_mask = 0;
    std::uint32_t numr_mask = 0;
    std::uint32_t dnom_mask = 0;
    std::uint32_t rtlm_mask = 0;
    std::uint32_t kern_mask = 0;
    bool has_frac = false;
    bool has_gpos_mark = false;
    bool apply_gpos = false;
    bool apply_kern = false;
    bool zero_marks = true;
    bool adjust_mark_positioning_when_zeroing = false;
    bool fallback_mark_positioning = false;
    bool fallback_glyph_classes = false;

    // A shaper's own: the Arabic joining forms' masks, the Hangul jamo
    // features' masks; and whether the Arabic fallback shaping runs.
    std::array<std::uint32_t, 8> shaper_masks {};
    bool shaper_fallback = false;
};

// Helpers the shapers share with the pipeline.
bool is_unicode_mark(GlyphInfo const&);
void set_unicode_props(GlyphInfo&); // category, combining class and flags from the code point
void set_glyph(GlyphInfo&, ShapeFont const&);

// The fallback shaping of Arabic from the presentation forms in the font's
// cmap, for fonts with no GSUB for the joining forms (ShaperArabic.cpp).
void arabic_fallback_shape(Plan const&, ShapeFont const&, Buffer&);

// Mark placement from glyph extents for fonts with no GPOS table
// (ShaperFallback.cpp): the combining classes recategorized first, the
// marks placed around their bases after positioning.
void fallback_recategorize_marks(Plan const&, Buffer&);
void fallback_position_marks(Plan const&, ShapeFont const&, Buffer&, bool adjust_offsets_when_zeroing);
// The `kern` table, applied the way positioning without GPOS kerning is.
void apply_kern_table(Plan const&, ShapeFont const&, Buffer&);

}

}
