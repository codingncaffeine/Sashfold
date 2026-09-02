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
#include <unordered_map>
#include <vector>

namespace sashfold::text {

struct FontRequest {
    std::vector<std::string> families; // as written in CSS, generic names included; empty = the default
    int weight = 400;
    bool italic = false;
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

    // The first catalogued face with a glyph for the code point, loading
    // faces as needed and remembering the answer; null when none has it.
    Face const* fallback_for(char32_t code_point);

private:
    FontManager() = default;
    void scan();
    Face const* load(std::size_t catalogue_index);
    Face const* best_face(std::string const& family_lower, int weight, bool italic);

    bool m_system_fonts = true;
    bool m_scanned = false;
    std::vector<FaceInfo> m_catalogue;
    std::unordered_map<std::string, std::vector<std::size_t>> m_by_family; // lowercased
    std::unordered_map<std::size_t, std::unique_ptr<Face>> m_loaded; // catalogue index -> face (null: unreadable)
    std::unordered_map<std::string, std::unique_ptr<FontStack>> m_stacks;
    std::unordered_map<char32_t, Face const*> m_fallbacks;
};

}
