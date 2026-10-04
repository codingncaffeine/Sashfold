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
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::text {

struct FontRequest {
    std::vector<std::string> families; // as written in CSS, generic names included; empty = the default
    int weight = 400;
    bool italic = false;
    int stretch = 100; // percent, as font-stretch
    // Whether the fonts a page brought along may answer. A page's text asks
    // with them; the browser's own words never do — a page that declares a
    // font by the name the window's words are set in must not get to draw
    // the address bar.
    bool page_fonts = true;
};

class Face;
class FontManager;
struct PageFontWait;

// What a page answers when a font it declared is first needed.
struct PageFontLoad {
    enum class Outcome : std::uint8_t {
        Loaded, // its bytes are here
        // Asked for and on its way: the text takes the next face it has,
        // and the page sets its fonts again once the font has come.
        Coming,
        Failed, // none of its sources gives a font: as if it were not declared
    };
    Outcome outcome = Outcome::Failed;
    std::vector<std::uint8_t> bytes;
    std::uint64_t bytes_hash = 0; // of the bytes, when the loader took it; else the manager does
};

// How a page fetches a font it declared, asked the first time text needs
// the font. A page whose fetcher is gone empties the function, and what
// still waits then stays waiting.
using PageFontLoader = std::function<PageFontLoad(PageFontWait const&)>;

// A font declared and not fetched: where it would come from, and who
// fetches it. One is shared by every copy of the page's font list, so
// what the manager learns of it — that it came, or cannot — is known to
// each of them.
struct PageFontWait {
    std::vector<std::string> urls; // its sources in the order to try them, each a URL in full
    std::shared_ptr<PageFontLoader> loader;
    // Asked for already and on its way: text takes its fallback, and the
    // loader is not asked again.
    bool coming = false;
    // The page has been asked and the font is among its faces.
    bool came() const { return settled && face; }

private:
    friend class FontManager;
    bool settled = false; // the loader has answered with the font, or that there is none
    Face const* face = nullptr; // the font, kept to its unicode-range; null for none
};

// A font a page brings along through @font-face: the family name it
// answers to, the weight, stretch and slant its descriptors claim, the
// code points its unicode-range keeps it to (none: every one), and the
// font file's bytes (TrueType, plain or wrapped as WOFF; anything else
// parses to nothing) — or, for a font nothing has needed yet, where they
// are to come from.
struct PageFont {
    std::string family;
    int weight = 400; // the low end of the weights it answers to
    bool italic = false;
    std::vector<std::uint8_t> bytes;
    int stretch = 100; // the low end of the stretches it answers to
    std::vector<std::pair<char32_t, char32_t>> unicode_ranges;
    int weight_max = 0; // the high ends; 0 means the low end alone
    int stretch_max = 0;
    // A hash of `bytes` (font_bytes_hash), taken once where the font was
    // collected, so that keying the face by its bytes does not read the
    // file again each time a page's fonts are set; 0 for not taken, and
    // the manager takes it then.
    std::uint64_t bytes_hash = 0;
    // Set for a font declared and not fetched; `bytes` is then empty.
    std::shared_ptr<PageFontWait> waiting = nullptr;
};

// Whether a page's fonts are fetched as text needs them (the default) or
// all of them as its sheets are read (SASHFOLD_LAZY=0).
bool page_fonts_on_demand();
// How many declared fonts have been fetched because text needed them, and
// how many times a loader was asked, since the program started.
struct PageFontCensus {
    std::size_t asked = 0;
    std::size_t loaded = 0;
    std::size_t coming = 0;
    std::size_t failed = 0;
};
PageFontCensus page_font_census();

// The hash a PageFont's bytes_hash holds.
std::uint64_t font_bytes_hash(std::vector<std::uint8_t> const& bytes);
// How many times a font's bytes have been hashed whole since the program
// started: what a test holds still while a page's fonts are collected
// again.
std::size_t font_bytes_hashed();

// The faces answering one request, most preferred first, the built-in
// face last. A face the page declared and has not fetched stands in its
// place without a glyph to its name until a code point in its range gets
// as far as it; the page is then asked for it, and where the page has it
// at hand the stack carries it from that moment on.
class FontStack {
public:
    // The first available font (css-fonts-4 §2.1): the first face of the
    // stack whose unicode-range has the space in it, and the built-in face
    // when none has. Line metrics and the ex and ch units come from here.
    Face const& primary() const
    {
        if (m_asks_primary) [[unlikely]]
            ask_primary();
        return *m_primary;
    }
    std::vector<Face const*> const& faces() const { return m_faces; }
    // Nothing answers but the built-in face, which is fixed pitch: a
    // width is then a count times one advance.
    bool builtin_alone() const { return m_faces.size() == 1 && m_waiting.empty(); }
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
    // The advance of a string at a size, glyph by glyph, each pair the same
    // face draws side by side kerned as its tables say — unless `kern` is
    // off (font-kerning: none).
    float measure(std::u32string_view text, float size, bool kern = true) const;

private:
    friend class FontManager;
    // A declared face not fetched, ahead of m_faces[before] in the order
    // the faces are asked in.
    struct Waiting {
        std::size_t before = 0;
        std::shared_ptr<PageFontWait> wait;
        std::vector<std::pair<char32_t, char32_t>> ranges; // none: every code point
        bool asked = false;
        bool covers(char32_t code_point) const;
    };
    void ask_for(char32_t code_point) const;
    void ask_primary() const;

    FontManager* m_manager = nullptr;
    std::vector<Face const*> m_faces;
    Face const* m_primary = nullptr;
    // What the manager changes when a waiting face comes, on the thread the
    // stack was resolved on — the one thread that lays out and paints with
    // it.
    std::vector<Waiting> m_waiting;
    std::size_t m_unasked = 0; // of m_waiting
    bool m_asks_primary = false; // a waiting face with the space in it stands ahead of the primary
    FontRequest m_request; // what it answers, to answer it again when a face comes
    void const* m_owner = nullptr; // the thread's fonts it was resolved against
    std::uint64_t m_serial = 0; // and which set of them
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
    // "The current page" is the page the calling thread is laying out: one
    // manager serves every page's thread, each page's fonts are its own
    // thread's, and so are the stacks resolved with them.
    // A file is parsed once and its face kept for as long as the manager
    // lives, so the same font on the next page costs a lookup; the stacks
    // resolved against an earlier set stay valid for the layouts that
    // hold them.
    // A font given waiting takes its place among the family's faces with
    // nothing fetched; the first text that gets as far as it has its page
    // asked for it (PageFontWait::loader).
    void set_page_fonts(std::vector<PageFont> const& fonts);
    // How many of the current page's fonts its text can be set in, and how
    // many more are declared and waiting — to be asked for, or to come.
    std::size_t page_font_count() const;
    std::size_t page_fonts_waiting() const;

    // The first catalogued face with a glyph for the code point, loading
    // faces as needed and remembering the answer; null when none has it.
    Face const* fallback_for(char32_t code_point);
    // The same among the colour faces alone — the bitmap and layered
    // fonts: what draws a smiling face as a picture.
    Face const* color_face_for(char32_t code_point);

    // One of the current page's fonts as the manager holds it.
    struct PageFace {
        std::string family_lower;
        int weight;
        bool italic;
        Face const* face; // kept to its unicode-range, when it has one; null while the font waits
        int stretch;
        int weight_max; // a face answering a range of weights or stretches
        int stretch_max;
        std::shared_ptr<PageFontWait> wait = nullptr; // for a font that waits
        std::vector<std::pair<char32_t, char32_t>> ranges = {}; // and the code points it is kept to
    };
    // The current page's fonts as they stand, and putting that set back: what
    // a layout of another document made meanwhile — a frame's, measured for
    // its scripts — brackets itself with.
    std::vector<PageFace> page_faces() const
    {
        std::lock_guard<std::recursive_mutex> const lock(m_mutex);
        return mine().page_faces;
    }
    void restore_page_faces(std::vector<PageFace> faces);

private:

    FontManager() = default;
    void scan();
    // What is a thread's own: the fonts of the page it is laying out, in
    // declaration order, and the stacks resolved with them — with the count
    // of the machine's faces those were resolved against, so that a font
    // added or the system fonts switched sets every thread's stacks aside
    // the next time that thread asks.
    struct ThreadFonts {
        std::vector<PageFace> page_faces;
        std::unordered_map<std::string, std::unique_ptr<FontStack>> stacks;
        std::uint64_t made_for = 0;
        std::uint64_t serial = 1; // moves when the stacks are set aside
    };
    ThreadFonts& mine() const; // the calling thread's; the caller holds the lock
    void retire_stacks(ThreadFonts&);
    friend class FontStack;
    // Asks the page for the waiting face at `index` of the stack's; true
    // when the stack is not what it was — the face came, or cannot — and
    // is to be walked again.
    bool ask(FontStack const& stack, std::size_t index);
    void fill(FontStack& stack, ThreadFonts& fonts);
    Face const* page_face_of(PageFace const& described, std::vector<std::uint8_t> const& bytes, std::uint64_t bytes_hash);
    // The entries of the set as their waits now stand: one whose font has
    // come carries it, one whose font cannot come is gone.
    static void settle_page_faces(std::vector<PageFace>& faces);
    static bool same_page_faces(std::vector<PageFace> const& left, std::vector<PageFace> const& right);
    Face const* load(std::size_t catalogue_index);
    Face const* best_of(std::vector<std::size_t> const& indices, int weight, int stretch, bool italic);
    Face const* best_face(std::string const& family_lower, int weight, int stretch, bool italic);
    Face const* added_face(std::string const& family_lower, int weight, int stretch, bool italic);
    // Every page face of the family that matches the request best — one,
    // or several when the family is split by unicode-range — in the
    // order declared; the waiting ones among them included.
    std::vector<PageFace const*> page_faces(std::string const& family_lower, int weight, int stretch, bool italic) const;
    Face const* ranged_face(Face const* face, std::vector<std::pair<char32_t, char32_t>> const& ranges);

    // One manager serves every document's thread — a test runner's
    // realms, a page and its frames — so every entry holds this lock; it is
    // recursive because the public entries call one another. What is
    // handed out (a stack, a face) lives as long as the manager and is
    // not changed once made, so it is read freely afterwards.
    mutable std::recursive_mutex m_mutex;
    bool m_system_fonts = true;
    bool m_scanned = false;
    std::vector<FaceInfo> m_catalogue;
    std::unordered_map<std::string, std::vector<std::size_t>> m_by_family; // lowercased
    std::unordered_map<std::string, std::vector<std::size_t>> m_added_by_family; // the ones handed over by name
    std::unordered_map<std::size_t, std::unique_ptr<Face>> m_loaded; // catalogue index -> face (null: unreadable)
    mutable std::unordered_map<std::thread::id, std::unique_ptr<ThreadFonts>> m_threads;
    std::uint64_t m_generation = 0; // moves when the faces every thread resolves against do
    std::vector<std::unique_ptr<FontStack>> m_retired_stacks; // superseded, kept for the layouts holding them
    std::unordered_map<char32_t, Face const*> m_fallbacks;
    std::unordered_map<char32_t, Face const*> m_color_fallbacks;
    std::unordered_map<std::string, std::unique_ptr<Face>> m_page_face_cache; // by family, weight, slant, bytes
    std::unordered_map<std::string, std::unique_ptr<Face>> m_ranged_faces; // a face kept to a unicode-range, by both
};

}
