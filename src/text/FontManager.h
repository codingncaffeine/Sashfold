#pragma once

// FontManager: from a CSS font-family list, a weight and a style to the
// faces that render it. System fonts are catalogued from the OS directories
// on first use (naming tables only; a face loads when first needed),
// generic families resolve to whatever the machine has, and a fallback
// chain by cmap coverage ends at Sashfold Mono, so every code point draws
// as something honest. With system fonts off, every request is the
// built-in face alone: what the reference tests and shell goldens run with.

#include "text/Face.h"
#include "text/TrueType.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sashfold::text {

struct FontRequest {
    std::vector<std::string> families; // as written in CSS, generic names included; empty = the default
    int weight = 400;
    bool italic = false;
};

// A font a page brings along through @font-face: the family name it
// answers to, the weight and slant its descriptors claim, and the font
// file's bytes (TrueType outlines; anything else parses to nothing).
struct PageFont {
    std::string family;
    int weight = 400;
    bool italic = false;
    std::vector<std::uint8_t> bytes;
};

class FontManager;

// The faces answering one request, most preferred first, the built-in
// face last.
class FontStack {
public:
    Face const& primary() const { return *m_faces.front(); } // line metrics come from here
    std::vector<Face const*> const& faces() const { return m_faces; }
    // The first face with a glyph for the code point; then the manager's
    // fallback chain; then the built-in face, which draws the box.
    Face const& face_for(char32_t code_point) const;

    // A face and a glyph id to measure and draw: never "none" — when
    // nothing has the code point, the built-in face with the code point as
    // its glyph, which is how it draws the box.
    struct Glyph {
        Face const* face;
        std::uint32_t glyph;
    };
    Glyph glyph_for(char32_t code_point) const;
    // The advance of a string at a size, glyph by glyph.
    float measure(std::u32string_view text, float size) const;

private:
    friend class FontManager;
    FontManager* m_manager = nullptr;
    std::vector<Face const*> m_faces;
};

class FontManager {
public:
    static FontManager& instance();

    void set_system_fonts(bool enabled);
    bool system_fonts() const { return m_system_fonts; }

    // Stacks live as long as the manager; the same request returns the same stack.
    FontStack const& resolve(FontRequest const& request);

    // Every face the OS directories offer (scanned on first use).
    std::vector<FaceInfo> const& catalogue();
    // Adds one font file's faces to the catalogue, as if it were installed:
    // for tests.
    void add_font_file(std::string const& path);

    // The fonts the current page declared with @font-face, replacing the
    // last page's. They answer their family names ahead of the machine's
    // fonts, with system fonts on or off, and take no part in fallback.
    // A file is parsed once and its face kept for as long as the manager
    // lives, so the same font on the next page costs a lookup; the stacks
    // resolved against an earlier set stay valid for the layouts that
    // hold them.
    void set_page_fonts(std::vector<PageFont> const& fonts);
    std::size_t page_font_count() const { return m_page_faces.size(); }

    // The first catalogued face with a glyph for the code point, loading
    // faces as needed and remembering the answer; null when none has it.
    Face const* fallback_for(char32_t code_point);

private:
    struct PageFace {
        std::string family_lower;
        int weight;
        bool italic;
        Face const* face;
    };

    FontManager() = default;
    void scan();
    void retire_stacks();
    Face const* load(std::size_t catalogue_index);
    Face const* best_of(std::vector<std::size_t> const& indices, int weight, bool italic);
    Face const* best_face(std::string const& family_lower, int weight, bool italic);
    Face const* added_face(std::string const& family_lower, int weight, bool italic);
    Face const* page_face(std::string const& family_lower, int weight, bool italic) const;

    bool m_system_fonts = true;
    bool m_scanned = false;
    std::vector<FaceInfo> m_catalogue;
    std::unordered_map<std::string, std::vector<std::size_t>> m_by_family; // lowercased
    std::unordered_map<std::string, std::vector<std::size_t>> m_added_by_family; // the ones handed over by name
    std::unordered_map<std::size_t, std::unique_ptr<Face>> m_loaded; // catalogue index -> face (null: unreadable)
    std::unordered_map<std::string, std::unique_ptr<FontStack>> m_stacks;
    std::vector<std::unique_ptr<FontStack>> m_retired_stacks; // superseded, kept for the layouts holding them
    std::unordered_map<char32_t, Face const*> m_fallbacks;
    std::vector<PageFace> m_page_faces; // the current page's, in declaration order
    std::unordered_map<std::string, std::unique_ptr<Face>> m_page_face_cache; // by family, weight, slant, bytes
};

}
